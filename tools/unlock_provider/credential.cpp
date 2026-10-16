#include "credential.h"

#include "cred_packing.h"

#include <new>

namespace {
  // The one field: a hidden label. LogonUI requires at least one descriptor; nobody ever sees it.
  const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR s_fields[VUF_NUM_FIELDS] = {
    {VUF_LABEL, CPFT_LARGE_TEXT, const_cast<LPWSTR>(VIBEPOLLO_UNLOCK_FRIENDLY_NAME), {}},
  };

  void FreeSecret(PWSTR &s) {
    if (s) {
      SecureZeroMemory(s, wcslen(s) * sizeof(wchar_t));
      CoTaskMemFree(s);
      s = nullptr;
    }
  }

  void FreeString(PWSTR &s) {
    if (s) {
      CoTaskMemFree(s);
      s = nullptr;
    }
  }

  struct WatchdogArg {
    UnlockCredential *self;
    LONG gen;
  };
}  // namespace

const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *UnlockFieldDescriptors() {
  return s_fields;
}

UnlockCredential::UnlockCredential():
    _cRef(1),
    _cpus(CPUS_INVALID),
    _armed(false),
    _armGen(0),
    _account(nullptr),
    _password(nullptr),
    _userSid(nullptr) {
  DllAddRef();
  InitializeCriticalSection(&_lock);
}

UnlockCredential::~UnlockCredential() {
  ClearCredentials();
  FreeString(_userSid);
  DeleteCriticalSection(&_lock);
  DllRelease();
}

void UnlockCredential::ClearLocked() {
  FreeSecret(_password);
  FreeString(_account);
  _armed = false;
}

void UnlockCredential::ClearCredentials() {
  EnterCriticalSection(&_lock);
  ClearLocked();
  LeaveCriticalSection(&_lock);
}

void UnlockCredential::Disarm() {
  EnterCriticalSection(&_lock);
  _armGen++;  // makes any pending watchdog a no-op
  ClearLocked();
  LeaveCriticalSection(&_lock);
}

bool UnlockCredential::Cancel(const std::string &request_id) {
  EnterCriticalSection(&_lock);
  bool cancelled = _requestId == request_id && !_submissionPending;
  if (cancelled) {
    ++_armGen;
    ClearLocked();
  }
  LeaveCriticalSection(&_lock);
  return cancelled;
}

bool UnlockCredential::WasRejected() {
  EnterCriticalSection(&_lock);
  bool rejected = _rejected;
  LeaveCriticalSection(&_lock);
  return rejected;
}

void UnlockCredential::SetUserSid(PCWSTR sid) {
  EnterCriticalSection(&_lock);
  FreeString(_userSid);
  if (sid && *sid) {
    DupString(sid, &_userSid);
  }
  LeaveCriticalSection(&_lock);
}

DWORD WINAPI UnlockCredential::WatchdogThunk(LPVOID param) {
  auto *arg = (WatchdogArg *) param;
  arg->self->Watchdog(arg->gen);
  arg->self->Release();  // the reference Arm took for this thread
  delete arg;
  return 0;
}

// Gives LogonUI a while to consume the armed credential; if it never does (no Advise yet, the
// screen asleep, another tile chosen), clears it so the next request isn't refused as busy.
void UnlockCredential::Watchdog(LONG gen) {
  Sleep(ArmTimeoutMs);
  EnterCriticalSection(&_lock);
  if (_armed && _armGen == gen) {
    ClearLocked();
  }
  LeaveCriticalSection(&_lock);
}

bool UnlockCredential::Arm(PCWSTR account, PCWSTR password, const std::string &request_id) {
  EnterCriticalSection(&_lock);
  if (_armed || _submissionPending) {
    LeaveCriticalSection(&_lock);
    return false;
  }

  ClearLocked();
  bool ok = SUCCEEDED(DupString(account, &_account)) && SUCCEEDED(DupString(password, &_password));
  if (!ok) {
    ClearLocked();
    LeaveCriticalSection(&_lock);
    return false;
  }
  _rejected = false;
  _requestId = request_id;
  _armed = true;
  LONG gen = ++_armGen;
  LeaveCriticalSection(&_lock);

  // The watchdog holds a reference so it can't touch a freed credential.
  AddRef();
  auto *arg = new (std::nothrow) WatchdogArg {this, gen};
  HANDLE t = arg ? CreateThread(nullptr, 0, WatchdogThunk, arg, 0, nullptr) : nullptr;
  if (t) {
    CloseHandle(t);
  } else {
    delete arg;
    Release();
  }
  return true;
}

// IUnknown
IFACEMETHODIMP_(ULONG)
UnlockCredential::AddRef() {
  return InterlockedIncrement(&_cRef);
}

IFACEMETHODIMP_(ULONG)
UnlockCredential::Release() {
  LONG n = InterlockedDecrement(&_cRef);
  if (n == 0) {
    delete this;
  }
  return n;
}

