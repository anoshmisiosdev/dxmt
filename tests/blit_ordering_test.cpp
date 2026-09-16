// Regression guard for the cross-submit event wait change.
//
// That wait exists so a blit consumer sees results produced in an EARLIER
// command buffer. This drives exactly that pattern -- render, force a command
// buffer boundary with an event query, then copy the result out and read it
// back -- and verifies the pixels are the ones just written. If the event edge
// were lost, readback would return stale or uninitialised data.
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <windows.h>

int main(void) {
  ID3D11Device *dev;
  ID3D11DeviceContext *ctx;
  D3D_FEATURE_LEVEL fl;
  if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
    printf("device fail\n");
    return 1;
  }

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = 16; td.Height = 16; td.MipLevels = 1; td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *rt;
  dev->CreateTexture2D(&td, NULL, &rt);
  ID3D11RenderTargetView *rtv;
  dev->CreateRenderTargetView(rt, NULL, &rtv);

  D3D11_TEXTURE2D_DESC sd = td;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D *staging;
  dev->CreateTexture2D(&sd, NULL, &staging);

  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_EVENT;
  ID3D11Query *evt;
  dev->CreateQuery(&qd, &evt);
  qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
  ID3D11Query *dj;
  dev->CreateQuery(&qd, &dj);

  int fails = 0;
  for (int i = 0; i < 64; i++) {
    // A colour unique to this iteration, so stale data is detectable.
    UINT r = (i * 4) & 0xff, g = (i * 7) & 0xff, b = (i * 11) & 0xff;
    float c[4] = {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, c);

    // Force a command buffer boundary between producer and consumer.
    ctx->Begin(dj);
    ctx->End(dj);
    ctx->End(evt);
    ctx->Flush();

    // Consumer blit, in a later command buffer than the clear above.
    ctx->CopyResource(staging, rt);

    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) {
      printf("iter %d: Map failed 0x%08lx\n", i, (unsigned long)hr);
      fails++;
      continue;
    }
    unsigned char *p = (unsigned char *)m.pData;
    // allow +/-1 for unorm rounding
    int dr = (int)p[0] - (int)r, dg = (int)p[1] - (int)g, db = (int)p[2] - (int)b;
    if (dr < -1 || dr > 1 || dg < -1 || dg > 1 || db < -1 || db > 1) {
      printf("iter %d: STALE/WRONG readback: got (%u,%u,%u) want (%u,%u,%u)\n",
             i, p[0], p[1], p[2], r, g, b);
      fails++;
    }
    ctx->Unmap(staging, 0);
  }

  printf("blit ordering across command buffers: %s (%d/64 bad)\n",
         fails ? "FAIL" : "PASS", fails);
  return fails ? 2 : 0;
}
