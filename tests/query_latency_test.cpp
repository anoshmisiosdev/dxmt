// How long after End() does a SINGLE GetData poll start returning S_OK?
//
// A spin loop hides this, because DXMT's CheckEventState escalates to
// EventState::Stall after kEventStallThreshold (64) polls of the SAME query
// and forces a flush. An app that polls each query once per frame -- the
// normal way to read GPU timing without stalling -- never reaches that
// escalation, so this measures the honest latency.
//
// On real D3D11 a query issued and then followed by several Presents and tens
// of milliseconds is unambiguously ready.

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <windows.h>

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *sc;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *bb, *staging, *src;

static void do_work(int f) {
  float c[4] = {0.2f, (f % 60) / 60.0f, 0.4f, 1.0f};
  ctx->ClearRenderTargetView(rtv, c);
  D3D11_BOX box = {0, 0, 0, 1, 1, 1};
  ctx->CopySubresourceRegion(src, 0, 0, 0, 0, bb, 0, &box);
  ctx->CopyResource(staging, src);
}

// Issue one query, then Present/sleep, polling ONCE per iteration.
// Reports which iteration first returned S_OK.
static void measure(const char *label, D3D11_QUERY type, int presents,
                    int sleep_ms, bool donotflush) {
  D3D11_QUERY_DESC qd = {};
  qd.Query = type;
  ID3D11Query *q = NULL;
  if (FAILED(dev->CreateQuery(&qd, &q))) { printf("  %s: CreateQuery failed\n", label); return; }

  do_work(0);
  if (type == D3D11_QUERY_TIMESTAMP_DISJOINT) ctx->Begin(q);
  ctx->End(q);
  sc->Present(0, 0);

  UINT flags = donotflush ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0;
  int first_ok = -1;
  DWORD t0 = GetTickCount();
  for (int i = 0; i < presents; i++) {
    if (sleep_ms) Sleep(sleep_ms);
    // one poll only -- no spin
    union { BOOL b; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 u; } data = {};
    UINT size = (type == D3D11_QUERY_TIMESTAMP_DISJOINT)
                    ? (UINT)sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)
                    : (type == D3D11_QUERY_TIMESTAMP ? (UINT)sizeof(UINT64) : (UINT)sizeof(BOOL));
    HRESULT hr = ctx->GetData(q, &data, size, flags);
    if (hr == S_OK && first_ok < 0) { first_ok = i; break; }
    // keep the pipeline moving, as a real app would
    do_work(i + 1);
    sc->Present(0, 0);
  }
  DWORD el = GetTickCount() - t0;
  if (first_ok >= 0)
    printf("  %-46s READY after %d present(s), %lu ms\n", label, first_ok + 1, el);
  else
    printf("  %-46s *** NEVER READY *** after %d presents / %lu ms\n", label,
           presents, el);
  q->Release();
}

// Control: the same query, but polled in a spin loop (what DXMT's stall
// escalation is tuned for).
static void measure_spin(const char *label, D3D11_QUERY type) {
  D3D11_QUERY_DESC qd = {};
  qd.Query = type;
  ID3D11Query *q = NULL;
  dev->CreateQuery(&qd, &q);
  do_work(0);
  if (type == D3D11_QUERY_TIMESTAMP_DISJOINT) ctx->Begin(q);
  ctx->End(q);
  sc->Present(0, 0);

  DWORD t0 = GetTickCount();
  long polls = 0;
  bool ok = false;
  union { BOOL b; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj; UINT64 u; } data = {};
  UINT size = (type == D3D11_QUERY_TIMESTAMP_DISJOINT)
                  ? (UINT)sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT)
                  : (UINT)sizeof(BOOL);
  while (GetTickCount() - t0 < 2000) {
    polls++;
    if (ctx->GetData(q, &data, size, 0) == S_OK) { ok = true; break; }
  }
  printf("  %-46s %s after %ld polls, %lu ms\n", label,
         ok ? "READY" : "*** NEVER READY ***", polls, GetTickCount() - t0);
  q->Release();
}

int main(void) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.lpszClassName = "DxmtQLat";
  RegisterClassA(&wc);
  HWND hwnd = CreateWindowExA(0, "DxmtQLat", "qlat", WS_OVERLAPPEDWINDOW, 100,
                              100, 400, 300, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(hwnd, SW_SHOW);

  D3D_FEATURE_LEVEL fl;
  if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
    printf("device fail\n"); return 1;
  }
  IDXGIDevice *dd; dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd);
  IDXGIAdapter *ad; dd->GetAdapter(&ad);
  IDXGIFactory2 *fac; ad->GetParent(__uuidof(IDXGIFactory2), (void **)&fac);
  DXGI_SWAP_CHAIN_DESC1 scd = {};
  scd.Width = 400; scd.Height = 300;
  scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.BufferCount = 2;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  fac->CreateSwapChainForHwnd(dev, hwnd, &scd, NULL, NULL, &sc);
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
  dev->CreateRenderTargetView(bb, NULL, &rtv);

  D3D11_TEXTURE2D_DESC sd = {};
  sd.Width = 1; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&sd, NULL, &staging);
  sd.Usage = D3D11_USAGE_DEFAULT; sd.CPUAccessFlags = 0;
  dev->CreateTexture2D(&sd, NULL, &src);

  printf("== single-poll query readiness (no spin) ==\n");
  measure("EVENT            1 poll/frame, flush allowed", D3D11_QUERY_EVENT, 30, 16, false);
  measure("EVENT            1 poll/frame, DONOTFLUSH   ", D3D11_QUERY_EVENT, 30, 16, true);
  measure("TIMESTAMP_DISJOINT 1 poll/frame, flush      ", D3D11_QUERY_TIMESTAMP_DISJOINT, 30, 16, false);
  measure("TIMESTAMP_DISJOINT 1 poll/frame, DONOTFLUSH ", D3D11_QUERY_TIMESTAMP_DISJOINT, 30, 16, true);

  printf("\n== control: same queries, spin-polled ==\n");
  measure_spin("EVENT              spin", D3D11_QUERY_EVENT);
  measure_spin("TIMESTAMP_DISJOINT spin", D3D11_QUERY_TIMESTAMP_DISJOINT);
  return 0;
}
