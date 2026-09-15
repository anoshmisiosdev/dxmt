#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_format.hpp"
#include "d3d11_resource.hpp"
#include "d3d11_shared_buffer.hpp"

namespace dxmt {

struct BufferViewInfo {
  BufferViewKey viewKey;
  uint32_t viewElementOffset;
  uint32_t viewElementWidth;
  uint32_t byteOffset;
  uint32_t byteWidth;
};

class D3D11Buffer : public TResourceBase<tag_buffer> {
private:
#ifdef DXMT_DEBUG
  std::string debug_name;
#endif
  bool structured;
  bool allow_raw_view;

  Rc<DynamicBuffer> dynamic_;

  using SRVBase = TResourceViewBase<tag_shader_resource_view<D3D11Buffer>>;

  class TBufferSRV : public SRVBase {

  public:
    TBufferSRV(
        const tag_shader_resource_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info
    ) :
        SRVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }

    ~TBufferSRV() {}
  };

  using UAVBase = TResourceViewBase<tag_unordered_access_view<D3D11Buffer>>;

  class UAVWithCounter : public UAVBase {
  public:
    UAVWithCounter(
        const tag_unordered_access_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info, Rc<Buffer>&& counter
    ) :
        UAVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth };
      counter_ = std::move(counter);
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
    }
  };

  using RTVBase = TResourceViewBase<tag_render_target_view<D3D11Buffer>>;

  class TBufferRTV : public RTVBase {
  public:
    TBufferRTV(
        const tag_render_target_view<>::DESC1 *pDesc, D3D11Buffer *pResource, MTLD3D11Device *pDevice,
        BufferViewInfo const &info, WMTPixelFormat format
    ) :
        RTVBase(pDesc, pResource, pDevice) {
      buffer_ = pResource->buffer_.ptr();
      view_id_ = info.viewKey;
      slice_ = {info.byteOffset, info.byteWidth, info.viewElementOffset, info.viewElementWidth};
      subset_ = ResourceSubsetState(info.byteOffset, info.byteWidth);
      format_ = format;
      pass_desc_ = {
          .RenderTargetArrayLength = 0,
          .SampleCount = 1,
          .DepthPlane = 0,
          .Width = info.viewElementWidth,
          .Height = 1,
      };
    }
  };

  // Shared buffers: pages come from a named file mapping, see d3d11_shared_buffer.hpp.
  HANDLE shared_mapping_ = nullptr;
  void *shared_memory_ = nullptr;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

