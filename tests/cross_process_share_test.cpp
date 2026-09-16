// Cross-process D3D11 shared-texture reproducer.
//
// Producer creates shared textures, fills each with a known NON-CONSTANT
// pattern, and hands the handles to a consumer process. The consumer opens
// each, copies to a staging texture, maps it and checks the pattern.
//
// The failure being chased is a completely uniform colour in SteamVR's
// compositor output, so the consumer explicitly distinguishes:
//   OK        - pattern matches
//   CONSTANT  - every texel identical (reports the colour) <- the suspected bug
//   MISMATCH  - varies, but not the pattern written
//
// Covers MISC_SHARED and MISC_SHARED_NTHANDLE, with and without
// SHARED_KEYEDMUTEX, sRGB and non-sRGB, and both a CPU fill
// (UpdateSubresource) and a GPU fill (ClearRenderTargetView + draw), since
// SteamVR's producer is a game rendering on the GPU.
//
//   cross_share.exe            -> runs producer, spawns consumer, prints both
//   cross_share.exe producer   -> producer only
//   cross_share.exe consumer   -> consumer only

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define TEX_W 64
#define TEX_H 64

static const char *kHandleFile = "C:\\temp\\share_handles.txt";
static const char *kReadyEvent = "Global\\dxmt_share_ready";
static const char *kDoneEvent = "Global\\dxmt_share_done";

struct Case {
  const char *name;
  UINT misc;
  DXGI_FORMAT format;
  bool gpu_fill; // GPU render instead of CPU UpdateSubresource
};

