// Fork addition (Scirocco). Ported from CouchPilot's PipeServer, with the handshake gone: both ends
// run as SYSTEM and nothing secret crosses the pipe, so the DACL (SYSTEM only) and a check of the
// caller's token are the whole of the authentication.
#include "pipe_server.h"

#include <aclapi.h>
#include <string>
#include <vector>

namespace {
  // A security descriptor that lets only SYSTEM open the pipe.
  PSECURITY_DESCRIPTOR BuildPipeSd(PACL *aclOut) {
    PSID systemSid = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &systemSid)) {
      return nullptr;
    }

    EXPLICIT_ACCESSW ea;
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = FILE_ALL_ACCESS;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR) systemSid;

    PACL acl = nullptr;
    DWORD err = SetEntriesInAclW(1, &ea, nullptr, &acl);
    FreeSid(systemSid);
    if (err != ERROR_SUCCESS) {
      return nullptr;
    }

    auto sd = (PSECURITY_DESCRIPTOR) LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!sd || !InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)) {
      if (sd) {
        LocalFree(sd);
      }
      LocalFree(acl);
      return nullptr;
    }
    *aclOut = acl;
    return sd;
  }

  // Waits for an overlapped op, or for Stop. Returns 1 done (bytes in *transferred), 0 stop,
  // -1 error.
  int AwaitOverlapped(HANDLE pipe, HANDLE stopEvent, OVERLAPPED *ov, DWORD *transferred, DWORD timeoutMs = INFINITE) {
    HANDLE waits[2] = {ov->hEvent, stopEvent};
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
    if (w != WAIT_OBJECT_0) {
      // Stop, a timeout or a failed wait. Cancel the I/O and let the cancellation land before
      // returning: the OVERLAPPED and the buffer live on the caller's stack, and the kernel
      // writes to both when the request completes, cancelled or not.
      CancelIoEx(pipe, ov);
      DWORD n = 0;
      GetOverlappedResult(pipe, ov, &n, TRUE);
      return w == WAIT_OBJECT_0 + 1 ? 0 : -1;
    }
    DWORD n = 0;
    if (!GetOverlappedResult(pipe, ov, &n, FALSE)) {
      return -1;
    }
    if (transferred) {
      *transferred = n;
    }
    return 1;
  }

  // The second lock after the DACL: the connected client must itself run as SYSTEM.
  bool CallerIsSystem(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) {
      return false;
    }

    bool ok = false;
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
      DWORD cb = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &cb);
      if (cb) {
        std::vector<BYTE> buf(cb);
        if (GetTokenInformation(token, TokenUser, buf.data(), cb, &cb)) {
          auto *tu = (TOKEN_USER *) buf.data();
          ok = IsWellKnownSid(tu->User.Sid, WinLocalSystemSid) == TRUE;
        }
      }
      CloseHandle(token);
    }
    RevertToSelf();
    return ok;
  }

  // The request is tiny and produced by the host; a field lookup is all the parsing it needs.
  bool JsonInt(const std::string &json, const char *key, int &out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) {
      return false;
    }
    size_t c = json.find(':', k + needle.size());
    if (c == std::string::npos) {
      return false;
    }
    out = atoi(json.c_str() + c + 1);
    return true;
  }

  bool JsonHasString(const std::string &json, const char *key, const char *value) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) {
      return false;
    }
    size_t c = json.find(':', k + needle.size());
    if (c == std::string::npos) {
      return false;
    }
    size_t q = json.find('"', c + 1);
    if (q == std::string::npos) {
      return false;
    }
    std::string expected = std::string(value) + "\"";
    return json.compare(q + 1, expected.size(), expected) == 0;
  }

  // Reads one message, honouring Stop and giving the client a few seconds. False on stop or error.
  bool ReadMsg(HANDLE pipe, HANDLE stopEvent, HANDLE ioEvent, std::string &out) {
    char buf[512];
    ResetEvent(ioEvent);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = ioEvent;
    DWORD read = 0;
    BOOL ok = ReadFile(pipe, buf, sizeof(buf), &read, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      if (AwaitOverlapped(pipe, stopEvent, &ov, &read, 5000) != 1) {
        return false;
      }
      ok = TRUE;
    }
    if (!ok || read == 0) {
      return false;
    }
    out.assign(buf, read);
    return true;
  }

  bool WriteMsg(HANDLE pipe, HANDLE stopEvent, HANDLE ioEvent, const char *json) {
    ResetEvent(ioEvent);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = ioEvent;
    DWORD written = 0;
    BOOL ok = WriteFile(pipe, json, (DWORD) strlen(json), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      ok = AwaitOverlapped(pipe, stopEvent, &ov, &written, 5000) == 1;
    }
    return ok == TRUE;
  }
}  // namespace

