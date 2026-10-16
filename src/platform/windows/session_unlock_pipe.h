/** Bounded, authenticated transport to the LogonUI credential provider. */
#pragma once

#include "session_unlock_contract.h"

#include <string>
#include <vector>
#include <windows.h>

namespace platf::session_unlock::pipe_transport {
  class handle {
  public:
    explicit handle(HANDLE value):
        value(value) {}

    ~handle() {
      if (value && value != INVALID_HANDLE_VALUE) {
        CloseHandle(value);
      }
    }

    handle(const handle &) = delete;
    handle &operator=(const handle &) = delete;
    HANDLE value;
  };

  struct result {
    std::string code = "unavailable";
    bool may_unlock = false;  // a write was attempted; a missing reply does not prove cancellation
  };

  inline bool trusted_server(HANDLE pipe, DWORD session_id) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) {
      return false;
    }
    handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value) {
      return false;
    }
    // Keep the process handle open while inspecting its image and token (no PID reuse).
    wchar_t image[32768] = {};
    DWORD length = ARRAYSIZE(image);
    wchar_t system_dir[MAX_PATH] = {};
    if (!QueryFullProcessImageNameW(process.value, 0, image, &length) || !GetSystemDirectoryW(system_dir, ARRAYSIZE(system_dir))) {
      return false;
    }
    std::wstring expected = std::wstring(system_dir) + L"\\LogonUI.exe";
    if (_wcsicmp(image, expected.c_str()) != 0) {
      return false;
    }
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process.value, TOKEN_QUERY, &raw_token)) {
      return false;
    }
    handle token(raw_token);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size) || !IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, WinLocalSystemSid)) {
      return false;
    }
    DWORD token_session = 0;
    return GetTokenInformation(token.value, TokenSessionId, &token_session, sizeof(token_session), &size) &&
           token_session == session_id;
  }

  inline bool transfer(HANDLE pipe, HANDLE event, void *buffer, DWORD size, bool write, ULONGLONG deadline, DWORD &transferred) {
    OVERLAPPED ov = {};
    ResetEvent(event);
    ov.hEvent = event;
    BOOL ok = write ? WriteFile(pipe, buffer, size, &transferred, &ov) :
                      ReadFile(pipe, buffer, size, &transferred, &ov);
    if (ok) {
      return true;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
      return false;
    }
    ULONGLONG now = GetTickCount64();
    DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    if (WaitForSingleObject(event, remaining) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &ov);
      // Cancellation must finish before the stack OVERLAPPED and buffer can be destroyed.
      GetOverlappedResult(pipe, &ov, &transferred, TRUE);
      return false;
    }
    return GetOverlappedResult(pipe, &ov, &transferred, FALSE) == TRUE;
  }

  inline result request(const char *op, DWORD session_id, const std::string &request_id) {
    result out;
    ULONGLONG deadline = GetTickCount64() + 3000;
    HANDLE raw_pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
      raw_pipe = CreateFileW(VIBEPOLLO_UNLOCK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
      if (raw_pipe != INVALID_HANDLE_VALUE) {
        break;
      }
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(VIBEPOLLO_UNLOCK_PIPE_NAME, 1000)) {
        return out;
      }
    }
    handle pipe(raw_pipe);
    if (raw_pipe == INVALID_HANDLE_VALUE || !trusted_server(raw_pipe, session_id)) {
      return out;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(raw_pipe, &mode, nullptr, nullptr)) {
      return out;
    }
    handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) {
      return out;
    }
    std::string message = std::string("{\"v\":") + std::to_string(VIBEPOLLO_UNLOCK_PROTOCOL_VERSION) +
                          ",\"op\":\"" + op + "\",\"id\":\"" + request_id + "\"}";
    DWORD transferred = 0;
    out.may_unlock = std::string(op) == "unlock";
    if (!transfer(raw_pipe, event.value, message.data(), static_cast<DWORD>(message.size()), true, deadline, transferred) || transferred != message.size()) {
      return out;
    }
    char reply[512] = {};
    if (!transfer(raw_pipe, event.value, reply, sizeof(reply), false, deadline, transferred)) {
      return out;
    }
    // Only accept exact protocol responses from the authenticated provider.
    std::string response(reply, transferred);
    if (response == "{\"ok\":true}") {
      out.code = "ok";
    } else {
      for (const char *code : {"cancelled", "rejected", "pending", "not-enrolled", "busy", "denied", "bad-request", "bad-version"}) {
        if (response == std::string("{\"ok\":false,\"code\":\"") + code + "\"}") {
          out.code = code;
          out.may_unlock = false;
          break;
        }
      }
    }
    return out;
  }
}  // namespace platf::session_unlock::pipe_transport
