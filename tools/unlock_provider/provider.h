// Fork addition (Scirocco). The credential provider. It takes part only in unlocking a locked
// session, contributes no tile until the host asks for an unlock over the pipe, and then one hidden
// credential that LogonUI submits on its own. Ported from CouchPilot's CouchPilotProvider.
#pragma once

#include "common.h"
#include "credential.h"
#include "pipe_server.h"

#include <credentialprovider.h>

class UnlockProvider: public ICredentialProvider {
public:
  UnlockProvider();

  // IUnknown
  IFACEMETHODIMP_(ULONG)
  AddRef() override;
  IFACEMETHODIMP_(ULONG)
  Release() override;
  IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv) override;

  // ICredentialProvider
  IFACEMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags) override;
  IFACEMETHODIMP SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs) override;
  IFACEMETHODIMP Advise(ICredentialProviderEvents *pcpe, UINT_PTR upAdviseContext) override;
  IFACEMETHODIMP UnAdvise() override;
  IFACEMETHODIMP GetFieldDescriptorCount(DWORD *pdwCount) override;
  IFACEMETHODIMP GetFieldDescriptorAt(DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR **ppcpfd) override;
  IFACEMETHODIMP GetCredentialCount(DWORD *pdwCount, DWORD *pdwDefault, BOOL *pbAutoLogonWithDefault) override;
  IFACEMETHODIMP GetCredentialAt(DWORD dwIndex, ICredentialProviderCredential **ppcpc) override;

private:
  virtual ~UnlockProvider();

  // The pipe handler: read the stored account, arm the credential, wake LogonUI.
  UnlockOutcome OnUnlock(const std::string &request_id);

  LONG _cRef;
  UnlockCredential *_cred;
  PipeServer _pipe;

  CRITICAL_SECTION _lock;
  ICredentialProviderEvents *_pEvents;
  UINT_PTR _adviseContext;
};