PipeServer::~PipeServer() {
  Stop();
}

DWORD WINAPI PipeServer::Thunk(LPVOID self) {
  ((PipeServer *) self)->Run();
  return 0;
}

bool PipeServer::Start(Handler handler) {
  if (_running) {
    return true;
  }

  _handler = std::move(handler);
  _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
  if (!_stopEvent) {
    return false;
  }

  _running = true;
  _thread = CreateThread(nullptr, 0, Thunk, this, 0, nullptr);
  if (!_thread) {
    _running = false;
    CloseHandle(_stopEvent);
    _stopEvent = nullptr;
    return false;
  }
  return true;
}

void PipeServer::Stop() {
  if (!_running) {
    return;
  }
  _running = false;
  if (_stopEvent) {
    SetEvent(_stopEvent);  // wakes the overlapped waits and the retry sleep
  }
  if (_thread) {
    WaitForSingleObject(_thread, INFINITE);
    CloseHandle(_thread);
    _thread = nullptr;
  }
  if (_stopEvent) {
    CloseHandle(_stopEvent);
    _stopEvent = nullptr;
  }
}

void PipeServer::Run() {
  while (_running) {
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR sd = BuildPipeSd(&acl);
    if (!sd) {
      WaitForSingleObject(_stopEvent, 500);
      continue;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = sd;

    // FIRST_PIPE_INSTANCE: if something squatted the name, this fails rather than sharing it. We
    // then never serve, and the normal sign-in methods are unaffected.
    HANDLE pipe = CreateNamedPipeW(
      VIBEPOLLO_UNLOCK_PIPE_NAME,
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,
      512,
      512,
      0,
      &sa
    );

    LocalFree(sd);
    LocalFree(acl);

    if (pipe == INVALID_HANDLE_VALUE) {
      WaitForSingleObject(_stopEvent, 500);
      continue;
    }

    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent) {
      CloseHandle(pipe);
      WaitForSingleObject(_stopEvent, 500);
      continue;
    }

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = ioEvent;

    BOOL connected = ConnectNamedPipe(pipe, &ov);
    DWORD err = GetLastError();
    bool ready = false;
    if (connected || err == ERROR_PIPE_CONNECTED) {
      ready = true;
    } else if (err == ERROR_IO_PENDING) {
      int r = AwaitOverlapped(pipe, _stopEvent, &ov, nullptr);
      if (r == 0) {
        CloseHandle(ioEvent);
        CloseHandle(pipe);
        break;  // stop
      }
      ready = r == 1;
    }

    if (ready && _running) {
      ServeOne(pipe, ioEvent);
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(ioEvent);
    CloseHandle(pipe);
  }
}

void PipeServer::ServeOne(HANDLE pipe, HANDLE ioEvent) {
  std::string request;
  if (!ReadMsg(pipe, _stopEvent, ioEvent, request)) {
    return;
  }

  if (!CallerIsSystem(pipe)) {
    WriteMsg(pipe, _stopEvent, ioEvent, "{\"ok\":false,\"code\":\"denied\"}");
    return;
  }

  int v = 0;
  if (!JsonInt(request, "v", v) || !JsonHasString(request, "op", "unlock")) {
    WriteMsg(pipe, _stopEvent, ioEvent, "{\"ok\":false,\"code\":\"bad-request\"}");
    return;
  }
  if (v != VIBEPOLLO_UNLOCK_PROTOCOL_VERSION) {
    WriteMsg(pipe, _stopEvent, ioEvent, "{\"ok\":false,\"code\":\"bad-version\"}");
    return;
  }

  UnlockOutcome outcome = _handler ? _handler() : UnlockOutcome::busy;
  const char *reply = "{\"ok\":false,\"code\":\"busy\"}";
  if (outcome == UnlockOutcome::ok) {
    reply = "{\"ok\":true}";
  } else if (outcome == UnlockOutcome::not_enrolled) {
    reply = "{\"ok\":false,\"code\":\"not-enrolled\"}";
  }
  WriteMsg(pipe, _stopEvent, ioEvent, reply);
}
