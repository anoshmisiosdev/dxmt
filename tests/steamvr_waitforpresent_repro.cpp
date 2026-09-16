// Reproducer for SteamVR 2.17.9 vrcompositor CGraphicsDevice::WaitForPresent
// hanging under DXMT.
//
// Reconstructed from the disassembly of vrcompositor.exe at 0x1400768c0:
//
//   WaitForPresent(...)
//     if (!present_pending) goto skip;
//     scope("Wait query")
//       // no NvAPI / LiquidVR / WinRT direct-mode object -> fallback path:
//       UINT done = 0;
//       do { hr = ctx->GetData(event_query, &done, 4, 0); }
//       while (hr == S_FALSE && done == 0);          // UNBOUNDED spin
//       ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m); // blocking read-back
//       ...read a byte...
//       ctx->Unmap(staging, 0);
//     scope("Read timestamps")
//       bounded spin on GetData(timestamp queries)
//
// Every step is timed and a stall threshold is reported so we can see which
// call actually blocks.

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

#define STEP_BEGIN() double _t0 = now_ms()
#define STEP_END(name)                                                         \
  do {                                                                         \
    double _el = now_ms() - _t0;                                               \
    if (_el > 20.0)                                                            \
      printf("    [SLOW] %-28s %8.2f ms\n", name, _el);                        \
    fflush(stdout);                                                            \
  } while (0)

