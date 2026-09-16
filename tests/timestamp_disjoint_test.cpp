// Checks what SteamVR's CGraphicsDevice::GetDeltas(CompositorPresent) needs
// from DXMT: a TIMESTAMP_DISJOINT result that actually becomes available
// (S_OK), reports Disjoint == FALSE and a non-zero Frequency, plus two
// TIMESTAMP queries whose delta is a plausible GPU duration.
//
// SteamVR logs "Aborting GetDeltas(CompositorPresent) disjoint! (n=5)" after
// five consecutive bad results.
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <windows.h>

static LARGE_INTEGER g_freq;
static double now_ms(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

// Bounded poll, like SteamVR's "Read timestamps" scope.
static HRESULT poll(ID3D11DeviceContext *ctx, ID3D11Query *q, void *data,
                    UINT size, double budget_ms, double *waited) {
  double t0 = now_ms();
  for (;;) {
    HRESULT hr = ctx->GetData(q, data, size, 0);
    if (hr != S_FALSE) {
      *waited = now_ms() - t0;
      return hr;
    }
    if (now_ms() - t0 > budget_ms) {
      *waited = now_ms() - t0;
      return S_FALSE;
    }
  }
}

int main(void) {
  QueryPerformanceFrequency(&g_freq);

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "DxmtTsTest";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "DxmtTsTest", "ts", WS_OVERLAPPEDWINDOW, 100,
                              100, 320, 240, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  D3D_FEATURE_LEVEL fl;
  if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
    printf("device fail\n");
    return 1;
  }

  IDXGIDevice *dd;
  dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
  IDXGIAdapter *ad;
  dd->GetAdapter(&ad);
  IDXGIFactory2 *fac;
  ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fac);
  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 320; scd.Height = 240;
  scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain1 *sc;
  fac->CreateSwapChainForHwnd(dev, hwnd, &scd, NULL, NULL, &sc);
  ID3D11Texture2D *bb;
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
  ID3D11RenderTargetView *rtv;
  dev->CreateRenderTargetView(bb, NULL, &rtv);

  // staging readback, as the compositor does -- this is what turns on the
  // cross-submit event wait inside DXMT
  D3D11_TEXTURE2D_DESC sd = {};
  sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging;
  dev->CreateTexture2D(&sd, NULL, &staging);
  sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
  ID3D11Texture2D *src;
  dev->CreateTexture2D(&sd, NULL, &src);

  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
  ID3D11Query *disjoint;
  dev->CreateQuery(&qd, &disjoint);
  qd.Query = D3D11_QUERY_TIMESTAMP;
  ID3D11Query *t0q, *t1q;
  dev->CreateQuery(&qd, &t0q);
  dev->CreateQuery(&qd, &t1q);

  int bad_run = 0, worst_run = 0, good = 0, bad = 0;
  printf("== TIMESTAMP_DISJOINT / TIMESTAMP validity (SteamVR GetDeltas) ==\n");

  for (int f = 0; f < 120; f++) {
    float c[4] = {0, (f % 60) / 60.0f, 0, 1};
    ctx->ClearRenderTargetView(rtv, c);

    ctx->Begin(disjoint);
    ctx->End(t0q);
    D3D11_BOX box = {0, 0, 0, 1, 1, 1};
    ctx->CopySubresourceRegion(src, 0, 0, 0, 0, bb, 0, &box);
    ctx->CopyResource(staging, src);
    ctx->End(t1q);
    ctx->End(disjoint);

    sc->Present(0, 0);

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 a = 0, b = 0;
    double w1, w2, w3;
    HRESULT h1 = poll(ctx, disjoint, &dj, sizeof(dj), 500.0, &w1);
    HRESULT h2 = poll(ctx, t0q, &a, sizeof(a), 500.0, &w2);
    HRESULT h3 = poll(ctx, t1q, &b, sizeof(b), 500.0, &w3);

    bool ok = (h1 == S_OK) && (h2 == S_OK) && (h3 == S_OK) &&
              (dj.Disjoint == FALSE) && (dj.Frequency != 0) && (b >= a);
    if (ok) { good++; bad_run = 0; }
    else {
      bad++; bad_run++;
      if (bad_run > worst_run) worst_run = bad_run;
    }

    if (f < 5 || !ok) {
      double gpu_ms = dj.Frequency ? (double)(b - a) * 1000.0 / (double)dj.Frequency : -1;
      printf("f%-4d %s hr=(%08lx,%08lx,%08lx) Disjoint=%d Freq=%llu "
             "t0=%llu t1=%llu delta=%llu gpu=%.4f ms wait=(%.1f,%.1f,%.1f)ms\n",
             f, ok ? "ok  " : "BAD ", (unsigned long)h1, (unsigned long)h2,
             (unsigned long)h3, (int)dj.Disjoint,
             (unsigned long long)dj.Frequency, (unsigned long long)a,
             (unsigned long long)b, (unsigned long long)(b - a), gpu_ms, w1, w2, w3);
      fflush(stdout);
    }
  }

  printf("\nsummary: %d good, %d bad, longest consecutive bad run = %d\n", good,
         bad, worst_run);
  printf("SteamVR aborts GetDeltas at 5 consecutive bad results -> %s\n",
         worst_run >= 5 ? "WOULD ABORT" : "would not abort");
  return worst_run >= 5 ? 2 : 0;
}
