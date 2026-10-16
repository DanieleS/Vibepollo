// Fork addition (Scirocco). The DLL's COM shell: the module ref count, the class factory, and the
// two exports LogonUI's host calls to make a provider. Registration is the host's job
// (session_unlock.cpp), so the DLL carries no self-registration code to run as SYSTEM.
//
// Ported from CouchPilot's CouchPilotUnlock (src/CouchPilotUnlock, docs/unlock.md).
#include "common.h"
#include "provider.h"

#include <new>

static LONG g_cRef = 0;  // outstanding objects + server locks; DllCanUnloadNow watches it

void DllAddRef() {
  InterlockedIncrement(&g_cRef);
}

void DllRelease() {
  InterlockedDecrement(&g_cRef);
}

namespace {
  class ClassFactory: public IClassFactory {
  public:
    ClassFactory():
        _cRef(1) {
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
      return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release() override {
      LONG n = InterlockedDecrement(&_cRef);
      if (n == 0) {
        delete this;
      }
      return n;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
      if (!ppv) {
        return E_POINTER;
      }
      if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory *>(this);
        AddRef();
        return S_OK;
      }
      *ppv = nullptr;
      return E_NOINTERFACE;
    }

    IFACEMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) override {
      if (pUnkOuter) {
        return CLASS_E_NOAGGREGATION;
      }
      auto *p = new (std::nothrow) UnlockProvider();
      if (!p) {
        return E_OUTOFMEMORY;
      }
      HRESULT hr = p->QueryInterface(riid, ppv);
      p->Release();
      return hr;
    }

    IFACEMETHODIMP LockServer(BOOL lock) override {
      if (lock) {
        DllAddRef();
      } else {
        DllRelease();
      }
      return S_OK;
    }

  private:
    virtual ~ClassFactory() = default;
    LONG _cRef;
  };
}  // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv) {
  if (rclsid != CLSID_VibepolloUnlock) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto *factory = new (std::nothrow) ClassFactory();
  if (!factory) {
    return E_OUTOFMEMORY;
  }
  HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return g_cRef == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hInst);
  }
  return TRUE;
}
