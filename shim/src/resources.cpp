// See resources.h. Hand-written methods of the generated resource proxies
// (identity, lifetime, Lock/Unlock) plus Wrap/Unwrap.
#include "resources.h"

#include <unordered_map>

#include "gen/d3d9_proxy.h"
#include "log.h"
#include "webgpu/backend.h"

namespace agpu {
namespace {

// {5C6F6E36-8E1F-4A2B-9C4D-616E64726561} "andrea"
constexpr GUID kProxyGuid = {0x5c6f6e36, 0x8e1f, 0x4a2b, {0x9c, 0x4d, 0x61, 0x6e, 0x64, 0x72, 0x65, 0x61}};

DeviceProxy* g_device = nullptr;

// Game-visible pointer (the proxy's interface) -> proxy. Single-threaded like the game.
std::unordered_map<const void*, ResourceProxy*> g_live;

// Lives in the real resource's private data; D3D releases it when the real resource
// is destroyed, and that is when the proxy goes away.
class LifetimeSentinel final : public IUnknown {
 public:
  LifetimeSentinel(ResourceProxy* proxy, const void* gamePointer) : proxy_(proxy), gamePointer_(gamePointer) {}
  ResourceProxy* proxy() const { return proxy_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == __uuidof(IUnknown)) {
      AddRef();
      *ppv = this;
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG refs = --refs_;
    if (refs == 0) {
      wgb::ResourceDestroyed(proxy_);
      g_live.erase(gamePointer_);
      delete proxy_;
      delete this;
    }
    return refs;
  }

 private:
  ResourceProxy* proxy_;
  const void* gamePointer_;
  ULONG refs_ = 1;
};

ResourceProxy* ProxyOfReal(IDirect3DResource9* real) {
  IUnknown* data = nullptr;
  DWORD size = sizeof(data);
  if (FAILED(real->GetPrivateData(kProxyGuid, &data, &size)) || !data) return nullptr;
  ResourceProxy* proxy = static_cast<LifetimeSentinel*>(data)->proxy();
  data->Release();  // GetPrivateData AddRef'd it
  return proxy;
}

template <class Proxy, class Real>
Real* WrapImpl(Real* real) {
  if (!real) return nullptr;
  if (g_live.count(real)) return real;  // already a proxy
  if (ResourceProxy* existing = ProxyOfReal(real)) return static_cast<Proxy*>(existing);
  auto proxy = new Proxy(real);
  Real* gamePointer = proxy;
  g_live[gamePointer] = proxy;
  IUnknown* sentinel = new LifetimeSentinel(proxy, gamePointer);
  // With D3DSPD_IUNKNOWN the data pointer *is* the interface; D3D AddRefs it.
  if (FAILED(real->SetPrivateData(kProxyGuid, sentinel, sizeof(sentinel), D3DSPD_IUNKNOWN)))
    Logf("[resources] SetPrivateData failed; proxy %p will leak\n", proxy);
  sentinel->Release();  // the real resource holds it now
  return gamePointer;
}

template <class Real>
Real* UnwrapImpl(Real* p) {
  auto it = g_live.find(p);
  return it == g_live.end() ? p : reinterpret_cast<Real*>(it->second->realUnknown());
}

template <class Self>
HRESULT ResourceQueryInterface(Self* self, IUnknown* real, REFIID riid, void** ppv, std::initializer_list<IID> own) {
  if (!ppv) return E_POINTER;
  for (const IID& iid : own) {
    if (riid == iid) {
      self->AddRef();
      *ppv = self;
      return S_OK;
    }
  }
  return real->QueryInterface(riid, ppv);
}

HRESULT ReturnDevice(IDirect3DDevice9** ppDevice) {
  if (!ppDevice) return D3DERR_INVALIDCALL;
  *ppDevice = g_device;
  if (g_device) g_device->AddRef();
  return g_device ? D3D_OK : D3DERR_INVALIDCALL;
}

}  // namespace

IDirect3DTexture9* Wrap(IDirect3DTexture9* real) { return WrapImpl<Texture9Proxy>(real); }
IDirect3DCubeTexture9* Wrap(IDirect3DCubeTexture9* real) { return WrapImpl<CubeTexture9Proxy>(real); }
IDirect3DSurface9* Wrap(IDirect3DSurface9* real) { return WrapImpl<Surface9Proxy>(real); }
IDirect3DVertexBuffer9* Wrap(IDirect3DVertexBuffer9* real) { return WrapImpl<VertexBuffer9Proxy>(real); }
IDirect3DIndexBuffer9* Wrap(IDirect3DIndexBuffer9* real) { return WrapImpl<IndexBuffer9Proxy>(real); }

IDirect3DBaseTexture9* Wrap(IDirect3DBaseTexture9* real) {
  if (!real || g_live.count(real)) return real;
  switch (real->GetType()) {
    case D3DRTYPE_TEXTURE: return Wrap(static_cast<IDirect3DTexture9*>(real));
    case D3DRTYPE_CUBETEXTURE: return Wrap(static_cast<IDirect3DCubeTexture9*>(real));
    default: return real;  // volume textures are not proxied
  }
}

IDirect3DTexture9* Unwrap(IDirect3DTexture9* p) { return UnwrapImpl(p); }
IDirect3DCubeTexture9* Unwrap(IDirect3DCubeTexture9* p) { return UnwrapImpl(p); }
IDirect3DSurface9* Unwrap(IDirect3DSurface9* p) { return UnwrapImpl(p); }
IDirect3DVertexBuffer9* Unwrap(IDirect3DVertexBuffer9* p) { return UnwrapImpl(p); }
IDirect3DIndexBuffer9* Unwrap(IDirect3DIndexBuffer9* p) { return UnwrapImpl(p); }
IDirect3DBaseTexture9* Unwrap(IDirect3DBaseTexture9* p) { return UnwrapImpl(p); }

ResourceProxy* FindProxy(const void* gamePointer) {
  auto it = g_live.find(gamePointer);
  return it == g_live.end() ? nullptr : it->second;
}

void SetDeviceProxy(DeviceProxy* device) { g_device = device; }

// ---------------------------------------------------------------------------
// Texture9

HRESULT STDMETHODCALLTYPE Texture9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  return ResourceQueryInterface(this, real_, riid, ppvObj,
                                {__uuidof(IUnknown), __uuidof(IDirect3DResource9), __uuidof(IDirect3DBaseTexture9),
                                 __uuidof(IDirect3DTexture9)});
}
ULONG STDMETHODCALLTYPE Texture9Proxy::AddRef() { return real_->AddRef(); }
ULONG STDMETHODCALLTYPE Texture9Proxy::Release() { return real_->Release(); }
HRESULT STDMETHODCALLTYPE Texture9Proxy::GetDevice(IDirect3DDevice9** ppDevice) { return ReturnDevice(ppDevice); }
HRESULT STDMETHODCALLTYPE Texture9Proxy::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect,
                                                  DWORD Flags) {
  HRESULT hr = real_->LockRect(Level, pLockedRect, pRect, Flags);
  if (SUCCEEDED(hr) && backend && !(Flags & D3DLOCK_READONLY))
    locks.push_back({Level, *pLockedRect, pRect ? *pRect : RECT{}, pRect != nullptr, 0, 0, nullptr});
  return hr;
}
HRESULT STDMETHODCALLTYPE Texture9Proxy::UnlockRect(UINT Level) {
  for (auto it = locks.begin(); it != locks.end(); ++it) {
    if (it->subresource != Level) continue;
    wgb::TextureWritten(this, Level, it->locked, it->hasRect ? &it->rect : nullptr);
    locks.erase(it);
    break;
  }
  return real_->UnlockRect(Level);
}

