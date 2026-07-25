
/**
 * @file process_handler.cpp
 * @brief Implements the ProcessHandler class for managing process creation and control on Windows.
 *
 * This file provides the implementation for starting, waiting, and terminating processes,
 * including support for attribute lists and impersonation as needed for the Sunshine project.
 */

// local includes (include our header first to enforce correct include order)
#include "process_handler.h"

// platform includes
#include <UserEnv.h>
#include <windows.h>
#include <WtsApi32.h>

// standard includes
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

#include <algorithm>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace {

  constexpr wchar_t kDefaultDesktopW[] = L"winsta0\\default";
  constexpr wchar_t kWinlogonDesktopW[] = L"winsta0\\winlogon";

  /// One end of a captured stream, plus the inheritable write end the child gets.
  struct capture_pipe_t {
    winrt::handle read;
    winrt::handle write;
  };

  /// Create an anonymous pipe whose **write** end is inheritable and whose read
  /// end explicitly is not: the child must not hold a handle to the reader, or
  /// the parent would never see end-of-stream when the child dies.
  std::optional<capture_pipe_t> make_capture_pipe() {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
      BOOST_LOG(error) << "CreatePipe failed while preparing helper capture, winerr=" << GetLastError();
      return std::nullopt;
    }
    capture_pipe_t pipe {winrt::handle {read_end}, winrt::handle {write_end}};
    if (!SetHandleInformation(pipe.read.get(), HANDLE_FLAG_INHERIT, 0)) {
      BOOST_LOG(error) << "SetHandleInformation failed on a helper capture pipe, winerr=" << GetLastError();
      return std::nullopt;
    }
    return pipe;
  }

  /// Owns the `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` allocation for the lifetime of
  /// a `CreateProcess*` call. Without this list, `bInheritHandles=TRUE` would
  /// hand the child *every* inheritable handle this process holds — and as a
  /// service that is a great deal more than two pipe ends.
  class handle_list_attribute_t {
  public:
    ~handle_list_attribute_t() {
      if (list_) {
        DeleteProcThreadAttributeList(list_);
      }
    }

    handle_list_attribute_t(const handle_list_attribute_t &) = delete;
    handle_list_attribute_t &operator=(const handle_list_attribute_t &) = delete;

    handle_list_attribute_t() = default;

    /// Build the list over `handles`, which must outlive this object and the
    /// `CreateProcess*` call it is passed to.
    bool init(STARTUPINFOEXW &si, std::vector<HANDLE> &handles) {
      SIZE_T size = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
      if (size == 0) {
        BOOST_LOG(error) << "InitializeProcThreadAttributeList sizing failed, winerr=" << GetLastError();
        return false;
      }
      storage_.resize(size);
      list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
      if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
        BOOST_LOG(error) << "InitializeProcThreadAttributeList failed, winerr=" << GetLastError();
        list_ = nullptr;
        return false;
      }
      if (!UpdateProcThreadAttribute(
            list_,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles.data(),
            handles.size() * sizeof(HANDLE),
            nullptr,
            nullptr
          )) {
        BOOST_LOG(error) << "UpdateProcThreadAttribute(HANDLE_LIST) failed, winerr=" << GetLastError();
        return false;
      }
      si.lpAttributeList = list_;
      return true;
    }

  private:
    std::vector<char> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
  };

  bool start_as_system_in_active_console_session(
    const std::wstring &cmd_line,
    const std::wstring &working_dir,
    DWORD creation_flags,
    BOOL inherit_handles,
    STARTUPINFOEXW &si,
    PROCESS_INFORMATION &pi
  ) {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF) {
      BOOST_LOG(debug) << "Active console session id unavailable; cannot launch SYSTEM child in active session.";
      return false;
    }

    HANDLE raw_process_token = nullptr;
    if (!OpenProcessToken(
          GetCurrentProcess(),
          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_SESSIONID,
          &raw_process_token
        )) {
      BOOST_LOG(debug) << "OpenProcessToken failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }
    auto close_process_token = util::fail_guard([&]() {
      CloseHandle(raw_process_token);
    });

    HANDLE raw_primary_token = nullptr;
    if (!DuplicateTokenEx(
          raw_process_token,
          MAXIMUM_ALLOWED,
          nullptr,
          SecurityImpersonation,
          TokenPrimary,
          &raw_primary_token
        )) {
      BOOST_LOG(debug) << "DuplicateTokenEx failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }
    auto close_primary_token = util::fail_guard([&]() {
      CloseHandle(raw_primary_token);
    });

    if (!SetTokenInformation(raw_primary_token, TokenSessionId, (PVOID) &session_id, sizeof(session_id))) {
      BOOST_LOG(debug) << "SetTokenInformation(TokenSessionId=" << session_id
                       << ") failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      return false;
    }

    void *env_block = nullptr;
    if (!CreateEnvironmentBlock(&env_block, raw_primary_token, FALSE)) {
      BOOST_LOG(debug) << "CreateEnvironmentBlock failed while launching SYSTEM child in active session, winerr=" << GetLastError();
      env_block = nullptr;
    }
    auto destroy_env = util::fail_guard([&]() {
      if (env_block) {
        DestroyEnvironmentBlock(env_block);
      }
    });

    // Ensure the child runs on the interactive window station/desktop for the active console session.
    auto *prev_desktop = si.StartupInfo.lpDesktop;
    const auto try_launch = [&](const wchar_t *desktop) {
      si.StartupInfo.lpDesktop = const_cast<LPWSTR>(desktop);
      return CreateProcessAsUserW(
        raw_primary_token,
        nullptr,
        (LPWSTR) cmd_line.c_str(),
        nullptr,
        nullptr,
        inherit_handles,
        creation_flags,
        env_block,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        (LPSTARTUPINFOW) &si,
        &pi
      );
    };

    // Prefer the user's default desktop, but fall back to Winlogon if required (e.g. lock/login screen).
    BOOL ok = try_launch(kDefaultDesktopW);
    const wchar_t *launched_desktop = kDefaultDesktopW;
    if (!ok) {
      ok = try_launch(kWinlogonDesktopW);
      launched_desktop = kWinlogonDesktopW;
    }

    si.StartupInfo.lpDesktop = prev_desktop;
    if (!ok) {
      BOOST_LOG(debug) << "CreateProcessAsUserW failed while launching SYSTEM child in active session (session_id="
                       << session_id << "), winerr=" << GetLastError();
    } else {
      BOOST_LOG(info) << "Launched SYSTEM child in active console session (session_id=" << session_id
                      << ", pid=" << pi.dwProcessId << ", desktop=" << (launched_desktop == kDefaultDesktopW ? "default" : "winlogon")
                      << ").";
    }
    return ok;
  }

}  // namespace

