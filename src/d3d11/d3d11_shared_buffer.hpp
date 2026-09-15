#pragma once

#include "util_d3dkmt.h"
#include <d3d11_1.h>

namespace dxmt {

class MTLD3D11Device;

/*
 * Shared D3D11 buffers.
 *
 * Textures are shared through a Metal shared texture (a mach port travels in the
 * D3DKMT private runtime data). Metal has no equivalent for buffers, so a shared
 * buffer is backed by a named Win32 file mapping instead: every process maps the
 * same pages and wraps them in its own MTLBuffer (newBufferWithBytesNoCopy). The
 * mapping's name travels in the same runtime-data field the texture path uses for
 * the mach port name.
 *
 * SteamVR's compositor needs this: it creates a shared constant buffer at start-up
 * and fails with VRInitError_Compositor_CreateSharedFrameInfoConstantBuffer (461)
 * when the shared handle can't be produced.
 */

//! Size of the shared-memory backing for a buffer of this description.
uint64_t SharedBufferBackingSize(const D3D11_BUFFER_DESC &desc);

/*!
 * Register a shared buffer with D3DKMT: picks a unique name for the file mapping
 * the caller creates, and produces the local and global (shared) handles.
 */
HRESULT CreateSharedBufferKmtResource(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC &desc, bool ntSecuritySharing, char (&mappingNameOut)[54],
    D3DKMT_HANDLE &localOut, D3DKMT_HANDLE &globalOut
);

//! Open a shared buffer created by CreateSharedBufferKmtResource in another device or process.
HRESULT ImportSharedBuffer(
    MTLD3D11Device *pDevice, const D3D11_BUFFER_DESC &desc, const char *mappingName, REFIID riid, void **ppResource
);

} // namespace dxmt