public:
  //! Shared buffer: wraps memory the caller mapped from `shared_mapping_`.
  D3D11Buffer(
      const tag_buffer::DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, MTLD3D11Device *device,
      HANDLE sharedMapping, void *sharedMemory, D3DKMT_HANDLE localKmt, D3DKMT_HANDLE globalKmt
  ) :
      TResourceBase<tag_buffer>(*pDesc, device),
      shared_mapping_(sharedMapping),
      shared_memory_(sharedMemory),
      local_kmt_(localKmt),
      global_kmt_(globalKmt) {
    buffer_ = new Buffer(pDesc->ByteWidth, device->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    flags.set(BufferAllocationFlag::GpuManaged);
    auto allocation = buffer_->allocateExternalCpu(flags, sharedMemory);
    if (pInitialData)
      allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
    auto _ = buffer_->rename(std::move(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  }

  ~D3D11Buffer() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
    if (shared_memory_)
      UnmapViewOfFile(shared_memory_);
    if (shared_mapping_)
      CloseHandle(shared_mapping_);
  }

  D3D11Buffer(const tag_buffer::DESC1 *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, MTLD3D11Device *device) :
      TResourceBase<tag_buffer>(*pDesc, device) {
    buffer_ = new Buffer(pDesc->ByteWidth, device->GetMTLDevice());
    Flags<BufferAllocationFlag> flags;
    if (!m_parent->IsTraced() && pDesc->Usage == D3D11_USAGE_DYNAMIC)
      flags.set(BufferAllocationFlag::CpuWriteCombined);
    // if (pDesc->Usage != D3D11_USAGE_DEFAULT)
    if (pDesc->Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(BufferAllocationFlag::GpuReadonly);
    if (pDesc->Usage != D3D11_USAGE_DYNAMIC) {
      if (pInitialData)
        flags.set(BufferAllocationFlag::GpuManaged);
      else if (pDesc->BindFlags & kD3D11OutputBindFlags)
        flags.set(BufferAllocationFlag::GpuPrivate);
      else
        flags.set(BufferAllocationFlag::GpuManaged);
    } else {
      flags.set(BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
      flags.set(BufferAllocationFlag::CpuShadow);
#endif
    }
    auto allocation = buffer_->allocate(flags);
    auto &initializer = device->GetDXMTDevice().queue().initializer;
    if (pInitialData) {
      allocation->updateContents(0, pInitialData->pSysMem, pDesc->ByteWidth);
    } else if (flags.test(BufferAllocationFlag::GpuPrivate)) {
      initializer.initWithZero(allocation.ptr(), 0, buffer_->length());
    }
    auto _ = buffer_->rename(std::move(allocation));
    D3D11_ASSERT(_.ptr() == nullptr);
    structured = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    allow_raw_view = pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (!(desc.BindFlags & kD3D11OutputBindFlags)) {
      dynamic_ = new DynamicBuffer(buffer_.ptr(), flags);
    }
  }

  HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (pSharedHandle == nullptr || (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
      return E_INVALIDARG;

    if (!(desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
      *pSharedHandle = NULL;
      return S_OK;
    }

    if (!global_kmt_)
      return E_INVALIDARG;

    *pSharedHandle = reinterpret_cast<HANDLE>(global_kmt_);
    return S_OK;
  }

  HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    InitReturnPtr(pNTHandle);
    if (!local_kmt_)
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES *>(Attributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (pName) {
      DWORD session, len, name_len = wcslen(pName);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    D3DKMT_HANDLE handles[1] = {local_kmt_};
    if (D3DKMTShareObjects(1, handles, &attr, Access, pNTHandle)) {
      ERR("SharedBuffer: Failed to create shared NT handle");
      return E_FAIL;
    }
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(ID3D11Texture1D) ||
        riid == __uuidof(ID3D11Texture2D) ||
        riid == __uuidof(ID3D11Texture3D)) {
      TRACE("D3D11Resource(buffer): texture interface query ",
            str::format(riid));
      return E_NOINTERFACE;
    }

    return TResourceBase<tag_buffer>::QueryInterface(riid, ppvObject);
  }

  Rc<StagingResource>
  staging(UINT Subresource) final {
    return nullptr;
  }
  Rc<DynamicBuffer>
  dynamicBuffer(UINT *pBufferLength, UINT *pBindFlags) override {
    *pBufferLength = desc.ByteWidth;
    *pBindFlags = desc.BindFlags;
    return dynamic_;
  }
  Rc<DynamicLinearTexture>
  dynamicLinearTexture(UINT *, UINT *) final {
    return {};
  };
  Rc<DynamicBuffer>
  dynamicTexture(UINT , UINT *, UINT *) final {
    return {};
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc, ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER &&
        finalDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
    } else if (finalDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && finalDesc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateShaderResourceView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({.format = view_format});

    auto srv = ref(new TBufferSRV(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        }
    ));
    *ppView = srv;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc, ID3D11RenderTargetView1 **ppView) override {
    if (!(desc.BindFlags & D3D11_BIND_RENDER_TARGET))
      return E_INVALIDARG;

    if (!pDesc)
      return E_INVALIDARG;

    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc = *pDesc;
    if (finalDesc.ViewDimension != D3D11_RTV_DIMENSION_BUFFER)
      return E_INVALIDARG;
    if (structured || finalDesc.Format == DXGI_FORMAT_UNKNOWN)
      return E_INVALIDARG;

    MTL_DXGI_FORMAT_DESC format;
    if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format)))
      return E_FAIL;
    if (!format.BytesPerTexel || format.Flag & MTL_DXGI_FORMAT_TYPELESS)
      return E_INVALIDARG;
    if (!any_bit_set(m_parent->GetMTLPixelFormatCapability(format.PixelFormat) & FormatCapability::Color))
      return E_INVALIDARG;

    uint64_t byteOffset = uint64_t(finalDesc.Buffer.FirstElement) * format.BytesPerTexel;
    uint64_t byteWidth = uint64_t(finalDesc.Buffer.NumElements) * format.BytesPerTexel;
    if (!finalDesc.Buffer.NumElements || byteOffset > desc.ByteWidth || byteWidth > desc.ByteWidth - byteOffset)
      return E_INVALIDARG;
    if (byteOffset > UINT32_MAX || byteWidth > UINT32_MAX)
      return E_INVALIDARG;

    if (!ppView)
      return S_FALSE;

    auto viewId = buffer_->createView({
        .format = format.PixelFormat,
        .usage = WMTTextureUsageRenderTarget,
        .type = WMTTextureType2D,
        .byteOffset = uint32_t(byteOffset),
        .byteLength = uint32_t(byteWidth),
    });

    auto rtv = ref(new TBufferRTV(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = finalDesc.Buffer.FirstElement,
            .viewElementWidth = finalDesc.Buffer.NumElements,
            .byteOffset = uint32_t(byteOffset),
            .byteWidth = uint32_t(byteWidth),
        },
        format.PixelFormat
    ));
    *ppView = rtv;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc, ID3D11UnorderedAccessView1 **ppView)
      override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc, &finalDesc))) {
      return E_INVALIDARG;
    }
    if (finalDesc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) {
      return E_FAIL;
    }
    WMTPixelFormat view_format = WMTPixelFormatInvalid;
    uint32_t offset, size, viewElementOffset, viewElementWidth;
    Rc<Buffer> counter = {};
    if (structured) {
      if (finalDesc.Format != DXGI_FORMAT_UNKNOWN) {
        return E_INVALIDARG;
      }
      // StructuredBuffer
      CalculateBufferViewOffsetAndSize(
          this->desc, desc.StructureByteStride, finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset,
          size
      );
      view_format = WMTPixelFormatR32Uint;
      // when structured buffer is interpreted as typed buffer for any reason
      viewElementOffset = finalDesc.Buffer.FirstElement * (desc.StructureByteStride >> 2);
      viewElementWidth = finalDesc.Buffer.NumElements * (desc.StructureByteStride >> 2);
      if (finalDesc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) {
        counter = new dxmt::Buffer(sizeof(uint32_t), m_parent->GetMTLDevice());
        auto allocation = counter->allocate(BufferAllocationFlag::GpuManaged | BufferAllocationFlag::NoTracking);
        const uint32_t initial_counter = 0;
        allocation->updateContents(0, &initial_counter, sizeof(initial_counter));
        allocation->buffer().didModifyRange(0, sizeof(initial_counter));
        counter->rename(std::move(allocation));
      }
    } else if (finalDesc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) {
      if (!allow_raw_view)
        return E_INVALIDARG;
      if (finalDesc.Format != DXGI_FORMAT_R32_TYPELESS)
        return E_INVALIDARG;
      CalculateBufferViewOffsetAndSize(
          this->desc, sizeof(uint32_t), finalDesc.Buffer.FirstElement, finalDesc.Buffer.NumElements, offset, size
      );
      view_format = WMTPixelFormatR32Uint;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    } else {
      MTL_DXGI_FORMAT_DESC format;
      if (FAILED(MTLQueryDXGIFormat(m_parent->GetMTLDevice(), finalDesc.Format, format))) {
        return E_FAIL;
      }
      if (!format.BytesPerTexel) {
        ERR("D3D11Buffer::CreateUnorderedAccessView: not an ordinary or packed format: ", finalDesc.Format);
        return E_FAIL;
      }

      view_format = format.PixelFormat;
      offset = finalDesc.Buffer.FirstElement * format.BytesPerTexel;
      size = finalDesc.Buffer.NumElements * format.BytesPerTexel;
      viewElementOffset = finalDesc.Buffer.FirstElement;
      viewElementWidth = finalDesc.Buffer.NumElements;
    }

    if (!ppView) {
      return S_FALSE;
    }

    auto viewId = buffer_->createView({
        .format = view_format,
        .usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite,
    });

    auto srv = ref(new UAVWithCounter(
        &finalDesc, this, m_parent,
        {
            .viewKey = viewId,
            .viewElementOffset = viewElementOffset,
            .viewElementWidth = viewElementWidth,
            .byteOffset = offset,
            .byteWidth = size,
        },
        std::move(counter)
    ));
    *ppView = srv;
    return S_OK;
  };

  void
  OnSetDebugObjectName(LPCSTR Name) override {
    if (!Name) {
      return;
    }
#ifdef DXMT_DEBUG
    debug_name = std::string(Name);
#endif
  }
};