static Case g_cases[] = {
    {"SHARED           RGBA8       cpu", D3D11_RESOURCE_MISC_SHARED, DXGI_FORMAT_R8G8B8A8_UNORM, false},
    {"SHARED           RGBA8_SRGB  cpu", D3D11_RESOURCE_MISC_SHARED, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
    {"SHARED           RGBA8_SRGB  gpu", D3D11_RESOURCE_MISC_SHARED, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true},
    {"SHARED           BGRA8       cpu", D3D11_RESOURCE_MISC_SHARED, DXGI_FORMAT_B8G8R8A8_UNORM, false},
    {"NTHANDLE         RGBA8       cpu", D3D11_RESOURCE_MISC_SHARED_NTHANDLE, DXGI_FORMAT_R8G8B8A8_UNORM, false},
    {"NTHANDLE         RGBA8_SRGB  cpu", D3D11_RESOURCE_MISC_SHARED_NTHANDLE, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
    {"NTHANDLE|KEYED   RGBA8_SRGB  cpu",
     D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false},
    {"NTHANDLE|KEYED   RGBA8_SRGB  gpu",
     D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
     DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true},
    {"NTHANDLE|KEYED   RGBA8       gpu",
     D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
     DXGI_FORMAT_R8G8B8A8_UNORM, true},
};
static const int kNumCases = sizeof(g_cases) / sizeof(g_cases[0]);

// Deterministic pattern: a gradient that varies in both axes, plus a magic
// value in the first texel so a zeroed texture is obviously distinguishable.
static void make_pattern(unsigned char *px, int case_index) {
  for (int y = 0; y < TEX_H; y++) {
    for (int x = 0; x < TEX_W; x++) {
      unsigned char *p = px + (y * TEX_W + x) * 4;
      p[0] = (unsigned char)(x * 4 + case_index);
      p[1] = (unsigned char)(y * 4);
      p[2] = (unsigned char)((x ^ y) * 3);
      p[3] = 255;
    }
  }
  px[0] = 0xDE; px[1] = 0xAD; px[2] = 0xBE; px[3] = 0xEF;
}

static bool dev_create(ID3D11Device **dev, ID3D11DeviceContext **ctx) {
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL,
                                 0, D3D11_SDK_VERSION, dev, &fl, ctx);
  if (FAILED(hr)) {
    printf("D3D11CreateDevice failed 0x%08lx\n", (unsigned long)hr);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- producer
static int run_producer(void) {
  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  if (!dev_create(&dev, &ctx)) return 1;

  HANDLE ready = CreateEventA(NULL, TRUE, FALSE, kReadyEvent);
  HANDLE done = CreateEventA(NULL, TRUE, FALSE, kDoneEvent);
  ResetEvent(ready); ResetEvent(done);

  FILE *f = fopen(kHandleFile, "w");
  if (!f) { printf("[P] cannot write %s\n", kHandleFile); return 1; }

  unsigned char *pattern = (unsigned char *)malloc(TEX_W * TEX_H * 4);

  for (int i = 0; i < kNumCases; i++) {
    Case &c = g_cases[i];
    make_pattern(pattern, i);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = TEX_W; td.Height = TEX_H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = c.format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = c.misc;

    ID3D11Texture2D *tex = NULL;
    HRESULT hr = dev->CreateTexture2D(&td, NULL, &tex);
    if (FAILED(hr)) {
      printf("[P] %-34s CreateTexture2D FAILED 0x%08lx\n", c.name, (unsigned long)hr);
      fprintf(f, "%d FAIL 0 0 0 0\n", i);
      continue;
    }

    // ---- fill with the pattern ----
    unsigned expect_r = 0, expect_g = 0, expect_b = 0;
    if (c.gpu_fill) {
      // GPU write, like a game rendering into its shared eye texture.
      ID3D11RenderTargetView *rtv = NULL;
      hr = dev->CreateRenderTargetView(tex, NULL, &rtv);
      if (FAILED(hr)) {
        printf("[P] %-34s CreateRenderTargetView FAILED 0x%08lx\n", c.name, (unsigned long)hr);
      } else {
        // A clear gives a constant, which cannot distinguish "constant bug"
        // from success -- so clear, then overwrite a sub-rect with a second
        // colour via a second RTV clear on a scissor-like box using
        // CopySubresourceRegion from a CPU-filled source.
        float col[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        ctx->ClearRenderTargetView(rtv, col);
        rtv->Release();

        // stamp a distinct block so the result is non-constant
        D3D11_TEXTURE2D_DESC bd = td;
        bd.MiscFlags = 0;
        bd.BindFlags = 0;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.Width = 16; bd.Height = 16;
        ID3D11Texture2D *blk = NULL;
        if (SUCCEEDED(dev->CreateTexture2D(&bd, NULL, &blk))) {
          D3D11_MAPPED_SUBRESOURCE m = {};
          if (SUCCEEDED(ctx->Map(blk, 0, D3D11_MAP_WRITE, 0, &m))) {
            for (int y = 0; y < 16; y++) {
              unsigned char *row = (unsigned char *)m.pData + y * m.RowPitch;
              for (int x = 0; x < 16; x++) {
                row[x*4+0] = 0xDE; row[x*4+1] = 0xAD;
                row[x*4+2] = 0xBE; row[x*4+3] = 0xEF;
              }
            }
            ctx->Unmap(blk, 0);
            ctx->CopySubresourceRegion(tex, 0, 0, 0, 0, blk, 0, NULL);
          }
          blk->Release();
        }
        // 0.25/0.5/0.75 in UNORM8
        expect_r = 64; expect_g = 128; expect_b = 191;
      }
    } else {
      ctx->UpdateSubresource(tex, 0, NULL, pattern, TEX_W * 4, 0);
    }

    // ---- export the handle ----
    unsigned long long handle_val = 0;
    int is_nt = (c.misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) ? 1 : 0;
    char name[128];
    sprintf(name, "dxmt_share_tex_%d", i);

    if (is_nt) {
      IDXGIResource1 *res1 = NULL;
      hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1);
      if (SUCCEEDED(hr)) {
        WCHAR wname[128];
        MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 128);
        HANDLE nth = NULL;
        hr = res1->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                      wname, &nth);
        if (SUCCEEDED(hr)) handle_val = (unsigned long long)(uintptr_t)nth;
        res1->Release();
      }
      if (FAILED(hr)) {
        printf("[P] %-34s CreateSharedHandle FAILED 0x%08lx\n", c.name, (unsigned long)hr);
        fprintf(f, "%d FAIL 0 0 0 0\n", i);
        tex->Release();
        continue;
      }
    } else {
      IDXGIResource *res = NULL;
      hr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
      if (SUCCEEDED(hr)) {
        HANDLE sh = NULL;
        hr = res->GetSharedHandle(&sh);
        if (SUCCEEDED(hr)) handle_val = (unsigned long long)(uintptr_t)sh;
        res->Release();
      }
      if (FAILED(hr)) {
        printf("[P] %-34s GetSharedHandle FAILED 0x%08lx\n", c.name, (unsigned long)hr);
        fprintf(f, "%d FAIL 0 0 0 0\n", i);
        tex->Release();
        continue;
      }
    }

    // ---- keyed mutex handoff: producer releases key 1 to the consumer ----
    if (c.misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      IDXGIKeyedMutex *km = NULL;
      if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
        HRESULT a = km->AcquireSync(0, 1000);
        HRESULT r = km->ReleaseSync(1);
        printf("[P] %-34s keyed mutex Acquire(0)=0x%08lx Release(1)=0x%08lx\n",
               c.name, (unsigned long)a, (unsigned long)r);
        km->Release();
      } else {
        printf("[P] %-34s NO IDXGIKeyedMutex interface!\n", c.name);
      }
    }

    ctx->Flush();
    fprintf(f, "%d %s %llu %d %u %u %u\n", i, c.gpu_fill ? "GPU" : "CPU",
            handle_val, is_nt, expect_r, expect_g, expect_b);
    printf("[P] %-34s ready (handle=0x%llx)\n", c.name, handle_val);
    fflush(stdout);
    // keep tex alive for the consumer
  }

  fclose(f);
  SetEvent(ready);
  printf("[P] all textures published, waiting for consumer\n");
  fflush(stdout);
  WaitForSingleObject(done, 30000);
  printf("[P] done\n");
  return 0;
}

