/**
 * @file src/platform/windows/session_unlock_contract.h
 * @brief The names the host and its lock-screen credential provider share.
 *
 * Fork addition (Scirocco). Unlocking a locked session takes a credential provider: a DLL that
 * Windows loads into LogonUI, the only place allowed to hand Windows credentials on the lock
 * screen. Both sides run as SYSTEM, so the design is small:
 *
 * - the host keeps the account and its password as LSA private data, readable only by SYSTEM;
 * - the provider, while the lock screen is up, serves a named pipe that only SYSTEM may open;
 * - the host writes one request on it, and the provider reads the secrets itself, arms its hidden
 *   credential and has LogonUI submit it. The password never crosses the pipe.
 *
 * Built into both tools/unlock_provider (the DLL) and src/platform/windows/session_unlock.cpp.
 * Change a name here and both sides change with it.
 */
#pragma once

// The provider's COM class. Fixed: the host registers it under this CLSID, and an installed
// provider is found and removed by it.
//   {28F4FEFF-97F3-4EC9-8D56-192763B039C4}
#define VIBEPOLLO_UNLOCK_CLSID_STRING L"{28F4FEFF-97F3-4EC9-8D56-192763B039C4}"
#define VIBEPOLLO_UNLOCK_CLSID_INIT 0x28f4feff, 0x97f3, 0x4ec9, {0x8d, 0x56, 0x19, 0x27, 0x63, 0xb0, 0x39, 0xc4}

// The name Windows lists the provider under (registry only; its tile is never shown).
#define VIBEPOLLO_UNLOCK_FRIENDLY_NAME L"Vibepollo Unlock"

// The provider DLL, next to the host's other tools.
#define VIBEPOLLO_UNLOCK_DLL_NAME L"VibepolloUnlock.dll"

// Served by the provider while an unlock scenario is up; message mode, SYSTEM only.
#define VIBEPOLLO_UNLOCK_PIPE_NAME L"\\\\.\\pipe\\VibepolloUnlock"

// LSA private data. The "L$" prefix makes them local secrets: only SYSTEM can read them.
//   account:  the account LogonUI signs in as, "DOMAIN\user" ("MicrosoftAccount\name@example.com"
//             for a Microsoft account, ".\user" for a local one);
//   password: its password;
//   sid:      its SID as a string, so the provider binds its credential to that user's tile.
#define VIBEPOLLO_UNLOCK_ACCOUNT_KEY L"L$VibepolloUnlockAccount"
#define VIBEPOLLO_UNLOCK_PASSWORD_KEY L"L$VibepolloUnlockPassword"
#define VIBEPOLLO_UNLOCK_SID_KEY L"L$VibepolloUnlockSid"

// The wire protocol, one request and one response, both UTF-8 JSON:
//   host → provider:  {"v":2,"op":"unlock","id":"{request GUID}"}
//   provider → host:  {"ok":true}  or  {"ok":false,"code":"<code>"}
// with code one of denied (the caller isn't SYSTEM), bad-version, bad-request, not-enrolled (no
// account stored) and busy (an unlock is already armed, or LogonUI can't be told to submit).
// "ok" means the credential was armed and LogonUI asked to submit it, not that the password was
// right: the host learns that from the session turning unlocked.
// Every request includes a fresh GUID id. Cancel must name the same accepted request;
// a new provider instance must never confirm cancellation of an unknown id.
// v2 adds op "cancel": "cancelled" proves that the credential was disarmed before
// serialization; "rejected" confirms a failed Windows result. "pending" means Windows may
// still unlock. Lost replies are ambiguous too.
// The host must retain responsibility for a pending unlock until it observes the WTS transition
// or receives a confirmed cancellation. A successful submission must never acknowledge cancel.
#define VIBEPOLLO_UNLOCK_PROTOCOL_VERSION 2
