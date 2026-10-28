#pragma once
// Proxies for D3D9 resources (textures, cube textures, surfaces, vertex/index
// buffers). The game only ever holds proxies; the backend sees the data the game
// writes through Lock/Unlock.
//
// Lifetime: every real resource carries a small sentinel IUnknown in its private
// data (D3DSPD_IUNKNOWN). D3D releases it exactly when the real resource is
// destroyed, which deletes the proxy. Reference counting is plain forwarding.
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <vector>

namespace agpu {

class DeviceProxy;

class ResourceProxy {
 public:
  explicit ResourceProxy(IUnknown* real) : real_(real) {}
  virtual ~ResourceProxy() = default;
  ResourceProxy(const ResourceProxy&) = delete;
  ResourceProxy& operator=(const ResourceProxy&) = delete;

  IUnknown* realUnknown() const { return real_; }

  // Owned by the WebGPU backend (mirror); released via wgb::ResourceDestroyed.
  void* backend = nullptr;

  // Locks waiting for their Unlock (the data reaches the backend on Unlock).
  struct PendingLock {
    UINT subresource;
    D3DLOCKED_RECT locked;
    RECT rect;
    bool hasRect;
    UINT offset, size;
    void* data;
  };
  std::vector<PendingLock> locks;

 private:
  IUnknown* real_;
};

// Returns a proxy for a real resource (existing one if it has been seen before).
IDirect3DTexture9* Wrap(IDirect3DTexture9* real);
IDirect3DCubeTexture9* Wrap(IDirect3DCubeTexture9* real);
IDirect3DSurface9* Wrap(IDirect3DSurface9* real);
IDirect3DVertexBuffer9* Wrap(IDirect3DVertexBuffer9* real);
IDirect3DIndexBuffer9* Wrap(IDirect3DIndexBuffer9* real);
IDirect3DBaseTexture9* Wrap(IDirect3DBaseTexture9* real);

// Returns the real resource behind a proxy; anything else is passed through.
IDirect3DTexture9* Unwrap(IDirect3DTexture9* p);
IDirect3DCubeTexture9* Unwrap(IDirect3DCubeTexture9* p);
IDirect3DSurface9* Unwrap(IDirect3DSurface9* p);
IDirect3DVertexBuffer9* Unwrap(IDirect3DVertexBuffer9* p);
IDirect3DIndexBuffer9* Unwrap(IDirect3DIndexBuffer9* p);
IDirect3DBaseTexture9* Unwrap(IDirect3DBaseTexture9* p);

// The proxy behind a pointer the game passed in, or nullptr.
ResourceProxy* FindProxy(const void* gamePointer);

// Returned by IDirect3DResource9::GetDevice.
void SetDeviceProxy(DeviceProxy* device);

}  // namespace agpu
