// A real compositor does not read its GPU timing queries immediately -- it
// keeps a ring of query sets, issues frame N's, and reads frame N-k's so the
// GPU is never stalled. That is a materially different path from reading back
// in the same frame, so this exercises it directly, with several timestamp
// pairs per frame like a compositor profiling multiple stages.
//
// Reports the same verdict SteamVR's GetDeltas uses: Disjoint flag, non-zero
// Frequency, and a monotonically sensible delta.

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <windows.h>

#define RING 4      // query sets in flight
#define STAGES 4    // max timestamp pairs per frame
static int g_stages = STAGES;
static int g_sleep = 0;
#define FRAMES 240

struct QuerySet {
  ID3D11Query *disjoint;
  ID3D11Query *ts[STAGES + 1];
  bool issued;
};

static UINT g_flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;

int main(int argc, char **argv) {
  int lag = (argc > 1) ? atoi(argv[1]) : 2; // read frame N-lag
  if (lag < 1) lag = 1;
  if (lag >= RING) lag = RING - 1;
  // TSPIPE_FLUSH=1 in the environment allows GetData to flush
  {
    char buf[16] = {0};
    if (GetEnvironmentVariableA("TSPIPE_FLUSH", buf, sizeof(buf)) && buf[0] == 0x31)
      g_flags = 0;
  }
  {
    char b[16] = {0};
    if (GetEnvironmentVariableA("TSPIPE_STAGES", b, sizeof(b))) {
      g_stages = atoi(b);
      if (g_stages < 0) g_stages = 0;
      if (g_stages > STAGES) g_stages = STAGES;
    }
  }
  {
    char b[16] = {0};
    if (GetEnvironmentVariableA("TSPIPE_SLEEP", b, sizeof(b))) g_sleep = atoi(b);
  }
  printf("sleep = %d ms\n", g_sleep);
  printf("flags = %s, stages = %d\n", g_flags ? "DONOTFLUSH" : "0 (allow flush)", g_stages);

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "DxmtTsPipe";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "DxmtTsPipe", "ts pipe", WS_OVERLAPPEDWINDOW,
                              100, 100, 512, 384, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  D3D_FEATURE_LEVEL fl;
  if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
    printf("device fail\n"); return 1;
  }

  IDXGIDevice *dd; dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
  IDXGIAdapter *ad; dd->GetAdapter(&ad);
  IDXGIFactory2 *fac; ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fac);
  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 512; scd.Height = 384;
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

  // staging readback, as the compositor does
  D3D11_TEXTURE2D_DESC sd = {};
  sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging; dev->CreateTexture2D(&sd, NULL, &staging);
  sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
  ID3D11Texture2D *src; dev->CreateTexture2D(&sd, NULL, &src);

  QuerySet ring[RING] = {};
  D3D11_QUERY_DESC qd = {};
  for (int i = 0; i < RING; i++) {
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    dev->CreateQuery(&qd, &ring[i].disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (int s = 0; s <= STAGES; s++) dev->CreateQuery(&qd, &ring[i].ts[s]);
  }

  int good = 0, bad = 0, bad_run = 0, worst = 0, not_ready = 0;
  printf("== pipelined GPU timing queries (ring=%d, stages=%d, lag=%d) ==\n",
         RING, STAGES, lag);

  for (int f = 0; f < FRAMES; f++) {
    QuerySet &cur = ring[f % RING];

    // ---- issue this frame's queries ----
    ctx->Begin(cur.disjoint);
    ctx->End(cur.ts[0]);
    for (int s = 0; s < g_stages; s++) {
      float c[4] = {(s * 0.2f), (f % 60) / 60.0f, 0.5f, 1.0f};
      ctx->ClearRenderTargetView(rtv, c);
      D3D11_BOX box = {0, 0, 0, 1, 1, 1};
      ctx->CopySubresourceRegion(src, 0, 0, 0, 0, bb, 0, &box);
      ctx->CopyResource(staging, src);
      ctx->End(cur.ts[s + 1]);
    }
    ctx->End(cur.disjoint);
    cur.issued = true;

    sc->Present(0, 0);
    if (g_sleep) Sleep(g_sleep);

    // ---- read the set issued `lag` frames ago, non-blocking ----
    int ri = (f - lag + RING * 4) % RING;
    if (f >= lag && ring[ri].issued) {
      QuerySet &old = ring[ri];
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
      HRESULT hd = ctx->GetData(old.disjoint, &dj, sizeof(dj), g_flags);
      if (f < lag + 6)
        printf("f%-4d read ring[%d] disjointHr=0x%08lx size=%d\n", f, ri,
               (unsigned long)hd, (int)sizeof(dj));
      if (hd == S_FALSE) { not_ready++; continue; }

      UINT64 t[STAGES + 1] = {};
      bool all = (hd == S_OK);
      for (int s = 0; s <= g_stages; s++) {
        HRESULT h = ctx->GetData(old.ts[s], &t[s], sizeof(UINT64), g_flags);
        if (h != S_OK) all = false;
      }
      bool mono = true;
      for (int s = 1; s <= g_stages; s++) if (t[s] < t[s - 1]) mono = false;
      bool ok = all && !dj.Disjoint && dj.Frequency != 0 && mono && t[0] != 0;

      if (ok) { good++; bad_run = 0; }
      else {
        bad++; bad_run++; if (bad_run > worst) worst = bad_run;
        if (bad <= 6)
          printf("f%-4d BAD disjointHr=%08lx Disjoint=%d Freq=%llu allTs=%d "
                 "mono=%d t0=%llu tN=%llu\n",
                 f, (unsigned long)hd, (int)dj.Disjoint,
                 (unsigned long long)dj.Frequency, (int)all, (int)mono,
                 (unsigned long long)t[0], (unsigned long long)t[STAGES]);
      }
      if (good == 1) {
        double ms = dj.Frequency ? (double)(t[STAGES] - t[0]) * 1000.0 / dj.Frequency : -1;
        printf("f%-4d first good: Disjoint=%d Freq=%llu span=%.4f ms\n", f,
               (int)dj.Disjoint, (unsigned long long)dj.Frequency, ms);
      }
    }
  }

  printf("\nsummary: %d good, %d bad, %d not-ready-skipped, longest bad run=%d\n",
         good, bad, not_ready, worst);
  printf("verdict: %s\n", worst >= 5 ? "WOULD ABORT (n=5)" : "healthy");
  return worst >= 5 ? 2 : 0;
}
