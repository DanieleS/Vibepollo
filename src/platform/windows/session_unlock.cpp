/**
 * @file src/platform/windows/session_unlock.cpp
 * @brief Host side of unlocking a locked Windows session for an allowed stream (Scirocco fork).
 *
 * The host already runs as SYSTEM, so everything CouchPilot split across an elevated setup helper
 * and a DPAPI-sealed, per-user store collapses into this one module:
 *
 * - it finds the console account, checks the password with Windows, and keeps the account, the
 *   password and the SID as LSA private data (readable only by SYSTEM) under the contract's keys;
 * - it registers tools/VibepolloUnlock.dll as a credential provider while the feature is on, and
 *   unregisters it on disable();
 * - at stream start it tells the provider to unlock over the SYSTEM-only pipe (the password never
 *   crosses it: the provider reads it from LSA itself), waits for the session to turn unlocked, and
 *   relocks it when the last stream ends — but only if this host did the unlocking.
 *
 * See session_unlock_contract.h for the names shared with the provider, and docs/unlock-on-stream.md
 * for the spec. Modelled on CouchPilot's src/CouchPilot/Unlock/UnlockService.cs and
 * src/CouchPilotUnlockSetup/Main.cpp.
 */
#ifdef _WIN32

// standard includes
  #include <chrono>
  #include <cstdint>
  #include <filesystem>
  #include <fstream>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

// platform includes
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>

  #include <ntsecapi.h>
  #include <objbase.h>  // StringFromGUID2
  #include <sddl.h>  // ConvertSidToStringSidW
  #include <wtsapi32.h>

// local includes
  #include "session_unlock.h"
  #include "session_unlock_contract.h"
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/utils.h"

namespace platf::session_unlock {

  namespace {
    using json = nlohmann::json;

    // The CLSID the host registers the provider under; the one definition is the contract's macro.
    constexpr GUID kProviderClsid = {VIBEPOLLO_UNLOCK_CLSID_INIT};

    // How long we let the session turn unlocked after the provider accepts the handoff.
    constexpr auto kUnlockPollTimeout = std::chrono::seconds(10);
    constexpr auto kUnlockPollStep = std::chrono::milliseconds(250);

    // An unlock with no stream behind it (a failed launch, a terminate) relocks after this.
    constexpr auto kOrphanTimeout = std::chrono::seconds(90);

    // The credential providers list, where Windows looks up our CLSID on the lock screen.
    const wchar_t *kCredentialProvidersKey =
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\";

    // Hello-only sign-in: while this is 2, Windows hides every password provider, ours included.
    const wchar_t *kPasswordLessKey =
      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\PasswordLess\\Device";

    // ---- small string helpers -------------------------------------------------------------------

    std::string wide_to_utf8(const std::wstring &w) {
      if (w.empty()) {
        return {};
      }
      int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
      std::string s(len, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), len, nullptr, nullptr);
      return s;
    }

