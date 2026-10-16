#include "cred_packing.h"

#include <cstring>
#include <wincred.h>

HRESULT DupString(PCWSTR in, PWSTR *out) {
  size_t cb = (wcslen(in) + 1) * sizeof(wchar_t);
  *out = (PWSTR) CoTaskMemAlloc(cb);
  if (!*out) {
    return E_OUTOFMEMORY;
  }
  memcpy(*out, in, cb);
  return S_OK;
}

namespace {
  // A UNICODE_STRING pointing into an existing buffer (no allocation).
  void InitUnicodeString(UNICODE_STRING *us, PCWSTR src) {
    if (src) {
      size_t len = wcslen(src);
      us->Length = (USHORT) (len * sizeof(wchar_t));
      us->MaximumLength = (USHORT) ((len + 1) * sizeof(wchar_t));
      us->Buffer = (PWSTR) src;
    } else {
      us->Length = 0;
      us->MaximumLength = 0;
      us->Buffer = nullptr;
    }
  }

  // Copies a UNICODE_STRING's buffer into the packed region and rebases its pointer to an offset.
  void PackUnicodeString(const UNICODE_STRING &src, UNICODE_STRING &dst, BYTE *base, ULONG_PTR &offset) {
    dst.Length = src.Length;
    dst.MaximumLength = src.Length;
    if (src.Length) {
      memcpy(base + offset, src.Buffer, src.Length);
      dst.Buffer = (PWSTR) offset;  // an offset from the start of the blob; LSA rebases it
      offset += src.Length;
    } else {
      dst.Buffer = nullptr;
    }
  }
}  // namespace

HRESULT SplitDomainAndUsername(PCWSTR qualified, PWSTR *domain, PWSTR *user) {
  *domain = nullptr;
  *user = nullptr;

  const wchar_t *slash = wcschr(qualified, L'\\');
  size_t dlen = slash ? (size_t) (slash - qualified) : 0;
  // "." stands for the local machine: leave the domain empty so LSA uses this computer.
  bool local = !slash || (dlen == 1 && qualified[0] == L'.');

  PWSTR d = (PWSTR) CoTaskMemAlloc((local ? 1 : dlen + 1) * sizeof(wchar_t));
  if (!d) {
    return E_OUTOFMEMORY;
  }
  if (local) {
    d[0] = L'\0';
  } else {
    memcpy(d, qualified, dlen * sizeof(wchar_t));
    d[dlen] = L'\0';
  }

  HRESULT hr = DupString(slash ? slash + 1 : qualified, user);
  if (FAILED(hr)) {
    CoTaskMemFree(d);
    return hr;
  }
  *domain = d;
  return S_OK;
}

HRESULT RetrieveNegotiateAuthPackage(ULONG *authPackage) {
  *authPackage = 0;
  HANDLE lsa = nullptr;
  NTSTATUS status = LsaConnectUntrusted(&lsa);
  if (status != 0) {
    return HRESULT_FROM_NT(status);
  }

  static const char negotiate[] = "Negotiate";
  LSA_STRING name;
  name.Buffer = (PCHAR) negotiate;
  name.Length = (USHORT) (sizeof(negotiate) - 1);
  name.MaximumLength = (USHORT) sizeof(negotiate);

  ULONG package = 0;
  status = LsaLookupAuthenticationPackage(lsa, &name, &package);
  LsaDeregisterLogonProcess(lsa);
  if (status != 0) {
    return HRESULT_FROM_NT(status);
  }
  *authPackage = package;
  return S_OK;
}

HRESULT ProtectIfNecessaryAndCopyPassword(PCWSTR password, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, PWSTR *protectedPassword) {
  *protectedPassword = nullptr;
  if (!password || !*password) {
    return DupString(L"", protectedPassword);
  }

  CRED_PROTECTION_TYPE protectionType;
  if (CredIsProtectedW((LPWSTR) password, &protectionType) && protectionType != CredUnprotected) {
    return DupString(password, protectedPassword);
  }

  // CredUI and change-password want the plaintext; logon and unlock want it protected.
  if (cpus == CPUS_CREDUI || cpus == CPUS_CHANGE_PASSWORD) {
    return DupString(password, protectedPassword);
  }

  DWORD cch = 0;
  CredProtectW(FALSE, (LPWSTR) password, (DWORD) wcslen(password) + 1, nullptr, &cch, nullptr);
  if (cch == 0) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  PWSTR out = (PWSTR) CoTaskMemAlloc(cch * sizeof(wchar_t));
  if (!out) {
    return E_OUTOFMEMORY;
  }
  if (!CredProtectW(FALSE, (LPWSTR) password, (DWORD) wcslen(password) + 1, out, &cch, nullptr)) {
    CoTaskMemFree(out);
    return HRESULT_FROM_WIN32(GetLastError());
  }
  *protectedPassword = out;
  return S_OK;
}

HRESULT KerbInteractiveUnlockLogonInit(PCWSTR domain, PCWSTR username, PCWSTR password, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, KERB_INTERACTIVE_UNLOCK_LOGON *pkiul) {
  ZeroMemory(pkiul, sizeof(*pkiul));

  KERB_INTERACTIVE_LOGON *pkil = &pkiul->Logon;
  InitUnicodeString(&pkil->LogonDomainName, domain);
  InitUnicodeString(&pkil->UserName, username);
  InitUnicodeString(&pkil->Password, password);
  pkil->MessageType = cpus == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon;
  return S_OK;
}

HRESULT KerbInteractiveUnlockLogonPack(const KERB_INTERACTIVE_UNLOCK_LOGON &kiulIn, BYTE **prgb, DWORD *pcb) {
  *prgb = nullptr;
  *pcb = 0;

  const KERB_INTERACTIVE_LOGON &in = kiulIn.Logon;
  DWORD cb = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + in.LogonDomainName.Length + in.UserName.Length + in.Password.Length;

  BYTE *rgb = (BYTE *) CoTaskMemAlloc(cb);
  if (!rgb) {
    return E_OUTOFMEMORY;
  }
  ZeroMemory(rgb, cb);

  auto *out = (KERB_INTERACTIVE_UNLOCK_LOGON *) rgb;
  out->Logon.MessageType = in.MessageType;

  ULONG_PTR offset = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
  PackUnicodeString(in.LogonDomainName, out->Logon.LogonDomainName, rgb, offset);
  PackUnicodeString(in.UserName, out->Logon.UserName, rgb, offset);
  PackUnicodeString(in.Password, out->Logon.Password, rgb, offset);

  *prgb = rgb;
  *pcb = cb;
  return S_OK;
}
