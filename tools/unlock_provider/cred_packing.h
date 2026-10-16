// Fork addition (Scirocco). Turning an account and password into the blob LSA unlocks with: the
// well-trodden helpers from Microsoft's credential-provider samples, trimmed to the unlock path
// (an interactive KERB logon, the password run through CredProtect, marshalled into one buffer for
// CPGSR_RETURN_CREDENTIAL_FINISHED). Ported from CouchPilot's CredPacking.
#pragma once

#include "common.h"

#include <credentialprovider.h>
#include <ntsecapi.h>

// Splits "DOMAIN\user" (or "user", or ".\user") into its parts. A bare name or "." yields an empty
// domain, which LSA reads as the local machine. Both outputs are CoTaskMemAlloc'd.
HRESULT SplitDomainAndUsername(PCWSTR qualified, PWSTR *domain, PWSTR *user);

// Fills a KERB_INTERACTIVE_UNLOCK_LOGON for the scenario, pointing its strings at the arguments
// (no copy yet; Pack copies).
HRESULT KerbInteractiveUnlockLogonInit(PCWSTR domain, PCWSTR username, PCWSTR password, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, KERB_INTERACTIVE_UNLOCK_LOGON *pkiul);

// Marshals the logon into one CoTaskMemAlloc'd buffer, strings after the struct and their pointers
// turned into offsets, the way LSA expects.
HRESULT KerbInteractiveUnlockLogonPack(const KERB_INTERACTIVE_UNLOCK_LOGON &kiulIn, BYTE **prgb, DWORD *pcb);

// Runs the password through CredProtect unless it already is, into a CoTaskMemAlloc'd copy.
HRESULT ProtectIfNecessaryAndCopyPassword(PCWSTR password, CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, PWSTR *protectedPassword);

// The negotiate package's id, which the logon blob names.
HRESULT RetrieveNegotiateAuthPackage(ULONG *authPackage);

// A CoTaskMemAlloc'd copy of a string.
HRESULT DupString(PCWSTR in, PWSTR *out);