static void logf_(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

int main(int argc, char **argv) {
  QueryPerformanceFrequency(&g_freq);

  int frames = (argc > 1) ? atoi(argv[1]) : 600;
  // Stall budget in ms before we declare the hang reproduced.
  double stall_budget = (argc > 2) ? atof(argv[2]) : 5000.0;

  logf_("== SteamVR WaitForPresent reproducer ==\n");
  logf_("frames=%d stall_budget=%.0f ms\n", frames, stall_budget);

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "DxmtWfpRepro";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "DxmtWfpRepro", "DXMT WaitForPresent repro",
                              WS_OVERLAPPEDWINDOW, 100, 100, 640, 480, NULL,
                              NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  ID3D11Device *dev = NULL;
  ID3D11DeviceContext *ctx = NULL;
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL,
                                 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
  if (FAILED(hr)) {
    logf_("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr);
    return 1;
  }
  logf_("device created, feature level 0x%x\n", (unsigned)fl);

  IDXGIDevice *dxgi_dev = NULL;
  dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
  IDXGIAdapter *adapter = NULL;
  dxgi_dev->GetAdapter(&adapter);
  IDXGIFactory2 *factory = NULL;
  adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);

  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 640;
  scd.Height = 480;
  scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // format 28, as SteamVR logs
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  IDXGISwapChain1 *sc = NULL;
  hr = factory->CreateSwapChainForHwnd(dev, hwnd, &scd, NULL, NULL, &sc);
  if (FAILED(hr)) {
    logf_("CreateSwapChainForHwnd failed 0x%08lx\n", (unsigned long)hr);
    return 1;
  }
  logf_("swapchain created\n");

  ID3D11Texture2D *backbuffer = NULL;
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backbuffer);
  ID3D11RenderTargetView *rtv = NULL;
  dev->CreateRenderTargetView(backbuffer, NULL, &rtv);

  // --- The objects SteamVR's WaitForPresent touches -------------------------
  // 1. an event query it spins on
  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_EVENT;
  ID3D11Query *event_query = NULL;
  hr = dev->CreateQuery(&qd, &event_query);
  logf_("CreateQuery(EVENT): 0x%08lx\n", (unsigned long)hr);

  // 2. timestamp disjoint + timestamp queries ("Read timestamps")
  qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
  ID3D11Query *disjoint = NULL;
  dev->CreateQuery(&qd, &disjoint);
  qd.Query = D3D11_QUERY_TIMESTAMP;
  ID3D11Query *ts_begin = NULL, *ts_end = NULL;
  dev->CreateQuery(&qd, &ts_begin);
  dev->CreateQuery(&qd, &ts_end);

  // 3. a 1x1 staging texture it Maps for READ after the query completes
  //    (SteamVR reads a single byte out of it and accumulates it)
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 1;
  td.Height = 1;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging = NULL;
  hr = dev->CreateTexture2D(&td, NULL, &staging);
  logf_("CreateTexture2D(STAGING/READ): 0x%08lx\n", (unsigned long)hr);

  // A 1x1 default texture we copy from the backbuffer region into, then
  // copy into staging -- same shape as the compositor's readback.
  td.Usage = D3D11_USAGE_DEFAULT;
  td.CPUAccessFlags = 0;
  ID3D11Texture2D *readback_src = NULL;
  dev->CreateTexture2D(&td, NULL, &readback_src);

  unsigned long long checksum = 0;
  int hung_at = -1;

  for (int f = 0; f < frames; f++) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }

    // ---- "render" ----
    float color[4] = {0.0f, (f % 60) / 60.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, color);

    ctx->Begin(disjoint);
    ctx->End(ts_begin);

    // copy 1 pixel of the backbuffer for readback
    D3D11_BOX box = {0, 0, 0, 1, 1, 1};
    ctx->CopySubresourceRegion(readback_src, 0, 0, 0, 0, backbuffer, 0, &box);
    ctx->CopyResource(staging, readback_src);

    ctx->End(ts_end);
    ctx->End(disjoint);

    // ---- this is what marks the present as pending ----
    ctx->End(event_query);

    // ---- Present ----
    {
      STEP_BEGIN();
      hr = sc->Present(0, 0);
      STEP_END("Present");
      if (FAILED(hr)) {
        logf_("frame %d: Present failed 0x%08lx\n", f, (unsigned long)hr);
        break;
      }
    }

    // ================= CGraphicsDevice::WaitForPresent =================
    // "Wait query": unbounded spin until the event query reports done.
    {
      double t0 = now_ms();
      UINT done = 0;
      unsigned long long polls = 0;
      for (;;) {
        hr = ctx->GetData(event_query, &done, 4, 0);
        polls++;
        if (hr != S_FALSE)
          break;
        if (done != 0)
          break;
        double el = now_ms() - t0;
        if (el > stall_budget) {
          logf_("\n*** HANG REPRODUCED ***\n");
          logf_("frame %d: GetData(EVENT) spun %.2f ms over %llu polls, "
                "still S_FALSE (done=%u)\n",
                f, el, polls, done);
          hung_at = f;
          break;
        }
      }
      if (hung_at >= 0)
        break;
      double el = now_ms() - t0;
      if (el > 20.0)
        logf_("    [SLOW] %-28s %8.2f ms (%llu polls)\n", "GetData(EVENT) spin",
              el, polls);
    }

    // blocking Map for READ on the staging texture
    {
      STEP_BEGIN();
      D3D11_MAPPED_SUBRESOURCE m = {};
      hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
      STEP_END("Map(STAGING, READ)");
      if (SUCCEEDED(hr)) {
        checksum += *(unsigned char *)m.pData;
        ctx->Unmap(staging, 0);
      } else {
        logf_("frame %d: Map failed 0x%08lx\n", f, (unsigned long)hr);
      }
    }

    // "Read timestamps": bounded spin
    {
      STEP_BEGIN();
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
      double t0 = now_ms();
      while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) {
        if (now_ms() - t0 > 1000.0)
          break;
      }
      UINT64 a = 0, b = 0;
      t0 = now_ms();
      while (ctx->GetData(ts_begin, &a, sizeof(a), 0) == S_FALSE) {
        if (now_ms() - t0 > 1000.0)
          break;
      }
      t0 = now_ms();
      while (ctx->GetData(ts_end, &b, sizeof(b), 0) == S_FALSE) {
        if (now_ms() - t0 > 1000.0)
          break;
      }
      checksum += (b - a);
      STEP_END("Read timestamps");
    }

    if ((f % 60) == 0)
      logf_("frame %d ok\n", f);
  }

  logf_("\nfinished: %s (checksum=%llu)\n",
        hung_at >= 0 ? "HUNG" : "no hang observed", checksum);
  return hung_at >= 0 ? 2 : 0;
}