    std::wstring utf8_to_wide(const std::string &s) {
      if (s.empty()) {
        return {};
      }
      int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
      std::wstring w(len, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), len);
      return w;
    }

    void secure_wipe(std::wstring &s) {
      if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
      }
      s.clear();
    }

    std::wstring clsid_string() {
      wchar_t buf[64] = {};
      StringFromGUID2(kProviderClsid, buf, 64);
      return buf;
    }

    // tools/VibepolloUnlock.dll next to sunshine.exe: the path Windows loads the provider from.
    std::wstring provider_dll_path() {
      wchar_t path[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, path, MAX_PATH);
      std::wstring exe(path);
      size_t slash = exe.find_last_of(L'\\');
      std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
      return dir + L"\\tools\\" + VIBEPOLLO_UNLOCK_DLL_NAME;
    }

    // ---- LSA private data (the account, password and SID) ---------------------------------------

    void init_lsa_string(LSA_UNICODE_STRING *s, const wchar_t *str) {
      size_t len = str ? wcslen(str) : 0;
      s->Length = (USHORT) (len * sizeof(wchar_t));
      s->MaximumLength = (USHORT) ((len + 1) * sizeof(wchar_t));
      s->Buffer = (PWSTR) str;
    }

    // value == nullptr deletes the secret.
    bool store_lsa_secret(const wchar_t *key, const wchar_t *value) {
      LSA_OBJECT_ATTRIBUTES attrs;
      ZeroMemory(&attrs, sizeof(attrs));
      LSA_HANDLE policy = nullptr;
      if (LsaOpenPolicy(nullptr, &attrs, POLICY_CREATE_SECRET, &policy) != 0) {
        return false;
      }
      LSA_UNICODE_STRING name, data;
      init_lsa_string(&name, key);
      if (value) {
        init_lsa_string(&data, value);
      }
      NTSTATUS status = LsaStorePrivateData(policy, &name, value ? &data : nullptr);
      LsaClose(policy);
      return status == 0;
    }

    std::wstring read_lsa_secret(const wchar_t *key) {
      LSA_OBJECT_ATTRIBUTES attrs;
      ZeroMemory(&attrs, sizeof(attrs));
      LSA_HANDLE policy = nullptr;
      if (LsaOpenPolicy(nullptr, &attrs, POLICY_GET_PRIVATE_INFORMATION, &policy) != 0) {
        return {};
      }
      LSA_UNICODE_STRING name;
      init_lsa_string(&name, key);
      PLSA_UNICODE_STRING data = nullptr;
      NTSTATUS status = LsaRetrievePrivateData(policy, &name, &data);
      LsaClose(policy);
      if (status != 0 || !data) {
        return {};
      }
      std::wstring value(data->Buffer, data->Length / sizeof(wchar_t));
      SecureZeroMemory(data->Buffer, data->MaximumLength);
      LsaFreeMemory(data);
      return value;
    }

    // ---- provider registration ------------------------------------------------------------------

    bool write_reg_sz(HKEY root, const std::wstring &subkey, const wchar_t *name, const std::wstring &value) {
      HKEY key = nullptr;
      if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
      }
      LONG r = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *) value.c_str(),
        (DWORD) ((value.size() + 1) * sizeof(wchar_t)));
      RegCloseKey(key);
      return r == ERROR_SUCCESS;
    }

    bool provider_registered() {
      HKEY key = nullptr;
      std::wstring subkey = kCredentialProvidersKey + clsid_string();
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
      }
      return false;
    }

    void unregister_provider() {
      std::wstring clsid = clsid_string();
      RegDeleteTreeW(HKEY_CLASSES_ROOT, (L"CLSID\\" + clsid).c_str());
      RegDeleteKeyW(HKEY_LOCAL_MACHINE, (kCredentialProvidersKey + clsid).c_str());
    }

    bool register_provider() {
      std::wstring clsid = clsid_string();
      std::wstring clsid_key = L"CLSID\\" + clsid;
      std::wstring inproc = clsid_key + L"\\InprocServer32";
      std::wstring dll = provider_dll_path();

      bool ok =
        write_reg_sz(HKEY_CLASSES_ROOT, clsid_key, nullptr, VIBEPOLLO_UNLOCK_FRIENDLY_NAME) &&
        write_reg_sz(HKEY_CLASSES_ROOT, inproc, nullptr, dll) &&
        write_reg_sz(HKEY_CLASSES_ROOT, inproc, L"ThreadingModel", L"Apartment") &&
        write_reg_sz(HKEY_LOCAL_MACHINE, kCredentialProvidersKey + clsid, nullptr, VIBEPOLLO_UNLOCK_FRIENDLY_NAME);

      if (!ok) {
        // A half-done registration is worse than none: undo everything on failure.
        unregister_provider();
      }
      return ok;
    }

    // ---- console account resolution -------------------------------------------------------------

    struct console_account_t {
      std::wstring logon_name;  // what LogonUI signs in as: "DOMAIN\\user", or "MicrosoftAccount\\email"
      std::wstring display;  // what the UI shows: the email for an MSA, else "DOMAIN\\user"
      std::wstring sid;  // the account's SID, as a string
      bool valid = false;
    };

    // The connected Microsoft-account email, kept in the IdentityStore cache under the user's SID.
    std::wstring microsoft_account_email(const std::wstring &sid) {
      std::wstring path = L"SOFTWARE\\Microsoft\\IdentityStore\\Cache\\" + sid + L"\\IdentityCache\\" + sid;
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return {};
      }
      wchar_t buf[512] = {};
      DWORD cb = sizeof(buf);
      DWORD type = 0;
      std::wstring email;
      if (RegQueryValueExW(key, L"UserName", nullptr, &type, (LPBYTE) buf, &cb) == ERROR_SUCCESS && type == REG_SZ) {
        email = buf;
      }
      RegCloseKey(key);
      if (email.find(L'@') == std::wstring::npos) {
        return {};  // not an email: not a Microsoft account
      }
      return email;
    }

    console_account_t resolve_console_account() {
      console_account_t acct;

      DWORD session_id = WTSGetActiveConsoleSessionId();
      if (session_id == 0xFFFFFFFF) {
        return acct;
      }

      HANDLE token = nullptr;
      if (!WTSQueryUserToken(session_id, &token)) {
        return acct;
      }

      DWORD cb = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &cb);
      std::vector<BYTE> buf(cb);
      if (cb && GetTokenInformation(token, TokenUser, buf.data(), cb, &cb)) {
        auto *tu = (TOKEN_USER *) buf.data();
        LPWSTR sid_str = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &sid_str)) {
          acct.sid = sid_str;
          LocalFree(sid_str);
        }

        wchar_t name[256] = {};
        wchar_t domain[256] = {};
        DWORD name_len = 256, domain_len = 256;
        SID_NAME_USE use;
        if (LookupAccountSidW(nullptr, tu->User.Sid, name, &name_len, domain, &domain_len, &use)) {
          std::wstring sam = std::wstring(domain) + L"\\" + name;
          std::wstring email = acct.sid.empty() ? std::wstring() : microsoft_account_email(acct.sid);
          if (!email.empty()) {
            acct.logon_name = L"MicrosoftAccount\\" + email;
            acct.display = email;
          } else {
            acct.logon_name = sam;
            acct.display = sam;
          }
          acct.valid = !acct.sid.empty();
        }
      }
      CloseHandle(token);
      return acct;
    }

    // True if the account and password really sign in. A network logon is tried first (it touches
    // no session); a Microsoft account may only take an interactive one, so a refusal is checked
    // that way too. The token is closed at once. A less usual failure is allowed through with a log
    // line rather than blocking a valid setup. Mirrors CouchPilot's ValidatePassword.
    bool validate_password(const std::wstring &logon_name, const std::wstring &password, std::string &error) {
      std::wstring domain, user;
      size_t slash = logon_name.find(L'\\');
      if (slash != std::wstring::npos) {
        domain = logon_name.substr(0, slash);
        user = logon_name.substr(slash + 1);
      } else {
        domain = L".";
        user = logon_name;
      }

      DWORD err = 0;
      for (DWORD logon_type : {LOGON32_LOGON_NETWORK, LOGON32_LOGON_INTERACTIVE}) {
        HANDLE token = nullptr;
        if (LogonUserW(user.c_str(), domain.c_str(), password.c_str(), logon_type, LOGON32_PROVIDER_DEFAULT, &token)) {
          if (token) {
            CloseHandle(token);
          }
          return true;
        }
        err = GetLastError();
        if (err != ERROR_LOGON_FAILURE && err != ERROR_ACCOUNT_RESTRICTION) {
          break;
        }
      }
      if (err == ERROR_LOGON_FAILURE || err == ERROR_ACCOUNT_RESTRICTION) {
        error = "Windows didn't accept that password for this account.";
        return false;
      }
      BOOST_LOG(warning) << "session_unlock: couldn't verify the password (error " << err << "); accepting it anyway";
      return true;
    }

    bool hello_only_enabled() {
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPasswordLessKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
      }
      DWORD value = 0, cb = sizeof(value), type = 0;
      bool on = RegQueryValueExW(key, L"DevicePasswordLessBuildVersion", nullptr, &type, (LPBYTE) &value, &cb) == ERROR_SUCCESS &&
                type == REG_DWORD && value == 2;
      RegCloseKey(key);
      return on;
    }

    // ---- the pipe client ------------------------------------------------------------------------

    // Connect to the provider's pipe and ask it to unlock. Returns the provider's response code, or
    // "unavailable" if the pipe isn't there (not registered, Hello-only, or LogonUI hasn't loaded
    // the provider yet).
    std::string send_unlock_request() {
      HANDLE pipe = INVALID_HANDLE_VALUE;
      for (int attempt = 0; attempt < 2; ++attempt) {
        pipe = CreateFileW(VIBEPOLLO_UNLOCK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
          OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
          break;
        }
        if (GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(VIBEPOLLO_UNLOCK_PIPE_NAME, 3000)) {
          continue;
        }
        break;
      }
      if (pipe == INVALID_HANDLE_VALUE) {
        return "unavailable";
      }

      DWORD mode = PIPE_READMODE_MESSAGE;
      SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

      std::string request = std::string("{\"v\":") + std::to_string(VIBEPOLLO_UNLOCK_PROTOCOL_VERSION) + ",\"op\":\"unlock\"}";
      DWORD written = 0;
      std::string reply;
      if (WriteFile(pipe, request.data(), (DWORD) request.size(), &written, nullptr)) {
        char buf[512] = {};
        DWORD read = 0;
        if (ReadFile(pipe, buf, sizeof(buf), &read, nullptr) && read > 0) {
          reply.assign(buf, read);
        }
      }
      CloseHandle(pipe);

      if (reply.find("\"ok\":true") != std::string::npos) {
        return "ok";
      }
      if (reply.find("not-enrolled") != std::string::npos) {
        return "not-enrolled";
      }
      if (reply.empty()) {
        return "unavailable";
      }
      return "failed";
    }

    // LogonUI may only load its providers once the lock-screen curtain is dismissed. Nudge it with
    // harmless input on the input desktop — a 1px mouse move and a Ctrl tap, never Enter — so the
    // provider's pipe comes up. Best-effort: flagged to verify on the PC.
    void wake_lock_screen() {
      HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
      if (!input) {
        return;
      }
      HDESK prev = GetThreadDesktop(GetCurrentThreadId());
      if (SetThreadDesktop(input)) {
        INPUT in[3] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dx = 1;
        in[0].mi.dy = 0;
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE;
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = VK_CONTROL;
        in[2].type = INPUT_KEYBOARD;
        in[2].ki.wVk = VK_CONTROL;
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(3, in, sizeof(INPUT));
        if (prev) {
          SetThreadDesktop(prev);
        }
      }
      CloseDesktop(input);
    }

    // Relock the console session, in the user's own session, the way Win+L would. Mirrors the doc:
    // rundll32.exe user32.dll,LockWorkStation with the user's token on winsta0\default.
    void lock_session() {
      HANDLE user_token = retrieve_users_token(false);
      if (!user_token) {
        BOOST_LOG(warning) << "session_unlock: no user token; can't relock the session";
        return;
      }

      wchar_t system_dir[MAX_PATH] = {};
      GetSystemDirectoryW(system_dir, MAX_PATH);
      std::wstring cmd = std::wstring(system_dir) + L"\\rundll32.exe user32.dll,LockWorkStation";

      std::wstring desktop = L"winsta0\\default";
      STARTUPINFOW si = {};
      si.cb = sizeof(si);
      si.lpDesktop = desktop.data();
      PROCESS_INFORMATION pi = {};
      std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
      mutable_cmd.push_back(L'\0');

      void *env = nullptr;
      if (CreateProcessAsUserW(user_token, nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      } else {
        BOOST_LOG(warning) << "session_unlock: LockWorkStation launch failed (" << GetLastError() << ')';
      }
      CloseHandle(user_token);
    }

    // ---- module state ---------------------------------------------------------------------------

    std::mutex g_mutex;  // serialises before_stream and guards the lifecycle flags below
    bool g_unlocked_by_us = false;  // this host unlocked the current session and hasn't relocked it
    bool g_stream_active = false;  // a stream session is live, so the orphan timer must not relock
    std::uint64_t g_unlock_generation = 0;  // bumped on each unlock, so a stale orphan timer bows out

    std::mutex g_state_mutex;  // guards the on-disk state file

    std::filesystem::path state_path() {
      return platf::appdata() / "unlock_state.json";
    }

    json load_state() {
      json state = {{"relock", true}, {"account", ""}, {"events", json::array()}};
      std::ifstream in(state_path());
      if (in) {
        try {
          json parsed;
          in >> parsed;
          if (parsed.is_object()) {
            if (parsed.contains("relock")) {
              state["relock"] = parsed["relock"];
            }
            if (parsed.contains("account")) {
              state["account"] = parsed["account"];
            }
            if (parsed.contains("events") && parsed["events"].is_array()) {
              state["events"] = parsed["events"];
            }
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "session_unlock: couldn't read the state file: " << e.what();
        }
      }
      return state;
    }

    void save_state(const json &state) {
      std::ofstream out(state_path(), std::ios::trunc);
      if (out) {
        out << state.dump(2);
      }
    }

    // Append one event to the history, keeping only the last 10. Never records a secret.
    void record_event(const std::string &client, const std::string &outcome, const std::string &detail) {
      std::lock_guard lock(g_state_mutex);
      json state = load_state();
      json ev = {
        {"at", (std::int64_t) std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()},
        {"client", client},
        {"outcome", outcome},
        {"detail", detail},
      };
      state["events"].push_back(ev);
      while (state["events"].size() > 10) {
        state["events"].erase(state["events"].begin());
      }
      save_state(state);
    }

    bool relock_enabled() {
      std::lock_guard lock(g_state_mutex);
      return load_state().value("relock", true);
    }

    bool feature_enabled() {
      // Set up and ready: the provider is registered and the secrets are on file.
      return provider_registered() && !read_lsa_secret(VIBEPOLLO_UNLOCK_SID_KEY).empty();
    }

    // Wait, up to kUnlockPollTimeout, for the session to turn unlocked. True if it did.
    bool wait_for_unlock() {
      auto deadline = std::chrono::steady_clock::now() + kUnlockPollTimeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (!is_user_session_locked()) {
          return true;
        }
        std::this_thread::sleep_for(kUnlockPollStep);
      }
      return !is_user_session_locked();
    }

    // Arm the orphan timer: if no stream has started within kOrphanTimeout, relock what we unlocked.
    void arm_orphan_timer(std::uint64_t generation) {
      std::thread([generation]() {
        std::this_thread::sleep_for(kOrphanTimeout);
        std::lock_guard lock(g_mutex);
        if (g_unlock_generation != generation || g_stream_active || !g_unlocked_by_us) {
          return;  // a newer unlock, or a stream is live, or we already relocked
        }
        g_unlocked_by_us = false;
        if (relock_enabled() && !is_user_session_locked()) {
          BOOST_LOG(info) << "session_unlock: no stream within "
                          << std::chrono::duration_cast<std::chrono::seconds>(kOrphanTimeout).count()
                          << "s of an unlock; relocking";
          lock_session();
          record_event("", "relocked", "orphan");
        }
      }).detach();
    }
  }  // namespace

  void before_stream(const std::string &client_name) {
    std::lock_guard lock(g_mutex);

    if (!feature_enabled()) {
      return;
    }
    // Nothing to do if the session is already unlocked (and nothing is relocked later).
    if (!is_user_session_locked()) {
      return;
    }
    if (hello_only_enabled()) {
      BOOST_LOG(info) << "session_unlock: Hello-only sign-in is on; the stream starts on the lock screen";
      record_event(client_name, "hello_only", "");
      return;
    }

    BOOST_LOG(info) << "session_unlock: unlocking the session for '" << client_name << '\'';
    std::string result = send_unlock_request();
    if (result == "unavailable") {
      // LogonUI may not have loaded the providers yet: nudge the curtain and retry briefly.
      wake_lock_screen();
      std::this_thread::sleep_for(std::chrono::milliseconds(750));
      result = send_unlock_request();
    }

    if (result == "ok") {
      if (wait_for_unlock()) {
        g_unlocked_by_us = true;
        g_stream_active = false;
        arm_orphan_timer(++g_unlock_generation);
        BOOST_LOG(info) << "session_unlock: session unlocked for '" << client_name << '\'';
        record_event(client_name, "unlocked", "");
      } else {
        BOOST_LOG(warning) << "session_unlock: the provider accepted the unlock but the session stayed locked (wrong password?)";
        record_event(client_name, "rejected", "");
      }
    } else if (result == "not-enrolled") {
      record_event(client_name, "unavailable", "not-enrolled");
    } else if (result == "unavailable") {
      record_event(client_name, "unavailable", "");
    } else {
      record_event(client_name, "failed", "");
    }
  }

  void stream_started() {
    std::lock_guard lock(g_mutex);
    g_stream_active = true;  // a real stream is up: the orphan timer must not relock
  }

  void stream_ended() {
    std::lock_guard lock(g_mutex);
    g_stream_active = false;
    if (!g_unlocked_by_us) {
      return;  // we didn't unlock it, so we don't relock it
    }
    ++g_unlock_generation;  // retire any pending orphan timer
    g_unlocked_by_us = false;

    if (!relock_enabled()) {
      return;
    }
    if (is_user_session_locked()) {
      return;  // the user relocked it themselves (Win+L); leave it
    }
    BOOST_LOG(info) << "session_unlock: last stream ended; relocking the session";
    lock_session();
    record_event("", "relocked", "");
  }

  nlohmann::json status() {
    json out;
    bool enabled = feature_enabled();
    out["enabled"] = enabled;

    std::wstring enrolled_account = read_lsa_secret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY);
    {
      std::lock_guard lock(g_state_mutex);
      json state = load_state();
      out["account"] = state.value("account", "");
      out["relock"] = state.value("relock", true);
      out["events"] = state["events"];
    }

    auto console = resolve_console_account();
    out["console_account"] = wide_to_utf8(console.display);
    out["provider_installed"] = provider_registered();
    out["hello_only"] = hello_only_enabled();
    out["locked"] = is_user_session_locked();

    secure_wipe(enrolled_account);
    return out;
  }

  std::string enable(const std::string &password) {
    auto console = resolve_console_account();
    if (!console.valid) {
      return "Couldn't read the Windows account signed in on this PC.";
    }

    std::wstring wpassword = utf8_to_wide(password);
    std::string error;
    if (!validate_password(console.logon_name, wpassword, error)) {
      secure_wipe(wpassword);
      return error;
    }

    std::lock_guard lock(g_mutex);

    bool stored =
      store_lsa_secret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY, console.logon_name.c_str()) &&
      store_lsa_secret(VIBEPOLLO_UNLOCK_PASSWORD_KEY, wpassword.c_str()) &&
      store_lsa_secret(VIBEPOLLO_UNLOCK_SID_KEY, console.sid.c_str());
    secure_wipe(wpassword);
    if (!stored) {
      store_lsa_secret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY, nullptr);
      store_lsa_secret(VIBEPOLLO_UNLOCK_PASSWORD_KEY, nullptr);
      store_lsa_secret(VIBEPOLLO_UNLOCK_SID_KEY, nullptr);
      return "Couldn't store the unlock credentials.";
    }

    if (!register_provider()) {
      store_lsa_secret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY, nullptr);
      store_lsa_secret(VIBEPOLLO_UNLOCK_PASSWORD_KEY, nullptr);
      store_lsa_secret(VIBEPOLLO_UNLOCK_SID_KEY, nullptr);
      return "Couldn't register the unlock credential provider.";
    }

    {
      std::lock_guard state_lock(g_state_mutex);
      json state = load_state();
      state["account"] = wide_to_utf8(console.display);
      save_state(state);
    }
    BOOST_LOG(info) << "session_unlock: feature enabled for '" << wide_to_utf8(console.display) << '\'';
    return {};
  }

  void disable() {
    std::lock_guard lock(g_mutex);
    unregister_provider();
    store_lsa_secret(VIBEPOLLO_UNLOCK_ACCOUNT_KEY, nullptr);
    store_lsa_secret(VIBEPOLLO_UNLOCK_PASSWORD_KEY, nullptr);
    store_lsa_secret(VIBEPOLLO_UNLOCK_SID_KEY, nullptr);

    std::lock_guard state_lock(g_state_mutex);
    json state = load_state();
    state["account"] = "";
    save_state(state);
    BOOST_LOG(info) << "session_unlock: feature disabled";
  }

  void set_relock(bool relock) {
    std::lock_guard lock(g_state_mutex);
    json state = load_state();
    state["relock"] = relock;
    save_state(state);
  }

  int cleanup_command() {
    disable();
    return 0;
  }
}  // namespace platf::session_unlock

#endif  // _WIN32