uint64_t
SharedBufferBackingSize(const D3D11_BUFFER_DESC &desc) {
  // Metal wants the host allocation page aligned, and the mapping has to cover the
  // whole buffer.
  constexpr uint64_t page = 0x4000; // 16 KiB covers both 4 KiB and 16 KiB page sizes
  return (uint64_t(desc.ByteWidth) + page - 1) & ~(page - 1);
}

/*!
 * Shared buffer: back the buffer with a named file mapping so other processes (and
 * other devices in this one) can map the same pages, and register it with D3DKMT so
 * a shared handle can be handed out.
 */
static HRESULT
CreateSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    // A keyed mutex needs a shared GPU event per resource; buffers don't have one yet.
    WARN("SharedBuffer: keyed mutex is not supported for buffers");
    return E_INVALIDARG;
  }

  const bool ntSecuritySharing = pDesc->MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  char mappingName[54] = {};
  D3DKMT_HANDLE localKmt = 0;
  D3DKMT_HANDLE globalKmt = 0;
  HRESULT hr = CreateSharedBufferKmtResource(pDevice, *pDesc, ntSecuritySharing, mappingName, localKmt, globalKmt);
  if (FAILED(hr))
    return hr;

  const uint64_t size = SharedBufferBackingSize(*pDesc);
  HANDLE mapping = CreateFileMappingA(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, uint32_t(size >> 32), uint32_t(size), mappingName
  );
  if (!mapping) {
    ERR("SharedBuffer: Failed to create the file mapping for a shared buffer");
    return E_FAIL;
  }
  void *memory = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!memory) {
    ERR("SharedBuffer: Failed to map the shared buffer");
    CloseHandle(mapping);
    return E_FAIL;
  }

  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(
      ref(new D3D11Buffer(pDesc, pInitialData, pDevice, mapping, memory, localKmt, globalKmt))
  );
  return S_OK;
}