ProcessHandler::ProcessHandler():
    job_(create_kill_on_close_job()),
    use_job_(true) {}

ProcessHandler::ProcessHandler(bool use_job):
    job_(use_job ? create_kill_on_close_job() : winrt::handle {}),
    use_job_(use_job) {}

bool ProcessHandler::start(
  const std::wstring &application_path,
  std::wstring_view arguments,
  bool allow_system_fallback,
  helper_pipes_t *pipes
) {
  if (running_) {
    // Check if the previously started process has already exited. If so, clear stale state.
    if (pi_.hProcess != nullptr) {
      DWORD wait_result = WaitForSingleObject(pi_.hProcess, 0);
      if (wait_result == WAIT_TIMEOUT) {
        // Still running, don't start a new one
        return false;
      }

      // Process either exited or handle is invalid, clean up and allow restart
      if (pi_.hThread) {
        CloseHandle(pi_.hThread);
      }
      if (pi_.hProcess) {
        CloseHandle(pi_.hProcess);
      }
      ZeroMemory(&pi_, sizeof(pi_));
      running_ = false;
    } else {
      // No process handle but marked running; reset state to allow restart
      running_ = false;
    }
  }

  ZeroMemory(&pi_, sizeof(pi_));

  // Build command line: "app_path" [arguments]
  std::wstring cmd_line;
  cmd_line.reserve(application_path.size() + arguments.size() + 3);
  cmd_line.push_back(L'"');
  cmd_line += application_path;
  cmd_line.push_back(L'"');
  if (!arguments.empty()) {
    cmd_line.push_back(L' ');
    cmd_line.append(arguments);
  }

  BOOST_LOG(debug) << "Launching process: " << platf::to_utf8(application_path)
                   << (arguments.empty() ? "" : " with arguments") << " (hidden, detached)";

  STARTUPINFOEXW si = {};
  si.StartupInfo.cb = sizeof(si);

  // Optional stdout/stderr capture. The write ends are the only handles the
  // child is allowed to inherit; both are closed in this process right after the
  // launch, so a read on the parent side ends cleanly when the child exits.
  std::optional<capture_pipe_t> out_pipe;
  std::optional<capture_pipe_t> err_pipe;
  std::vector<HANDLE> inherited;
  handle_list_attribute_t handle_list;
  BOOL inherit_handles = FALSE;
  if (pipes) {
    out_pipe = make_capture_pipe();
    err_pipe = make_capture_pipe();
    if (!out_pipe || !err_pipe) {
      return false;
    }
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = out_pipe->write.get();
    si.StartupInfo.hStdError = err_pipe->write.get();
    si.StartupInfo.hStdInput = nullptr;
    inherited = {out_pipe->write.get(), err_pipe->write.get()};
    if (!handle_list.init(si, inherited)) {
      return false;
    }
    inherit_handles = TRUE;
  }

  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
  // When not using a job (keep-alive child), prefer to break away from any existing job to avoid kill-on-close
  if (!use_job_) {
    creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
  }
  // Compute a sane working directory for the child: the directory of the target executable
  std::wstring working_dir;
  {
    size_t pos = application_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
      working_dir = application_path.substr(0, pos);
    }
  }

  BOOL ret = FALSE;

  if (platf::is_running_as_system()) {
    HANDLE user_token = platf::retrieve_users_token(false);
    if (user_token) {
      auto close_token = util::fail_guard([&]() {
        CloseHandle(user_token);
      });

      // Build a user-specific environment block for the child process
      void *env_block = nullptr;
      if (!CreateEnvironmentBlock(&env_block, user_token, FALSE)) {
        BOOST_LOG(error) << "CreateEnvironmentBlock failed, error: " << GetLastError();
        env_block = nullptr;
      }
      auto destroy_env = util::fail_guard([&]() {
        if (env_block) {
          DestroyEnvironmentBlock(env_block);
        }
      });

      // Launch in the user's context with their environment and an explicit working directory
      ret = CreateProcessAsUserW(
        user_token,
        nullptr,
        (LPWSTR) cmd_line.c_str(),
        nullptr,
        nullptr,
        inherit_handles,
        creation_flags,
        env_block,
        working_dir.empty() ? nullptr : working_dir.c_str(),
        (LPSTARTUPINFOW) &si,
        &pi_
      );
    } else if (allow_system_fallback) {
      BOOST_LOG(warning) << "No user session available; launching as SYSTEM: " << platf::to_utf8(application_path);

      // Prefer launching into the active console session so display APIs (e.g., SetDisplayConfig)
      // have a better chance of working while Sunshine runs as a service.
      if (start_as_system_in_active_console_session(cmd_line, working_dir, creation_flags, inherit_handles, si, pi_)) {
        ret = TRUE;
      } else {
        ret = CreateProcessW(
          nullptr,
          (LPWSTR) cmd_line.c_str(),
          nullptr,
          nullptr,
          inherit_handles,
          creation_flags,
          nullptr,
          working_dir.empty() ? nullptr : working_dir.c_str(),
          (LPSTARTUPINFOW) &si,
          &pi_
        );
      }
    } else {
      BOOST_LOG(error) << "Failed to retrieve user token while launching: " << platf::to_utf8(application_path);
      return false;
    }
  } else {
    // Non-SYSTEM: inherit our environment but still supply a sensible working directory
    ret = CreateProcessW(
      nullptr,
      (LPWSTR) cmd_line.c_str(),
      nullptr,
      nullptr,
      inherit_handles,
      creation_flags,
      nullptr,
      working_dir.empty() ? nullptr : working_dir.c_str(),
      (LPSTARTUPINFOW) &si,
      &pi_
    );
  }

  if (ret && use_job_ && job_) {
    AssignProcessToJobObject(job_.get(), pi_.hProcess);
  }

  // Hand the read ends over and drop our copies of the write ends. Holding a
  // write end here would keep the pipe open after the child died, so the reader
  // would block forever instead of seeing end-of-stream.
  if (pipes && ret) {
    pipes->stdout_read = std::move(out_pipe->read);
    pipes->stderr_read = std::move(err_pipe->read);
  }
  out_pipe.reset();
  err_pipe.reset();

  running_ = ret;
  if (!running_) {
    auto winerr = GetLastError();
    BOOST_LOG(error) << "Failed to launch process: " << platf::to_utf8(application_path) << ", error: " << winerr;
    ZeroMemory(&pi_, sizeof(pi_));
  }
  if (running_) {
    DWORD pid = pi_.dwProcessId;
    BOOST_LOG(debug) << "Process started successfully (pid=" << pid << ")";
  }
  return running_;
}

