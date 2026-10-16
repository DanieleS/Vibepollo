// Real Windows API regression tests; they never sign in, unlock or register a provider.
#include "src/platform/windows/session_unlock_pipe.h"
#include "tools/unlock_provider/credential.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

// The executable owns its lifetime; the DLL's module-reference accounting is irrelevant here.
void DllAddRef() {}

void DllRelease() {}

namespace {
  using namespace platf::session_unlock;

  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s (Win32 error %lu)\n", message, GetLastError());
      std::exit(1);
    }
  }

  const std::string request_id = "{11111111-1111-4111-8111-111111111111}";
  const std::string other_id = "{22222222-2222-4222-8222-222222222222}";

  void free_serialization(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION &serialization) {
    if (serialization.rgbSerialization) {
      SecureZeroMemory(serialization.rgbSerialization, serialization.cbSerialization);
      CoTaskMemFree(serialization.rgbSerialization);
    }
  }

  void cancellation() {
    auto *credential = new UnlockCredential;
    credential->SetScenario(CPUS_UNLOCK_WORKSTATION);
    require(!credential->Cancel(request_id), "a new provider cannot cancel an unknown request");
    require(credential->Arm(L".\\test", L"test-only", request_id), "arm");
    require(!credential->Cancel(other_id), "cancellation must match the accepted request ID");
    require(credential->Cancel(request_id), "unsubmitted credentials can be cancelled");
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serialization = {};
    require(SUCCEEDED(credential->GetSerialization(&response, &serialization, nullptr, nullptr)), "serialization after cancel");
    require(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED, "cancelled credential must never serialize");
    credential->Release();
  }

  void submitted() {
    auto *credential = new UnlockCredential;
    credential->SetScenario(CPUS_UNLOCK_WORKSTATION);
    require(credential->Arm(L".\\test", L"test-only", request_id), "arm for serialization");
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serialization = {};
    require(SUCCEEDED(credential->GetSerialization(&response, &serialization, nullptr, nullptr)), "serialize");
    require(response == CPGSR_RETURN_CREDENTIAL_FINISHED, "Windows packing must succeed");
    free_serialization(serialization);
    require(!credential->Cancel(request_id), "a submitted credential cannot acknowledge cancellation");
    credential->ReportResult(0, 0, nullptr, nullptr);
    require(!credential->Cancel(request_id), "success before WTS unlock must remain pending");
    credential->Disarm();
    require(!credential->Cancel(request_id), "UnAdvise cannot retract an already submitted credential");
    credential->ReportResult(static_cast<NTSTATUS>(0xc000006dUL), 0, nullptr, nullptr);
    require(credential->Cancel(request_id), "a failed Windows result proves the credential is spent");
    require(credential->WasRejected(), "a rejected Windows result must remain distinguishable from timeout");
    credential->Release();
  }

  void cancellation_race() {
    for (int i = 0; i < 32; ++i) {
      auto *credential = new UnlockCredential;
      credential->SetScenario(CPUS_UNLOCK_WORKSTATION);
      require(credential->Arm(L".\\test", L"test-only", request_id), "race arm");
      bool cancelled = false;
      CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response;
      CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION serialization = {};
      std::thread cancel([&] {
        cancelled = credential->Cancel(request_id);
      });
      credential->GetSerialization(&response, &serialization, nullptr, nullptr);
      cancel.join();
      require(!(cancelled && response == CPGSR_RETURN_CREDENTIAL_FINISHED), "cancel acknowledgement must be mutually exclusive with Windows serialization");
      free_serialization(serialization);
      credential->Release();
    }
  }

  void transport() {
    std::wstring name = L"\\\\.\\pipe\\VibepolloUnlockTest-" + std::to_wstring(GetCurrentProcessId());
    pipe_transport::handle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 512, 512, 0, nullptr));
    require(server.value != INVALID_HANDLE_VALUE, "test pipe");
    pipe_transport::handle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    require(client.value != INVALID_HANDLE_VALUE, "test client");
    pipe_transport::handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(event.value != nullptr, "test event");
    DWORD session_id = 0;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &session_id), "test session");
    require(!pipe_transport::trusted_server(client.value, session_id), "a pipe squatter is not LogonUI");

    char buffer[32] = {};
    DWORD transferred = 0;
    ULONGLONG started = GetTickCount64();
    require(!pipe_transport::transfer(client.value, event.value, buffer, sizeof(buffer), false, started + 100, transferred), "an unresponsive server must time out");
    require(GetTickCount64() - started < 2000, "read timeout must be bounded");
    // Reuse the same event/buffer after cancellation; catches outstanding-I/O lifetime bugs.
    require(pipe_transport::transfer(client.value, event.value, buffer, 1, true, GetTickCount64() + 1000, transferred), "write after cancelled read");
    require(pipe_transport::transfer(server.value, event.value, buffer, sizeof(buffer), false, GetTickCount64() + 1000, transferred), "server read");
    require(ImpersonateNamedPipeClient(server.value), "identify test client");
    HANDLE raw_token = nullptr;
    require(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token), "identification token");
    pipe_transport::handle token(raw_token);
    SECURITY_IMPERSONATION_LEVEL level;
    DWORD size = 0;
    require(GetTokenInformation(token.value, TokenImpersonationLevel, &level, sizeof(level), &size), "token level");
    require(RevertToSelf(), "revert test identity");
    require(level == SecurityIdentification, "pipe server must not receive an impersonation-capable token");

    std::vector<char> large(1024 * 1024);
    started = GetTickCount64();
    require(!pipe_transport::transfer(client.value, event.value, large.data(), static_cast<DWORD>(large.size()), true, started + 100, transferred), "a server not reading must time out the write");
    require(GetTickCount64() - started < 2000, "write timeout must be bounded");
  }
}  // namespace

int main() {
  cancellation();
  submitted();
  cancellation_race();
  transport();
  std::puts("session unlock Windows regressions: all checks passed");
}
