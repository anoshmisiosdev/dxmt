// Narrow down which ingredient makes D3D11_QUERY_EVENT never signal in DXMT.
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static LARGE_INTEGER g_freq;
static double now_ms(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *sc;
static ID3D11RenderTargetView *sc_rtv;
static ID3D11RenderTargetView *off_rtv;
static ID3D11Texture2D *staging, *readback_src, *offscreen;
static ID3D11Query *disjoint, *ts_begin, *ts_end;

// Returns ms spent spinning, or -1 if it never signalled within budget.
static double run_case(const char *name, bool use_swapchain, bool use_present,
                       bool use_timestamps, bool use_staging,
                       bool explicit_flush, bool donotflush) {
  ID3D11Query *q = NULL;
  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_EVENT;
  dev->CreateQuery(&qd, &q);

  float color[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  ctx->ClearRenderTargetView(use_swapchain ? sc_rtv : off_rtv, color);

  if (use_timestamps) {
    ctx->Begin(disjoint);
    ctx->End(ts_begin);
  }
  if (use_staging) {
    D3D11_BOX box = {0, 0, 0, 1, 1, 1};
    ctx->CopySubresourceRegion(readback_src, 0, 0, 0, 0,
                               use_swapchain ? (ID3D11Resource *)offscreen
                                             : (ID3D11Resource *)offscreen,
                               0, &box);
    ctx->CopyResource(staging, readback_src);
  }
  if (use_timestamps) {
    ctx->End(ts_end);
    ctx->End(disjoint);
  }

  ctx->End(q);

  if (use_present && sc)
    sc->Present(0, 0);
  if (explicit_flush)
    ctx->Flush();

  double t0 = now_ms();
  UINT done = 0;
  unsigned long long polls = 0;
  double result = -1;
  UINT flags = donotflush ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0;
  for (;;) {
    HRESULT hr = ctx->GetData(q, &done, 4, flags);
    polls++;
    if (hr != S_FALSE) {
      result = now_ms() - t0;
      break;
    }
    if (now_ms() - t0 > 3000.0)
      break;
  }
  printf("%-58s : %s  (%.2f ms, %llu polls)\n", name,
         result >= 0 ? "SIGNALLED" : "*** NEVER SIGNALLED ***",
         result >= 0 ? result : 3000.0, polls);
  fflush(stdout);
  q->Release();
  return result;
}

int main(void) {
  QueryPerformanceFrequency(&g_freq);

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "DxmtEvtMatrix";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "DxmtEvtMatrix", "matrix", WS_OVERLAPPEDWINDOW,
                              100, 100, 320, 240, NULL, NULL, wc.hInstance,
                              NULL);
  ShowWindow(hwnd, SW_SHOW);

  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL,
                                 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
  if (FAILED(hr)) { printf("device fail\n"); return 1; }

  IDXGIDevice *dd; dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
  IDXGIAdapter *ad; dd->GetAdapter(&ad);
  IDXGIFactory2 *fac; ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fac);

  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 320; scd.Height = 240;
  scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  fac->CreateSwapChainForHwnd(dev, hwnd, &scd, NULL, NULL, &sc);
  ID3D11Texture2D *bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
  dev->CreateRenderTargetView(bb, NULL, &sc_rtv);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
  dev->CreateTexture2D(&td, NULL, &offscreen);
  dev->CreateRenderTargetView(offscreen, NULL, &off_rtv);

  D3D11_TEXTURE2D_DESC sd = {};
  sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&sd, NULL, &staging);
  sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
  dev->CreateTexture2D(&sd, NULL, &readback_src);

  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; dev->CreateQuery(&qd, &disjoint);
  qd.Query = D3D11_QUERY_TIMESTAMP;
  dev->CreateQuery(&qd, &ts_begin); dev->CreateQuery(&qd, &ts_end);

  printf("== D3D11_QUERY_EVENT signalling matrix ==\n");
  printf("%-58s   %s\n", "case", "result");
  //         name                                  swap   present ts    staging flush  dnf
  run_case("1. offscreen clear only",              false, false, false, false, false, false);
  run_case("2. offscreen clear + explicit Flush",  false, false, false, false, true,  false);
  run_case("3. offscreen + timestamps",            false, false, true,  false, false, false);
  run_case("4. offscreen + staging copy",          false, false, false, true,  false, false);
  run_case("5. offscreen + ts + staging",          false, false, true,  true,  false, false);
  run_case("6. swapchain clear, no Present",       true,  false, false, false, false, false);
  run_case("7. swapchain clear + Present",         true,  true,  false, false, false, false);
  run_case("8. swapchain + Present + ts + staging",true,  true,  true,  true,  false, false);
  run_case("9. same as 8 + explicit Flush",        true,  true,  true,  true,  true,  false);
  run_case("10. same as 8 but DONOTFLUSH polling", true,  true,  true,  true,  false, true);
  run_case("11. offscreen clear only (repeat)",    false, false, false, false, false, false);
  printf("done\n");
  return 0;
}