IFACEMETHODIMP UnlockCredential::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) {
    return E_POINTER;
  }
  if (riid == IID_IUnknown || riid == __uuidof(ICredentialProviderCredential) || riid == __uuidof(ICredentialProviderCredential2)) {
    *ppv = static_cast<ICredentialProviderCredential2 *>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

IFACEMETHODIMP UnlockCredential::Advise(ICredentialProviderCredentialEvents *) {
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::UnAdvise() {
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::SetSelected(BOOL *pbAutoLogon) {
  // Armed, being selected means go: LogonUI calls GetSerialization at once.
  *pbAutoLogon = _armed ? TRUE : FALSE;
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::SetDeselected() {
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::GetFieldState(DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_STATE *pcpfs, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pcpfis) {
  if (dwFieldID >= VUF_NUM_FIELDS || !pcpfs || !pcpfis) {
    return E_INVALIDARG;
  }
  *pcpfs = CPFS_HIDDEN;  // nothing of ours ever shows
  *pcpfis = CPFIS_NONE;
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::GetStringValue(DWORD dwFieldID, PWSTR *ppwsz) {
  if (dwFieldID >= VUF_NUM_FIELDS || !ppwsz) {
    return E_INVALIDARG;
  }
  return DupString(L"", ppwsz);
}

IFACEMETHODIMP UnlockCredential::GetBitmapValue(DWORD, HBITMAP *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::GetCheckboxValue(DWORD, BOOL *, PWSTR *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::GetComboBoxValueCount(DWORD, DWORD *, DWORD *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::GetComboBoxValueAt(DWORD, DWORD, PWSTR *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::GetSubmitButtonValue(DWORD, DWORD *) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::SetStringValue(DWORD, PCWSTR) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::SetCheckboxValue(DWORD, BOOL) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::SetComboBoxSelectedValue(DWORD, DWORD) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::CommandLinkClicked(DWORD) {
  return E_NOTIMPL;
}

IFACEMETHODIMP UnlockCredential::GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpgsr, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs, PWSTR *ppwszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon) {
  if (ppwszOptionalStatusText) {
    *ppwszOptionalStatusText = nullptr;
  }
  if (pcpsiOptionalStatusIcon) {
    *pcpsiOptionalStatusIcon = CPSI_NONE;
  }

  EnterCriticalSection(&_lock);
  if (!_armed || !_account || !_password) {
    LeaveCriticalSection(&_lock);
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    return S_OK;
  }

  PWSTR domain = nullptr;
  PWSTR user = nullptr;
  PWSTR protectedPassword = nullptr;
  HRESULT hr = SplitDomainAndUsername(_account, &domain, &user);
  if (SUCCEEDED(hr)) {
    hr = ProtectIfNecessaryAndCopyPassword(_password, _cpus, &protectedPassword);
  }
  // Keep cancellation excluded until serialization is committed or fails.

  KERB_INTERACTIVE_UNLOCK_LOGON kiul;
  if (SUCCEEDED(hr)) {
    hr = KerbInteractiveUnlockLogonInit(domain, user, protectedPassword, _cpus, &kiul);
  }

  ULONG authPackage = 0;
  if (SUCCEEDED(hr)) {
    hr = RetrieveNegotiateAuthPackage(&authPackage);
  }

  BYTE *rgb = nullptr;
  DWORD cb = 0;
  if (SUCCEEDED(hr)) {
    hr = KerbInteractiveUnlockLogonPack(kiul, &rgb, &cb);
  }

  if (SUCCEEDED(hr)) {
    pcpcs->ulAuthenticationPackage = authPackage;
    pcpcs->clsidCredentialProvider = CLSID_VibepolloUnlock;
    pcpcs->rgbSerialization = rgb;
    pcpcs->cbSerialization = cb;
    _submissionPending = true;
    *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
  } else {
    // Nothing to submit: say so, so LogonUI doesn't act on an uninitialised response. S_OK keeps
    // the lock screen healthy.
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    if (rgb) {
      SecureZeroMemory(rgb, cb);
      CoTaskMemFree(rgb);
    }
    hr = S_OK;
  }

  FreeString(domain);
  FreeString(user);
  FreeSecret(protectedPassword);
  // Spent, win or lose: one request never submits twice.
  ClearLocked();
  LeaveCriticalSection(&_lock);
  return hr;
}

IFACEMETHODIMP UnlockCredential::ReportResult(NTSTATUS status, NTSTATUS, PWSTR *ppwszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon) {
  if (ppwszOptionalStatusText) {
    *ppwszOptionalStatusText = nullptr;
  }
  if (pcpsiOptionalStatusIcon) {
    *pcpsiOptionalStatusIcon = CPSI_NONE;
  }
  EnterCriticalSection(&_lock);
  // A successful report may precede the host observing the WTS transition. Never acknowledge
  // cancellation in that window. A failed report proves the submitted credential is spent.
  if (status < 0) {
    _submissionPending = false;
    _rejected = true;
  }
  ClearLocked();
  LeaveCriticalSection(&_lock);
  return S_OK;
}

IFACEMETHODIMP UnlockCredential::GetUserSid(PWSTR *ppszSid) {
  *ppszSid = nullptr;
  EnterCriticalSection(&_lock);
  HRESULT hr = S_FALSE;  // no SID known: a generic, "other user" credential
  if (_userSid && SUCCEEDED(DupString(_userSid, ppszSid))) {
    hr = S_OK;
  }
  LeaveCriticalSection(&_lock);
  return hr;
}
