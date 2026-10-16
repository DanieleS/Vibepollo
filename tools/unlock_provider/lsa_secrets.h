// Fork addition (Scirocco). Reads the LSA private data the host stores (see the contract header).
#pragma once

#include "common.h"

#include <string>

// The secret's value, or an empty string when it isn't stored. The caller wipes it after use.
std::wstring ReadLsaSecret(const wchar_t *key);

// Overwrites a string's buffer before it goes away.
void WipeString(std::wstring &s);
