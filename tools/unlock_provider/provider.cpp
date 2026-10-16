#include "provider.h"

#include "lsa_secrets.h"

#include <cstring>
#include <new>

// Defined in credential.cpp.
const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *UnlockFieldDescriptors();

UnlockProvider::UnlockProvider():
    _cRef(1),
    _cred(nullptr),
    _pEvents(nullptr),
    _adviseContext(0) {
  DllAddRef();
  InitializeCriticalSection(&_lock);
}

UnlockProvider::~UnlockProvider() {
  _pipe.Stop();
  if (_cred) {
    _cred->Release();
    _cred = nullptr;
  }
  if (_pEvents) {
    _pEvents->Release();
    _pEvents = nullptr;
  }
  DeleteCriticalSection(&_lock);
  DllRelease();
}

// IUnknown
IFACEMETHODIMP_(ULONG) UnlockProvider::AddRef() {
  return InterlockedIncrement(&_cRef);
}

IFACEMETHODIMP_(ULONG) UnlockProvider::Release() {
  LONG n = InterlockedDecrement(&_cRef);
  if (n == 0) {
    delete this;
  }
  return n;
}

IFACEMETHODIMP UnlockProvider::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) {
    return E_POINTER;
  }
  if (riid == IID_IUnknown || riid == __uuidof(ICredentialProvider)) {
    *ppv = static_cast<ICredentialProvider *>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

IFACEMETHODIMP UnlockProvider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD) {
  // Only unlocking a session that is already signed in. Signing in after a reboot, CredUI and
  // change-password are none of our business.
  if (cpus != CPUS_UNLOCK_WORKSTATION) {
    return E_NOTIMPL;
  }

  if (!_cred) {
    _cred = new (std::nothrow) UnlockCredential();
    if (!_cred) {
      return E_OUTOFMEMORY;
    }
  }
  _cred->SetScenario(cpus);
  // A fresh scenario: drop anything a previous one left armed.
  _cred->Disarm();

  // Nothing enrolled (the feature was turned off but the provider is still registered): stay
  // inert, no pipe, no tile. Otherwise bind to the enrolled user's tile and start listening.
  std::wstring sid = ReadLsaSecret(VIBEPOLLO_UNLOCK_SID_KEY);
  if (sid.empty()) {
    return S_OK;
  }
  _cred->SetUserSid(sid.c_str());
  _pipe.Start([this]() {
    return OnUnlock();
  });
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockProvider::Advise(ICredentialProviderEvents *pcpe, UINT_PTR upAdviseContext) {
  EnterCriticalSection(&_lock);
  if (_pEvents) {
    _pEvents->Release();
  }
  _pEvents = pcpe;
  if (_pEvents) {
    _pEvents->AddRef();
  }
  _adviseContext = upAdviseContext;
  LeaveCriticalSection(&_lock);
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::UnAdvise() {
  EnterCriticalSection(&_lock);
  if (_pEvents) {
    _pEvents->Release();
    _pEvents = nullptr;
  }
  _adviseContext = 0;
  LeaveCriticalSection(&_lock);
  // No one left to tell about a submit: don't keep an armed credential, and its password, around.
  if (_cred) {
    _cred->Disarm();
  }
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::GetFieldDescriptorCount(DWORD *pdwCount) {
  *pdwCount = VUF_NUM_FIELDS;
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::GetFieldDescriptorAt(DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd) {
  if (dwIndex >= VUF_NUM_FIELDS || !ppcpfd) {
    return E_INVALIDARG;
  }

  const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR &src = UnlockFieldDescriptors()[dwIndex];
  auto *out = (CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *) CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR));
  if (!out) {
    return E_OUTOFMEMORY;
  }
  out->dwFieldID = src.dwFieldID;
  out->cpft = src.cpft;
  out->guidFieldType = src.guidFieldType;
  out->pszLabel = nullptr;
  if (src.pszLabel) {
    size_t cb = (wcslen(src.pszLabel) + 1) * sizeof(wchar_t);
    out->pszLabel = (LPWSTR) CoTaskMemAlloc(cb);
    if (!out->pszLabel) {
      CoTaskMemFree(out);
      return E_OUTOFMEMORY;
    }
    memcpy(out->pszLabel, src.pszLabel, cb);
  }
  *ppcpfd = out;
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::GetCredentialCount(DWORD *pdwCount, DWORD *pdwDefault, BOOL *pbAutoLogonWithDefault) {
  // No tile until a request arms us, so the lock screen never shows an empty option. Armed, we
  // are the one tile and ask LogonUI to log on with it at once: that is what turns the request
  // into an unlock, with no click.
  if (_cred && _cred->IsArmed()) {
    *pdwCount = 1;
    *pdwDefault = 0;
    *pbAutoLogonWithDefault = TRUE;
  } else {
    *pdwCount = 0;
    *pdwDefault = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *pbAutoLogonWithDefault = FALSE;
  }
  return S_OK;
}

IFACEMETHODIMP UnlockProvider::GetCredentialAt(DWORD dwIndex, ICredentialProviderCredential **ppcpc) {
  // Only valid while armed, when the count is 1.
  if (dwIndex != 0 || !_cred || !_cred->IsArmed() || !ppcpc) {
    return E_INVALIDARG;
  }
  return _cred->QueryInterface(__uuidof(ICredentialProviderCredential), (void **) ppcpc);
}

UnlockOutcome UnlockProvider::OnUnlock() {
  if (!_cred) {
    return UnlockOutcome::busy;
  }

  // Without an events sink LogonUI can't be made to submit, so arming would only strand the
  // password in memory and turn the next request away as busy.
  EnterCriticalSection(&_lock);
  ICredentialProviderEvents *events = _pEvents;
  UINT_PTR ctx = _adviseContext;
  if (events) {
    events->AddRef();
  }
  LeaveCriticalSection(&_lock);
  if (!events) {
    return UnlockOutcome::busy;
  }

  std::wstring account = ReadLsaSecret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY);
  std::wstring password = ReadLsaSecret(VIBEPOLLO_UNLOCK_PASSWORD_KEY);
  UnlockOutcome outcome = UnlockOutcome::not_enrolled;
  if (!account.empty() && !password.empty()) {
    outcome = _cred->Arm(account.c_str(), password.c_str()) ? UnlockOutcome::ok : UnlockOutcome::busy;
  }
  WipeString(password);

  if (outcome == UnlockOutcome::ok) {
    // Wake LogonUI: it re-reads GetCredentialCount, finds us armed with auto-logon, and submits.
    events->CredentialsChanged(ctx);
  }
  events->Release();
  return outcome;
}
