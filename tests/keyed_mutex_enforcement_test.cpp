// Does DXMT's IDXGIKeyedMutex actually ENFORCE mutual exclusion and ordering
// across processes, or does it merely return S_OK?
//
// Content transfer through shared textures is already known to work, so this
// deliberately tests only the synchronisation contract:
//
//   T1 EXCLUSION   producer holds key 0; consumer's AcquireSync(0) must BLOCK
//                  and return WAIT_TIMEOUT. S_OK here means no exclusion.
//   T2 WRONG KEY   only key 1 was released; AcquireSync(99) must time out.
//   T3 HANDOFF     AcquireSync(1) after the producer released 1 must succeed.
//   T4 RETURN TRIP consumer releases key 2, producer re-acquires it. This is
//                  the pattern behind SteamVR's
//                  "WaitForAcquire timed out (FAILED)".
//   T5 ORDERING    bytes written before ReleaseSync must be visible after the
//                  matching AcquireSync.
//
// Also probes the underlying Wine D3DKMT keyed mutex entry points directly,
// since DXMT delegates all enforcement to them.
//
//   keyed_mutex.exe            -> runs both halves
//   keyed_mutex.exe producer|consumer

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define TEX_W 64
#define TEX_H 64

static const char *kTexName = "dxmt_km_tex";
// step gates
static const char *kEvProducerHolding = "Global\\dxmt_km_p_holding";
static const char *kEvConsumerDidT1   = "Global\\dxmt_km_c_t1";
static const char *kEvProducerRel1    = "Global\\dxmt_km_p_rel1";
static const char *kEvConsumerRel2    = "Global\\dxmt_km_c_rel2";
static const char *kEvAllDone         = "Global\\dxmt_km_done";

static const char *hrname(HRESULT hr) {
  switch ((unsigned long)hr) {
  case 0x00000000: return "S_OK";
  case 0x00000102: return "WAIT_TIMEOUT";
  case 0x00000080: return "WAIT_ABANDONED";
  case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
  case 0x80004005: return "E_FAIL";
  case 0x80070057: return "E_INVALIDARG";
  default: return "?";
  }
}

#define CHECK(cond, label)                                                     \
  do {                                                                         \
    printf("    %-14s %s\n", (cond) ? "[PASS]" : "[FAIL]", label);             \
    if (!(cond)) g_failures++;                                                 \
    fflush(stdout);                                                            \
  } while (0)

static int g_failures = 0;
static int g_gap_ms = 0;

// ------------------------------------------------- direct Wine D3DKMT probe
typedef struct { UINT64 unused; } DUMMY;
typedef NTSTATUS(WINAPI *PFN_CreateKeyedMutex)(void *);
typedef NTSTATUS(WINAPI *PFN_AcquireKeyedMutex)(void *);
typedef NTSTATUS(WINAPI *PFN_ReleaseKeyedMutex)(void *);

typedef struct {
  UINT64 Key;
  void *pPrivateRuntimeData;
  UINT PrivateRuntimeDataSize;
  UINT hSharedHandle;
  UINT hKeyedMutex;
} KMT_CREATE;

typedef struct {
  UINT hKeyedMutex;
  UINT64 Key;
  LARGE_INTEGER *pTimeout;
  UINT64 FenceValue;
} KMT_ACQUIRE;

typedef struct {
  UINT hKeyedMutex;
  UINT64 Key;
  UINT64 FenceValue;
} KMT_RELEASE;

static void probe_wine_d3dkmt(void) {
  HMODULE g = LoadLibraryA("gdi32.dll");
  PFN_CreateKeyedMutex c = (PFN_CreateKeyedMutex)GetProcAddress(g, "D3DKMTCreateKeyedMutex");
  PFN_AcquireKeyedMutex a = (PFN_AcquireKeyedMutex)GetProcAddress(g, "D3DKMTAcquireKeyedMutex");
  PFN_ReleaseKeyedMutex r = (PFN_ReleaseKeyedMutex)GetProcAddress(g, "D3DKMTReleaseKeyedMutex");
  printf("  gdi32 exports: Create=%p Acquire=%p Release=%p\n", (void *)c, (void *)a, (void *)r);
  if (!c || !a || !r) { printf("  -> entry points MISSING\n"); return; }

  KMT_CREATE cr = {};
  cr.Key = 0;
  NTSTATUS s = c(&cr);
  printf("  D3DKMTCreateKeyedMutex  -> 0x%08lx  hKeyedMutex=0x%x\n",
         (unsigned long)s, cr.hKeyedMutex);
  if (s != 0) {
    printf("  -> Wine does NOT implement D3DKMT keyed mutexes (status 0x%08lx)\n",
           (unsigned long)s);
    return;
  }
  // acquire twice without releasing: a real mutex must not allow this
  KMT_ACQUIRE ac = {}; ac.hKeyedMutex = cr.hKeyedMutex; ac.Key = 0;
  LARGE_INTEGER to; to.QuadPart = -5000000; // 500 ms
  ac.pTimeout = &to;
  NTSTATUS s1 = a(&ac);
  NTSTATUS s2 = a(&ac);
  printf("  D3DKMTAcquireKeyedMutex(0) first=0x%08lx second=0x%08lx%s\n",
         (unsigned long)s1, (unsigned long)s2,
         (s1 == 0 && s2 == 0) ? "   <-- DOUBLE ACQUIRE ALLOWED" : "");
  KMT_RELEASE rl = {}; rl.hKeyedMutex = cr.hKeyedMutex; rl.Key = 1;
  printf("  D3DKMTReleaseKeyedMutex(1) -> 0x%08lx\n", (unsigned long)r(&rl));
}