bool ProcessHandler::wait(DWORD &exit_code) {
  return wait_for(exit_code, INFINITE);
}

bool ProcessHandler::wait_for(DWORD &exit_code, DWORD timeout_ms) {
  if (!running_ || pi_.hProcess == nullptr) {
    return false;
  }
  DWORD wait_result = WaitForSingleObject(pi_.hProcess, timeout_ms);
  if (wait_result != WAIT_OBJECT_0) {
    if (wait_result == WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Process wait timed out after " << timeout_ms << "ms";
    } else {
      BOOST_LOG(warning) << "Process wait failed, result=" << wait_result << ", error=" << GetLastError();
    }
    return false;
  }
  BOOL got_code = GetExitCodeProcess(pi_.hProcess, &exit_code);

  // The process has exited; release OS handles and clear state to allow clean restarts.
  running_ = false;
  if (pi_.hThread) {
    CloseHandle(pi_.hThread);
  }
  if (pi_.hProcess) {
    CloseHandle(pi_.hProcess);
  }
  ZeroMemory(&pi_, sizeof(pi_));
  return got_code != 0;
}

void ProcessHandler::terminate() {
  if (running_ && pi_.hProcess) {
    if (!TerminateProcess(pi_.hProcess, 1)) {
      BOOST_LOG(warning) << "TerminateProcess failed, error=" << GetLastError();
    }
    // Do not clear running_/handles here: callers may need to wait() for full teardown
    // to avoid overlapping helper instances and destabilizing the driver stack.
  }
}

ProcessHandler::~ProcessHandler() {
  // For helpers that should outlive the parent (use_job_ == false),
  // do not terminate them on handler destruction. Only terminate
  // processes that we explicitly manage via a kill-on-close Job.
  if (use_job_) {
    terminate();
  }

  // Clean up handles
  if (pi_.hProcess) {
    CloseHandle(pi_.hProcess);
  }
  if (pi_.hThread) {
    CloseHandle(pi_.hThread);
  }
  // job_ is a winrt::handle and will auto-cleanup
}

HANDLE ProcessHandler::get_process_handle() const {
  return running_ ? pi_.hProcess : nullptr;
}

winrt::handle create_kill_on_close_job() {
  winrt::handle job_handle {CreateJobObjectW(nullptr, nullptr)};
  if (!job_handle) {
    return {};
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
  job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job_handle.get(), JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
    // winrt::handle will auto-close on destruction
    return {};
  }
  return job_handle;
}
