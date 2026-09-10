#pragma once
#include "com/com_guid.hpp"
#include "d3d11.h"
#include "d3d11_3.h" // OpenXR interop: D3D11_TEXTURE2D_DESC1 / ID3D11Fence

namespace dxmt {
class ArgumentEncodingContext;
}

struct MTL_TEMPORAL_UPSCALE_D3D11_DESC {
  UINT InputContentWidth; // can be 0, which means full width
  UINT InputContentHeight; // can be 0, which means full height
  BOOL AutoExposure;
  BOOL InReset;
  BOOL DepthReversed;
  BOOL MotionVectorInDisplayRes;
  ID3D11Resource *Color;
  ID3D11Resource *Depth;
  ID3D11Resource *MotionVector;
  ID3D11Resource *Output;
  FLOAT MotionVectorScaleX;
  FLOAT MotionVectorScaleY;
  FLOAT PreExposure;
  ID3D11Resource *ExposureTexture;
  FLOAT JitterOffsetX;
  FLOAT JitterOffsetY;
};

DEFINE_COM_INTERFACE("43ace3ce-1956-448b-a4eb-aee68bdeb283",
                     IMTLD3D11ContextExt)
    : public IUnknown{
   virtual void STDMETHODCALLTYPE TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D11_DESC *pDesc) = 0;
   virtual void STDMETHODCALLTYPE BeginUAVOverlap() = 0;
   virtual void STDMETHODCALLTYPE EndUAVOverlap() = 0;
};

typedef enum MTL_FEATURE {
  MTL_FEATURE_METALFX_TEMPORAL_SCALER = 0,
} MTL_FEATURE;

DEFINE_COM_INTERFACE("19a8e35a-38be-418f-94e3-9f7323936870", IMTLD3D11ContextExt1) : public IMTLD3D11ContextExt {
  virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
      MTL_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize
  ) = 0;
};

DEFINE_COM_INTERFACE("efc77ae6-2179-4c0a-b844-7661ca0dcde7", IMTLD3D11DeviceExt)
    : public IUnknown {
  virtual void STDMETHODCALLTYPE SetShaderExtensionSlot(UINT Slot) = 0;
};

DEFINE_COM_INTERFACE("8cc8848d-76c9-4aea-8309-67a5d4c7e0bf",
                     IMTLD3D11CounterExt)
    : public IUnknown {
  virtual void STDMETHODCALLTYPE BeginCounter() = 0;
  virtual void STDMETHODCALLTYPE EndCounter() = 0;
  virtual void STDMETHODCALLTYPE EncodeBeginCounter(
      dxmt::ArgumentEncodingContext *enc) = 0;
  virtual void STDMETHODCALLTYPE EncodeEndCounter(
      dxmt::ArgumentEncodingContext *enc) = 0;
  virtual void STDMETHODCALLTYPE ReplayBeginCounter(
      dxmt::ArgumentEncodingContext *enc) = 0;
  virtual void STDMETHODCALLTYPE ReplayEndCounter(
      dxmt::ArgumentEncodingContext *enc) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCounterData(void *data) = 0;
};

// OpenXR interop: adopt an external MTLTexture as a D3D11 Texture2D, and extract
// the MTLSharedEvent behind an ID3D11Fence. Consumed by the wineopenxr bridge for
// zero-copy frame sharing with the native OpenXR runtime.
DEFINE_COM_INTERFACE("8b6fc874-7429-430d-8253-522e296cd8e2", IMTLD3D11InteropDevice) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE ImportMTLTexture2D(
      const D3D11_TEXTURE2D_DESC1 *pDesc, uint64_t mtlTexture, ID3D11Texture2D **ppTexture2D
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetFenceSharedEvent(ID3D11Fence * pFence, uint64_t *pMtlSharedEvent) = 0;
};