// ------------------------------------------------------------ device setup
static bool dev_create(ID3D11Device **dev, ID3D11DeviceContext **ctx) {
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL,
                                 0, D3D11_SDK_VERSION, dev, &fl, ctx);
  if (FAILED(hr)) { printf("D3D11CreateDevice 0x%08lx\n", (unsigned long)hr); return false; }
  return true;
}

static ID3D11Texture2D *make_shared_km_texture(ID3D11Device *dev) {
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = TEX_W; td.Height = TEX_H;
  td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // SteamVR's format (29)
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  ID3D11Texture2D *t = NULL;
  HRESULT hr = dev->CreateTexture2D(&td, NULL, &t);
  if (FAILED(hr)) { printf("CreateTexture2D(KEYEDMUTEX) 0x%08lx\n", (unsigned long)hr); return NULL; }
  return t;
}

// ---------------------------------------------------------------- producer
static int run_producer(void) {
  {
    char b[16] = {0};
    if (GetEnvironmentVariableA("KM_GAP", b, sizeof(b))) g_gap_ms = atoi(b);
  }
  printf("[P] KM_GAP = %d ms\n", g_gap_ms);
  printf("[P] === Wine D3DKMT keyed mutex probe ===\n");
  probe_wine_d3dkmt();

  ID3D11Device *dev; ID3D11DeviceContext *ctx;
  if (!dev_create(&dev, &ctx)) return 1;

  HANDLE evHold = CreateEventA(NULL, TRUE, FALSE, kEvProducerHolding);
  HANDLE evT1   = CreateEventA(NULL, TRUE, FALSE, kEvConsumerDidT1);
  HANDLE evRel1 = CreateEventA(NULL, TRUE, FALSE, kEvProducerRel1);
  HANDLE evRel2 = CreateEventA(NULL, TRUE, FALSE, kEvConsumerRel2);
  HANDLE evDone = CreateEventA(NULL, TRUE, FALSE, kEvAllDone);
  ResetEvent(evHold); ResetEvent(evT1); ResetEvent(evRel1);
  ResetEvent(evRel2); ResetEvent(evDone);

  ID3D11Texture2D *tex = make_shared_km_texture(dev);
  if (!tex) return 1;

  IDXGIResource1 *res1 = NULL;
  if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1))) {
    printf("[P] no IDXGIResource1\n"); return 1;
  }
  WCHAR wname[64];
  MultiByteToWideChar(CP_ACP, 0, kTexName, -1, wname, 64);
  HANDLE nth = NULL;
  HRESULT hr = res1->CreateSharedHandle(
      NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, wname, &nth);
  printf("[P] CreateSharedHandle -> 0x%08lx %s\n", (unsigned long)hr, hrname(hr));
  if (FAILED(hr)) return 1;

  IDXGIKeyedMutex *km = NULL;
  if (FAILED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
    printf("[P] no IDXGIKeyedMutex interface\n"); return 1;
  }

  // ---- T1: take key 0 and HOLD it while the consumer tries to acquire ----
  hr = km->AcquireSync(0, 1000);
  printf("[P] T1 AcquireSync(0) -> 0x%08lx %s  (now holding)\n",
         (unsigned long)hr, hrname(hr));
  SetEvent(evHold);

  // wait for the consumer to finish its blocked-acquire attempt
  WaitForSingleObject(evT1, 20000);

  // ---- T5 ordering: write a known payload while still holding ----
  {
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = TEX_W; sd.Height = TEX_H; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *up = NULL;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, NULL, &up))) {
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (SUCCEEDED(ctx->Map(up, 0, D3D11_MAP_WRITE, 0, &m))) {
        for (int y = 0; y < TEX_H; y++) {
          unsigned char *row = (unsigned char *)m.pData + y * m.RowPitch;
          for (int x = 0; x < TEX_W; x++) {
            row[x*4+0] = (unsigned char)(x * 4);
            row[x*4+1] = (unsigned char)(y * 4);
            row[x*4+2] = (unsigned char)((x ^ y) * 3);
            row[x*4+3] = 255;
          }
        }
        ((unsigned char *)m.pData)[0] = 0xDE;
        ((unsigned char *)m.pData)[1] = 0xAD;
        ((unsigned char *)m.pData)[2] = 0xBE;
        ctx->Unmap(up, 0);
        ctx->CopyResource(tex, up);
      }
      up->Release();
    }
    ctx->Flush();
  }

  // ---- release with key 1, handing the texture to the consumer ----
  hr = km->ReleaseSync(1);
  printf("[P] ReleaseSync(1) -> 0x%08lx %s\n", (unsigned long)hr, hrname(hr));
  SetEvent(evRel1);

  // ---- T4: the SteamVR "WaitForAcquire" pattern. The consumer will release
  //      key 2; we must be able to re-acquire it. ----
  DWORD t0 = GetTickCount();
  hr = km->AcquireSync(2, 3000);
  DWORD el = GetTickCount() - t0;
  printf("[P] T4 return-trip AcquireSync(2) -> 0x%08lx %s after %lu ms\n",
         (unsigned long)hr, hrname(hr), el);
  CHECK(hr == S_OK, "T4 return trip: producer re-acquires key released by consumer");

  WaitForSingleObject(evRel2, 20000);

  // ---- T6: strict alternating handoff, logged per iteration on both sides.
  //      P holds the mutex here (it acquired key 2 in T4).
  //      Each iteration:  P Release(3) -> C Acquire(3) -> C Release(4) -> P Acquire(4)
  {
    const int N = 20;
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    double worst = 0, total = 0;
    int ok_iters = 0, failed_at = -1;
    for (int i = 0; i < N; i++) {
      LARGE_INTEGER a, b;
      QueryPerformanceCounter(&a);
      HRESULT hr3 = km->ReleaseSync(3);
      // KM_GAP=N: wait N ms before acquiring, so the consumer's Release(4)
      // happens BEFORE this acquire blocks. Distinguishes "a release is lost"
      // from "an already-blocked waiter is never woken".
      if (g_gap_ms) Sleep(g_gap_ms);
      HRESULT h = km->AcquireSync(4, 2000);
      QueryPerformanceCounter(&b);
      double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
      if (i < 5 || h != S_OK)
        printf("[P] T6 iter %2d: Release(3)=%-12s Acquire(4)=%-12s %.3f ms\n", i,
               hrname(hr3), hrname(h), ms);
      if (FAILED(hr3) || h != S_OK) { failed_at = i; break; }
      ok_iters++; total += ms; if (ms > worst) worst = ms;
    }
    printf("[P] T6: %d/%d round trips ok, mean=%.3f ms worst=%.3f ms%s\n",
           ok_iters, N, ok_iters ? total / ok_iters : 0.0, worst,
           failed_at >= 0 ? "  <-- BROKE" : "");
    CHECK(ok_iters == N, "T6 sustained handoff: 20 consecutive round trips");
    CHECK(worst < 11.1, "T6 handoff latency fits a 90 Hz frame budget (11.1 ms)");
  }

  SetEvent(evDone);
  printf("[P] producer failures: %d\n", g_failures);
  return g_failures ? 2 : 0;
}

