// Fork addition (Scirocco). The hidden credential. It has no visible fields and does nothing until
// the provider arms it with the stored account; then its GetSerialization returns the packed KERB
// logon and Windows unlocks. Ported from CouchPilot's CouchPilotCredential.
#pragma once

#include "common.h"

#include <credentialprovider.h>
#include <string>

class UnlockCredential: public ICredentialProviderCredential2 {
public:
  UnlockCredential();

  // IUnknown
  IFACEMETHODIMP_(ULONG)
  AddRef() override;
  IFACEMETHODIMP_(ULONG)
  Release() override;
  IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override;

  // ICredentialProviderCredential
  IFACEMETHODIMP Advise(ICredentialProviderCredentialEvents *pcpce) override;
  IFACEMETHODIMP UnAdvise() override;
  IFACEMETHODIMP SetSelected(BOOL *pbAutoLogon) override;
  IFACEMETHODIMP SetDeselected() override;
  IFACEMETHODIMP GetFieldState(DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_STATE *pcpfs, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pcpfis) override;
  IFACEMETHODIMP GetStringValue(DWORD dwFieldID, PWSTR *ppwsz) override;
  IFACEMETHODIMP GetBitmapValue(DWORD dwFieldID, HBITMAP *phbmp) override;
  IFACEMETHODIMP GetCheckboxValue(DWORD dwFieldID, BOOL *pbChecked, PWSTR *ppwszLabel) override;
  IFACEMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, DWORD *pcItems, DWORD *pdwSelectedItem) override;
  IFACEMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR *ppwszItem) override;
  IFACEMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, DWORD *pdwAdjacentTo) override;
  IFACEMETHODIMP SetStringValue(DWORD dwFieldID, PCWSTR pwz) override;
  IFACEMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) override;
  IFACEMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) override;
  IFACEMETHODIMP CommandLinkClicked(DWORD dwFieldID) override;
  IFACEMETHODIMP GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpgsr, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs, PWSTR *ppwszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon) override;
  IFACEMETHODIMP ReportResult(NTSTATUS ntsStatus, NTSTATUS ntsSubstatus, PWSTR *ppwszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon) override;

  // ICredentialProviderCredential2
  IFACEMETHODIMP GetUserSid(PWSTR *ppszSid) override;

  // Called from the provider, on the pipe thread. Copies the credentials and arms the tile; the
  // provider then has LogonUI submit it. False if an unlock is already armed. A watchdog disarms
  // it if LogonUI never consumes it, so a request that goes nowhere neither wedges the tile at
  // busy nor leaves the password in memory.
  bool Arm(PCWSTR account, PCWSTR password, const std::string &request_id);

  bool IsArmed() const {
    return _armed;
  }

  // Force-clear: on UnAdvise, a scenario change, or the watchdog.
  void Disarm();
  // True only when no serialization can still unlock Windows.
  bool Cancel(const std::string &request_id);
  bool WasRejected();

  void SetScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus) {
    _cpus = cpus;
  }

  // The enrolled account's SID, so the credential binds to the locked user's tile.
  void SetUserSid(PCWSTR sid);

private:
  virtual ~UnlockCredential();
  void ClearLocked();  // assumes _lock is held
  void ClearCredentials();  // takes _lock
  void Watchdog(LONG gen);
  static DWORD WINAPI WatchdogThunk(LPVOID param);

  static const DWORD ArmTimeoutMs = 30000;  // an armed credential LogonUI never takes disarms itself

  LONG _cRef;
  CREDENTIAL_PROVIDER_USAGE_SCENARIO _cpus;
  CRITICAL_SECTION _lock;
  volatile bool _armed;
  bool _rejected = false;
  std::string _requestId;
  bool _submissionPending = false;  // remains true after successful submission until this provider dies
  LONG _armGen;  // bumped on each Arm/Disarm, so a stale watchdog does nothing
  PWSTR _account;
  PWSTR _password;
  PWSTR _userSid;
};
