#include "lsa_secrets.h"

#include <ntsecapi.h>

std::wstring ReadLsaSecret(const wchar_t *key) {
  LSA_OBJECT_ATTRIBUTES attrs;
  ZeroMemory(&attrs, sizeof(attrs));

  LSA_HANDLE policy = nullptr;
  if (LsaOpenPolicy(nullptr, &attrs, POLICY_GET_PRIVATE_INFORMATION, &policy) != 0) {
    return {};
  }

  LSA_UNICODE_STRING name;
  size_t len = wcslen(key);
  name.Length = (USHORT) (len * sizeof(wchar_t));
  name.MaximumLength = (USHORT) ((len + 1) * sizeof(wchar_t));
  name.Buffer = (PWSTR) key;

  PLSA_UNICODE_STRING data = nullptr;
  NTSTATUS status = LsaRetrievePrivateData(policy, &name, &data);
  LsaClose(policy);
  if (status != 0 || !data) {
    return {};
  }

  std::wstring value(data->Buffer, data->Length / sizeof(wchar_t));
  SecureZeroMemory(data->Buffer, data->MaximumLength);
  LsaFreeMemory(data);
  return value;
}

void WipeString(std::wstring &s) {
  if (!s.empty()) {
    SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
  }
  s.clear();
}