// ---------------------------------------------------------------- consumer
static int run_consumer(void) {
  HANDLE evHold = CreateEventA(NULL, TRUE, FALSE, kEvProducerHolding);
  HANDLE evT1   = CreateEventA(NULL, TRUE, FALSE, kEvConsumerDidT1);
  HANDLE evRel1 = CreateEventA(NULL, TRUE, FALSE, kEvProducerRel1);
  HANDLE evRel2 = CreateEventA(NULL, TRUE, FALSE, kEvConsumerRel2);

  ID3D11Device *dev; ID3D11DeviceContext *ctx;
  if (!dev_create(&dev, &ctx)) return 1;
  ID3D11Device1 *dev1 = NULL;
  dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);
  if (!dev1) { printf("[C] no ID3D11Device1\n"); return 1; }

  if (WaitForSingleObject(evHold, 20000) != WAIT_OBJECT_0) {
    printf("[C] producer never signalled\n"); return 1;
  }

  WCHAR wname[64];
  MultiByteToWideChar(CP_ACP, 0, kTexName, -1, wname, 64);
  ID3D11Texture2D *tex = NULL;
  HRESULT hr = dev1->OpenSharedResourceByName(
      wname, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
      __uuidof(ID3D11Texture2D), (void **)&tex);
  printf("[C] OpenSharedResourceByName -> 0x%08lx %s\n", (unsigned long)hr, hrname(hr));
  if (FAILED(hr)) return 1;

  IDXGIKeyedMutex *km = NULL;
  if (FAILED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
    printf("[C] no IDXGIKeyedMutex\n"); return 1;
  }

  // ---- T1: producer is HOLDING key 0. This must block then time out. ----
  DWORD t0 = GetTickCount();
  hr = km->AcquireSync(0, 800);
  DWORD el = GetTickCount() - t0;
  printf("[C] T1 AcquireSync(0) while producer holds -> 0x%08lx %s after %lu ms\n",
         (unsigned long)hr, hrname(hr), el);
  CHECK(hr != S_OK, "T1 mutual exclusion: acquire of a held key must not succeed");
  CHECK(el >= 600, "T1 blocking: acquire actually waited for the timeout");
  if (hr == S_OK) km->ReleaseSync(0); // don't leave it held
  SetEvent(evT1);

  // ---- T2: nobody released key 99 -> must time out ----
  WaitForSingleObject(evRel1, 20000);
  t0 = GetTickCount();
  hr = km->AcquireSync(99, 800);
  el = GetTickCount() - t0;
  printf("[C] T2 AcquireSync(99) (never released) -> 0x%08lx %s after %lu ms\n",
         (unsigned long)hr, hrname(hr), el);
  CHECK(hr != S_OK, "T2 key semantics: acquiring an unreleased key must fail");
  if (hr == S_OK) km->ReleaseSync(0);

  // ---- T3: the producer released key 1 -> must succeed ----
  t0 = GetTickCount();
  hr = km->AcquireSync(1, 3000);
  el = GetTickCount() - t0;
  printf("[C] T3 AcquireSync(1) -> 0x%08lx %s after %lu ms\n",
         (unsigned long)hr, hrname(hr), el);
  CHECK(hr == S_OK, "T3 handoff: acquire of the released key succeeds");

  // ---- T5: the payload written before ReleaseSync must be visible ----
  if (hr == S_OK) {
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = TEX_W; sd.Height = TEX_H; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = NULL;
    if (SUCCEEDED(dev->CreateTexture2D(&sd, NULL, &st))) {
      ctx->CopyResource(st, tex);
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        unsigned char *b = (unsigned char *)m.pData;
        bool magic = (b[0] == 0xDE && b[1] == 0xAD && b[2] == 0xBE);
        unsigned char *p = b + 20 * m.RowPitch + 30 * 4;
        bool grad = (p[0] == (unsigned char)(30 * 4) && p[1] == (unsigned char)(20 * 4));
        bool constant = true;
        unsigned char c0[4] = {b[0], b[1], b[2], b[3]};
        for (int y = 0; y < TEX_H && constant; y++) {
          unsigned char *row = b + y * m.RowPitch;
          for (int x = 0; x < TEX_W; x++)
            if (memcmp(row + x * 4, c0, 4) != 0) { constant = false; break; }
        }
        printf("[C] T5 payload: magic=%s gradient=%s constant=%s first=(%u,%u,%u)\n",
               magic ? "ok" : "BAD", grad ? "ok" : "BAD",
               constant ? "YES  <-- flat colour!" : "no", b[0], b[1], b[2]);
        CHECK(!constant, "T5 ordering: texture is not a single flat colour");
        CHECK(magic && grad, "T5 ordering: pre-release writes visible after acquire");
        ctx->Unmap(st, 0);
      }
      st->Release();
    }
  }

  // ---- hand back with key 2 so the producer's T4 can complete ----
  hr = km->ReleaseSync(2);
  printf("[C] ReleaseSync(2) -> 0x%08lx %s\n", (unsigned long)hr, hrname(hr));
  SetEvent(evRel2);

  // ---- T6 partner ----
  {
    const int N = 20;
    for (int i = 0; i < N; i++) {
      HRESULT h = km->AcquireSync(3, i == 0 ? 10000 : 2000);
      if (i < 5 || h != S_OK)
        printf("[C] T6 iter %2d: Acquire(3)=%s\n", i, hrname(h));
      if (h != S_OK) break;
      HRESULT hr4 = km->ReleaseSync(4);
      if (FAILED(hr4)) { printf("[C] T6 iter %2d: Release(4)=%s\n", i, hrname(hr4)); break; }
    }
  }

  printf("[C] consumer failures: %d\n", g_failures);
  return g_failures ? 2 : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "producer") == 0) return run_producer();
  if (argc > 1 && strcmp(argv[1], "consumer") == 0) return run_consumer();

  char self[MAX_PATH];
  GetModuleFileNameA(NULL, self, MAX_PATH);
  char cmd[MAX_PATH + 32];
  sprintf(cmd, "\"%s\" consumer", self);
  STARTUPINFOA si = {}; si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    printf("CreateProcess failed %lu\n", GetLastError());
    return 1;
  }
  int rc = run_producer();
  WaitForSingleObject(pi.hProcess, 40000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  printf("\n=== consumer exit=%lu, producer exit=%d ===\n", code, rc);
  return rc ? rc : (int)code;
}
