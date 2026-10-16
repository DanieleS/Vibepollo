// Fork addition (Scirocco): bits shared across the provider DLL. The names it shares with the host
// live in src/platform/windows/session_unlock_contract.h.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "src/platform/windows/session_unlock_contract.h"

// The provider's CLSID, from the contract.
static const CLSID CLSID_VibepolloUnlock = {VIBEPOLLO_UNLOCK_CLSID_INIT};

// The one field the hidden credential carries. LogonUI wants at least one.
enum VIBEPOLLO_UNLOCK_FIELD_ID {
  VUF_LABEL = 0,
  VUF_NUM_FIELDS,
};

// Defined in dll.cpp: keeps the module loaded while a provider, a credential or one of their
// threads is alive.
void DllAddRef();
void DllRelease();