// ---------------------------------------------------------------------------
// CubeTexture9

HRESULT STDMETHODCALLTYPE CubeTexture9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  return ResourceQueryInterface(this, real_, riid, ppvObj,
                                {__uuidof(IUnknown), __uuidof(IDirect3DResource9), __uuidof(IDirect3DBaseTexture9),
                                 __uuidof(IDirect3DCubeTexture9)});
}
ULONG STDMETHODCALLTYPE CubeTexture9Proxy::AddRef() { return real_->AddRef(); }
ULONG STDMETHODCALLTYPE CubeTexture9Proxy::Release() { return real_->Release(); }
HRESULT STDMETHODCALLTYPE CubeTexture9Proxy::GetDevice(IDirect3DDevice9** ppDevice) { return ReturnDevice(ppDevice); }
HRESULT STDMETHODCALLTYPE CubeTexture9Proxy::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                                      D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
  return real_->LockRect(FaceType, Level, pLockedRect, pRect, Flags);
}
HRESULT STDMETHODCALLTYPE CubeTexture9Proxy::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  return real_->UnlockRect(FaceType, Level);
}

// ---------------------------------------------------------------------------
// Surface9

HRESULT STDMETHODCALLTYPE Surface9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  return ResourceQueryInterface(this, real_, riid, ppvObj,
                                {__uuidof(IUnknown), __uuidof(IDirect3DResource9), __uuidof(IDirect3DSurface9)});
}
ULONG STDMETHODCALLTYPE Surface9Proxy::AddRef() { return real_->AddRef(); }
ULONG STDMETHODCALLTYPE Surface9Proxy::Release() { return real_->Release(); }
HRESULT STDMETHODCALLTYPE Surface9Proxy::GetDevice(IDirect3DDevice9** ppDevice) { return ReturnDevice(ppDevice); }
HRESULT STDMETHODCALLTYPE Surface9Proxy::GetContainer(REFIID riid, void** ppContainer) {
  if (riid == __uuidof(IDirect3DDevice9)) return ReturnDevice(reinterpret_cast<IDirect3DDevice9**>(ppContainer));
  HRESULT hr = real_->GetContainer(riid, ppContainer);
  if (FAILED(hr) || !ppContainer || !*ppContainer) return hr;
  if (riid == __uuidof(IDirect3DTexture9))
    *ppContainer = Wrap(static_cast<IDirect3DTexture9*>(*ppContainer));
  else if (riid == __uuidof(IDirect3DCubeTexture9))
    *ppContainer = Wrap(static_cast<IDirect3DCubeTexture9*>(*ppContainer));
  else if (riid == __uuidof(IDirect3DBaseTexture9))
    *ppContainer = Wrap(static_cast<IDirect3DBaseTexture9*>(*ppContainer));
  return hr;
}
// RenderWare fills textures through their mip-level surfaces (GetSurfaceLevel +
// LockRect), so a surface lock that belongs to a texture uploads into that level.
HRESULT STDMETHODCALLTYPE Surface9Proxy::LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) {
  HRESULT hr = real_->LockRect(pLockedRect, pRect, Flags);
  if (FAILED(hr) || (Flags & D3DLOCK_READONLY) || !wgb::Active()) return hr;
  IDirect3DTexture9* container = nullptr;
  if (FAILED(real_->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&container))) || !container)
    return hr;
  ResourceProxy* texture = ProxyOfReal(container);
  DWORD levels = container->GetLevelCount();
  for (DWORD level = 0; texture && level < levels; ++level) {
    IDirect3DSurface9* surface = nullptr;
    if (FAILED(container->GetSurfaceLevel(level, &surface))) continue;
    bool match = surface == real_;
    surface->Release();
    if (match) {
      // `subresource` holds the level; `data` remembers the texture proxy.
      locks.push_back({level, *pLockedRect, pRect ? *pRect : RECT{}, pRect != nullptr, 0, 0, texture});
      break;
    }
  }
  container->Release();
  return hr;
}
HRESULT STDMETHODCALLTYPE Surface9Proxy::UnlockRect() {
  if (!locks.empty()) {
    const PendingLock& lock = locks.back();
    wgb::TextureWritten(static_cast<ResourceProxy*>(lock.data), lock.subresource, lock.locked,
                        lock.hasRect ? &lock.rect : nullptr);
    locks.pop_back();
  }
  return real_->UnlockRect();
}