HRESULT
ImportSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC &desc, const char *mappingName, REFIID riid, void **ppResource
) {
  HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
  if (!mapping) {
    ERR("ImportSharedBuffer: Failed to open the shared buffer mapping");
    return E_INVALIDARG;
  }
  void *memory = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, SharedBufferBackingSize(desc));
  if (!memory) {
    ERR("ImportSharedBuffer: Failed to map the shared buffer");
    CloseHandle(mapping);
    return E_INVALIDARG;
  }

  // The importing side doesn't own the D3DKMT resource, so it passes no handles.
  Com<ID3D11Buffer> buffer = reinterpret_cast<ID3D11Buffer *>(
      ref(new D3D11Buffer(&desc, nullptr, pDevice, mapping, memory, 0, 0))
  );
  return buffer->QueryInterface(riid, ppResource);
}

HRESULT
CreateBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData,
    ID3D11Buffer **ppBuffer
) {
  constexpr UINT sharedFlags =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (pDesc->MiscFlags & sharedFlags)
    return CreateSharedBuffer(pDevice, pDesc, pInitialData, ppBuffer);

  *ppBuffer = reinterpret_cast<ID3D11Buffer *>(ref(new D3D11Buffer(pDesc, pInitialData, pDevice)));
  return S_OK;
}

} // namespace dxmt
