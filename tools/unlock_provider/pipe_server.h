// Fork addition (Scirocco). The lock-screen end of the pipe: while an unlock scenario is up it
// listens on VIBEPOLLO_UNLOCK_PIPE_NAME, and answers one request per connection. Only SYSTEM can
// open the pipe, and the caller's token is checked again on top of that.
#pragma once

#include "common.h"

#include <functional>

// What the provider makes of a well-formed request from SYSTEM. Transport-level outcomes
// (denied, bad-request, bad-version) are decided by the server itself.
enum class UnlockOutcome {
  ok,
  busy,
  not_enrolled,
};

class PipeServer {
public:
  // Runs on the pipe thread: reads the stored account, arms the credential, wakes LogonUI.
  using Handler = std::function<UnlockOutcome()>;

  PipeServer() = default;
  ~PipeServer();
  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  // Idempotent. Starts the listener thread.
  bool Start(Handler handler);
  void Stop();

private:
  static DWORD WINAPI Thunk(LPVOID self);
  void Run();
  void ServeOne(HANDLE pipe, HANDLE ioEvent);

  Handler _handler;
  HANDLE _thread = nullptr;
  HANDLE _stopEvent = nullptr;
  volatile bool _running = false;
};