// ---------------------------------------------------------------------------
// VertexBuffer9 / IndexBuffer9

HRESULT STDMETHODCALLTYPE VertexBuffer9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  return ResourceQueryInterface(this, real_, riid, ppvObj,
                                {__uuidof(IUnknown), __uuidof(IDirect3DResource9), __uuidof(IDirect3DVertexBuffer9)});
}
ULONG STDMETHODCALLTYPE VertexBuffer9Proxy::AddRef() { return real_->AddRef(); }
ULONG STDMETHODCALLTYPE VertexBuffer9Proxy::Release() { return real_->Release(); }
HRESULT STDMETHODCALLTYPE VertexBuffer9Proxy::GetDevice(IDirect3DDevice9** ppDevice) { return ReturnDevice(ppDevice); }
HRESULT STDMETHODCALLTYPE VertexBuffer9Proxy::Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
  HRESULT hr = real_->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  if (SUCCEEDED(hr) && backend && !(Flags & D3DLOCK_READONLY))
    locks.push_back({0, {}, {}, false, OffsetToLock, SizeToLock, *ppbData});
  return hr;
}
HRESULT STDMETHODCALLTYPE VertexBuffer9Proxy::Unlock() {
  if (!locks.empty()) {
    const PendingLock& lock = locks.back();
    wgb::BufferWritten(this, lock.offset, lock.size, lock.data);
    locks.pop_back();
  }
  return real_->Unlock();
}

HRESULT STDMETHODCALLTYPE IndexBuffer9Proxy::QueryInterface(REFIID riid, void** ppvObj) {
  return ResourceQueryInterface(this, real_, riid, ppvObj,
                                {__uuidof(IUnknown), __uuidof(IDirect3DResource9), __uuidof(IDirect3DIndexBuffer9)});
}
ULONG STDMETHODCALLTYPE IndexBuffer9Proxy::AddRef() { return real_->AddRef(); }
ULONG STDMETHODCALLTYPE IndexBuffer9Proxy::Release() { return real_->Release(); }
HRESULT STDMETHODCALLTYPE IndexBuffer9Proxy::GetDevice(IDirect3DDevice9** ppDevice) { return ReturnDevice(ppDevice); }
HRESULT STDMETHODCALLTYPE IndexBuffer9Proxy::Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
  HRESULT hr = real_->Lock(OffsetToLock, SizeToLock, ppbData, Flags);
  if (SUCCEEDED(hr) && backend && !(Flags & D3DLOCK_READONLY))
    locks.push_back({0, {}, {}, false, OffsetToLock, SizeToLock, *ppbData});
  return hr;
}
HRESULT STDMETHODCALLTYPE IndexBuffer9Proxy::Unlock() {
  if (!locks.empty()) {
    const PendingLock& lock = locks.back();
    wgb::BufferWritten(this, lock.offset, lock.size, lock.data);
    locks.pop_back();
  }
  return real_->Unlock();
}

}  // namespace agpu