// ---------------------------------------------------------------- consumer
static int run_consumer(void) {
  HANDLE ready = CreateEventA(NULL, TRUE, FALSE, kReadyEvent);
  HANDLE done = CreateEventA(NULL, TRUE, FALSE, kDoneEvent);
  if (WaitForSingleObject(ready, 30000) != WAIT_OBJECT_0) {
    printf("[C] producer never signalled ready\n");
    return 1;
  }

  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  if (!dev_create(&dev, &ctx)) return 1;
  ID3D11Device1 *dev1 = NULL;
  dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&dev1);

  FILE *f = fopen(kHandleFile, "r");
  if (!f) { printf("[C] cannot read %s\n", kHandleFile); return 1; }

  unsigned char *pattern = (unsigned char *)malloc(TEX_W * TEX_H * 4);
  int failures = 0;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int idx = 0, is_nt = 0;
    unsigned long long hv = 0;
    unsigned er = 0, eg = 0, eb = 0;
    char kind[8] = {0};
    if (sscanf(line, "%d %7s %llu %d %u %u %u", &idx, kind, &hv, &is_nt, &er, &eg, &eb) < 2)
      continue;
    if (idx < 0 || idx >= kNumCases) continue;
    Case &c = g_cases[idx];
    if (strcmp(kind, "FAIL") == 0) {
      printf("[C] %-34s SKIPPED (producer failed)\n", c.name);
      failures++;
      continue;
    }

    ID3D11Texture2D *tex = NULL;
    HRESULT hr;
    if (is_nt) {
      char name[128]; sprintf(name, "dxmt_share_tex_%d", idx);
      WCHAR wname[128];
      MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 128);
      hr = dev1 ? dev1->OpenSharedResourceByName(wname, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                 __uuidof(ID3D11Texture2D), (void **)&tex)
                : E_NOINTERFACE;
      if (FAILED(hr) && dev1) {
        // fall back to the raw NT handle value
        hr = dev1->OpenSharedResource1((HANDLE)(uintptr_t)hv, __uuidof(ID3D11Texture2D), (void **)&tex);
      }
    } else {
      hr = dev->OpenSharedResource((HANDLE)(uintptr_t)hv, __uuidof(ID3D11Texture2D), (void **)&tex);
    }
    if (FAILED(hr) || !tex) {
      printf("[C] %-34s OPEN FAILED 0x%08lx\n", c.name, (unsigned long)hr);
      failures++;
      continue;
    }

    // keyed mutex: consumer acquires key 1 (what the producer released)
    IDXGIKeyedMutex *km = NULL;
    if (c.misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
      if (SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
        HRESULT a = km->AcquireSync(1, 2000);
        printf("[C] %-34s AcquireSync(1)=0x%08lx%s\n", c.name, (unsigned long)a,
               a == S_OK ? "" : "  <-- NOT ACQUIRED");
      }
    }

    D3D11_TEXTURE2D_DESC gd = {};
    tex->GetDesc(&gd);

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = gd.Width; sd.Height = gd.Height;
    sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = gd.Format; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    hr = dev->CreateTexture2D(&sd, NULL, &staging);
    if (FAILED(hr)) {
      printf("[C] %-34s staging CreateTexture2D FAILED 0x%08lx\n", c.name, (unsigned long)hr);
      failures++;
      if (km) { km->ReleaseSync(0); km->Release(); }
      tex->Release();
      continue;
    }

    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
      printf("[C] %-34s Map FAILED 0x%08lx\n", c.name, (unsigned long)hr);
      failures++;
      if (km) { km->ReleaseSync(0); km->Release(); }
      staging->Release(); tex->Release();
      continue;
    }

    // --- classify the result ---
    unsigned char *base = (unsigned char *)m.pData;
    unsigned char c0[4] = {base[0], base[1], base[2], base[3]};
    bool constant = true;
    for (int y = 0; y < (int)gd.Height && constant; y++) {
      unsigned char *row = base + y * m.RowPitch;
      for (int x = 0; x < (int)gd.Width; x++) {
        if (memcmp(row + x * 4, c0, 4) != 0) { constant = false; break; }
      }
    }

    const char *verdict;
    char detail[160] = {0};
    if (constant) {
      verdict = "*** CONSTANT ***";
      sprintf(detail, "every texel = (%u,%u,%u,%u)  #%02X%02X%02X",
              c0[0], c0[1], c0[2], c0[3], c0[0], c0[1], c0[2]);
    } else if (c.gpu_fill) {
      // expect the DEADBEEF block at 0,0 and the clear colour elsewhere
      unsigned char *far_px = base + 40 * m.RowPitch + 40 * 4;
      bool blk_ok = (base[0] == 0xDE && base[1] == 0xAD && base[2] == 0xBE);
      int dr = (int)far_px[0] - (int)er, dg = (int)far_px[1] - (int)eg, db = (int)far_px[2] - (int)eb;
      bool clr_ok = (dr > -3 && dr < 3) && (dg > -3 && dg < 3) && (db > -3 && db < 3);
      verdict = (blk_ok && clr_ok) ? "OK" : "MISMATCH";
      sprintf(detail, "block=(%u,%u,%u)%s clear=(%u,%u,%u) want(%u,%u,%u)%s",
              base[0], base[1], base[2], blk_ok ? "" : " BAD",
              far_px[0], far_px[1], far_px[2], er, eg, eb, clr_ok ? "" : " BAD");
    } else {
      make_pattern(pattern, idx);
      int bad = 0;
      for (int y = 0; y < (int)gd.Height; y++) {
        unsigned char *row = base + y * m.RowPitch;
        for (int x = 0; x < (int)gd.Width; x++) {
          unsigned char *want = pattern + (y * TEX_W + x) * 4;
          if (memcmp(row + x * 4, want, 3) != 0) bad++;
        }
      }
      verdict = bad ? "MISMATCH" : "OK";
      sprintf(detail, "%d/%d texels wrong; first=(%u,%u,%u) want(%u,%u,%u)", bad,
              (int)(gd.Width * gd.Height), base[0], base[1], base[2],
              pattern[0], pattern[1], pattern[2]);
    }
    if (strcmp(verdict, "OK") != 0) failures++;
    printf("[C] %-34s %-16s %s\n", c.name, verdict, detail);
    fflush(stdout);

    ctx->Unmap(staging, 0);
    if (km) { km->ReleaseSync(0); km->Release(); }
    staging->Release();
    tex->Release();
  }
  fclose(f);

  printf("[C] %d failing case(s)\n", failures);
  SetEvent(done);
  return failures ? 2 : 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "producer") == 0) return run_producer();
  if (argc > 1 && strcmp(argv[1], "consumer") == 0) return run_consumer();

  // default: spawn the consumer, then run the producer here
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
  printf("consumer exit=%lu\n", code);
  return rc ? rc : (int)code;
}
