# Unlock on stream (fork: Scirocco)

Tracks [DanieleS/scirocco#21](https://github.com/DanieleS/scirocco/issues/21). Status: the
credential provider, the shared contract, the host module, the stream hooks, the `unlock`
permission, the web API, the web UI, the installer cleanup and the CI import check are all on
branch `feat/unlock-on-stream`. What remains is testing it end to end on the PC (see
[Testing on the PC](#testing-on-the-pc)): none of the Windows-specific code has been run yet.

## What it does

The PC is woken with Wake-on-LAN, or Windows locks itself, and the session sits on the lock
screen. Today a stream then shows the lock screen and a game starts behind it.

With this feature:

- **Stream start.** A client starts or resumes a stream. If the session is locked and the client
  holds the new `unlock` permission, the host unlocks the session before the app launches. It
  waits a few seconds at most for the unlock to land, and the stream goes ahead either way.
- **Stream end.** When the last stream ends, the host locks the session again, but only if it did
  the unlocking. This is a setting and it's on by default.
- **No client change.** Zefiro, any Moonlight client and the nightly live test get it as they are.

Edge cases:

| Case | Behaviour |
|---|---|
| Session already unlocked at stream start | Nothing happens; nothing is relocked later |
| Two clients streaming | Relock when the last session ends |
| Disconnect, then resume | Relock on disconnect, unlock again on resume |
| User presses Win+L mid-stream | Left locked; no unlock mid-stream |
| Unlocked, but no stream ever starts (launch failed, terminate request) | Relocked after about 90 s |
| Password rejected, or Hello-only sign-in on | The stream starts on the lock screen, as today; the event is logged and shown in the Web UI |

Limits, inherited from how Windows works:

- It only unlocks a session that was signed in and then locked. It does not sign in after a
  reboot.
- It needs the account password, not the Windows Hello PIN.
- "For improved security, only allow Windows Hello sign-in" must be off.

## Architecture

```
client ──launch/resume──▶ nvhttp ──before_stream()──▶ session_unlock (host, SYSTEM)
                                                          │  pipe, SYSTEM only: {"v":2,"op":"unlock","id":"GUID"}
                                                          ▼
                                    VibepolloUnlock.dll in LogonUI (SYSTEM) ──▶ Windows unlocks
stream.cpp, last RTSP session ended ──stream_ended()──▶ session_unlock ──▶ lock the session again
```

### Contract (done)

`src/platform/windows/session_unlock_contract.h` holds every name the two sides share:

- the CLSID `{28F4FEFF-97F3-4EC9-8D56-192763B039C4}`;
- the pipe name;
- the three LSA private-data keys (account, password, SID);
- the wire protocol, with its response codes.

Change a name there and both sides follow.

### Credential provider (done, untested on Windows)

`tools/unlock_provider/` is a port of CouchPilot's `CouchPilotUnlock`, simplified because both
ends run as SYSTEM:

- It only takes part in `CPUS_UNLOCK_WORKSTATION`.
- It stays inert (no pipe, no tile) when no SID is stored.
- Its pipe has a SYSTEM-only DACL plus a check that the caller's token is SYSTEM, and no HMAC
  handshake. The host connects with identification-only SQOS and verifies the server's SYSTEM
  token, console session and full System32/LogonUI.exe image path before sending any request.
  Connect waits and overlapped read/write operations have deadlines; timed-out I/O is cancelled
  and drained before its buffers are freed.
- On a request it reads the account and password from LSA itself, arms the hidden credential and
  calls `CredentialsChanged`. The password never crosses the pipe.

It is built by `tools/unlock_provider/unlock_provider.cmake`, included from
`cmake/targets/windows.cmake`, and installed to `tools/VibepolloUnlock.dll`. It is statically
linked: a local mingw build imports only ADVAPI32, KERNEL32, ole32, Secur32 and the system UCRT.

### Host module (done, untested on Windows)

The interface is in `src/platform/windows/session_unlock.h`:
`before_stream`, `stream_started`, `stream_ended`, `status`, `enable`, `disable`, `set_relock`
and `cleanup_command`. The new file is `src/platform/windows/session_unlock.cpp`; add it to the
Windows source list in `cmake/compile_definitions/windows.cmake` and link `netapi32` if you use the
NetUser APIs.

**The credential side.** Finding the account, checking the password, storing the LSA secrets,
registering the provider in HKLM and talking to the pipe: follow CouchPilot.

- `docs/unlock.md` is the spec.
- `src/CouchPilot/Unlock/UnlockService.cs` is the account and password handling.
- `src/CouchPilotUnlockSetup/Main.cpp` is the registration and LSA storage, which the host can do
  directly because it is already SYSTEM.

The secrets go under the keys in the contract. Register the provider only while the feature is on,
and unregister it on `disable()`.

**Stream lifecycle:**

- `before_stream` holds one mutex, so two simultaneous launches make one request.
- It does nothing unless the feature is on and the console session is locked. Lock state comes
  from `WTSQuerySessionInformationW(WTSSessionInfoEx)` for the original target session; query
  failure is represented as unknown.
- Each unlock carries a fresh GUID. After the pipe answers `ok` (or a reply is lost after a
  write), poll the target session's lock state for about 10 s. Unlocked means success:
  remember "unlocked by us". A timeout is **not** a rejected password.
- On timeout, ask the provider to `cancel` that GUID. Cancellation is acknowledged only if that
  exact request has not been serialized. Cancellation and serialization hold the same credential
  lock. A submitted credential stays pending until Windows reports failure or WTS shows an unlock;
  even a successful Windows report cannot acknowledge cancellation before the WTS transition.
- Continue monitoring ambiguous requests after stream teardown and pipe disappearance. Unknown
  request IDs (including in a replacement provider) cannot confirm cancellation. A late unlock
  after teardown is relocked immediately; a failed Windows result records `rejected`.
  If Windows never resolves a submission, the host conservatively keeps monitoring it and does
  not send another credential. It may treat a later manual unlock of that session as the pending
  unlock, rather than silently abandon the relock obligation.
- **Assumption to verify on the PC:** LogonUI may load the providers only once the lock-screen
  curtain is dismissed. If the pipe isn't there, wake the curtain with harmless input on the input
  desktop (a 1 px mouse move and a Ctrl tap; never Enter), then retry for a few seconds.
- **Relock:** run `rundll32.exe user32.dll,LockWorkStation` in the user's session, with the user's
  token for the **original target session** (`WTSQueryUserToken`) on `winsta0\default`.
  Record `relocked` and retire ownership only after WTS confirms the locked state. Retry up to
  three times, three seconds apart; on failure retain ownership and record a `failed` event.
  A later stream-end event can retry. A failed WTS query is unknown, never evidence of success.
- **Orphan unlock:** give an unlock attempt about 90 s for a stream to start, then relock when
  an unlock is observed. Preserve an already-active stream when another request arrives.
- The monitor stops and joins before host state is destroyed during shutdown. Its ownership is
  in memory; host termination/restart does not provide a persistent relock guarantee.
- Protocol v2 requires the host and DLL to be updated together. An already-loaded v1 DLL must be
  unloaded (for example by signing out/rebooting) before v2 can unlock; mismatches fail closed.
- **State file:** `platf::appdata()/unlock_state.json` holds only non-secret data: `relock`
  (default true), the enrolled account's display name and the last 10 events.
  Each event is `{at, client, outcome, detail}`, where `outcome` is one of `unlocked`, `relocked`,
  `rejected`, `hello_only`, `unavailable` or `failed`.

`status()` returns this JSON for the Web UI:

```json
{
  "enabled": true,
  "account": "name@example.com",
  "console_account": "name@example.com",
  "provider_installed": true,
  "hello_only": false,
  "relock": true,
  "locked": false,
  "events": [{"at": 1792154743, "client": "Thor", "outcome": "unlocked", "detail": ""}]
}
```

`hello_only` is true when the registry value
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\PasswordLess\Device` → `DevicePasswordLessBuildVersion`
is 2.

## Permission

A new bit, `unlock = _operation << 6` (`0x00400000`). It is a fork addition, like
`telemetry_read`, so follow commit `4b6baf3ee` and the comment in it about `PERM::_all` clamping.

| File | Change |
|---|---|
| `src/crypto.h` | `unlock` in the enum and in `_all_opeiations`; **not** in `_all_unrequested`, so it's never granted without someone choosing it |
| `src_assets/common/assets/web/views/DevicesView.vue` | `PERMISSION_ALL` 0x071f1f00 → 0x075f1f00 |
| `src_assets/common/assets/web/components/devices/ClientSettingsEditor.vue` | `{ key: 'unlock', mask: 0x00400000 }` in `permissionOptions` |
| `src_assets/common/assets/web/pin.html`, `web-legacy/pin.html` | the enum and the option list, next to `telemetry_read` |
| `src_assets/common/assets/web-legacy/views/ClientManagementView.vue` | key, mask and option, next to `telemetry_read` |
| `public/assets/locale/en.json` (both UIs) | `"permissions.unlock": "Unlock this PC"` (English only) |

## Stream hooks

| Where | Call |
|---|---|
| `src/nvhttp.cpp`, `launch()`, right after the `verified_client` check (around line 4426) | `before_stream(verified_client->name)` when the client has `PERM::unlock` and `PERM::_allow_view`. This is ahead of the Remote Monitor branch, so the live test is covered too |
| `src/nvhttp.cpp`, `resume()`, after the `_allow_view` check (around line 5401) | same |
| `src/stream.cpp`, where `first_rtsp_session` is true (around line 3954) | `stream_started()` |
| `src/stream.cpp`, where `last_rtsp_session` is true (around line 3788) | `stream_ended()` |

Every call goes under `#ifdef _WIN32`, and each is one line next to upstream code, to keep merges
easy.

## Web API

Use a new file, `src/confighttp_unlock.cpp`, following the pattern of `confighttp_rtss.cpp` (it
declares `print_req` and `send_response`). Register the routes in `confighttp.cpp` next to the
other `#ifdef _WIN32` routes; `register_api_route` already checks CSRF on POST and DELETE.

| Route | Body | Does |
|---|---|---|
| `GET /api/unlock` | — | `status()` |
| `POST /api/unlock` | `{"password": "…"}` | `enable()`; on failure `{"status": false, "error": "…"}`. Never log the body |
| `DELETE /api/unlock` | — | `disable()` |
| `POST /api/unlock/settings` | `{"relock": bool}` | `set_relock()` |

The "allowed devices" summary comes from `/api/clients/list` (`perm & 0x00400000`), so the
backend doesn't need it.

## Web UI

Mock: https://claude.ai/artifact/XkmKNG3gNDroFrTpBRBjSd. The approved choice is **A** plus B's
summary rows.

- Add `components/settings/UnlockSettings.vue`, shown in Settings → **Network & security** (the
  `network` category, after "Stream and browser security"), as a custom component like
  `DisplayRecoverySettings.vue`.
- The section is titled **Unlock when streaming** and contains:
  - "Unlock the PC when a stream starts": a switch with a status badge (Ready, Password rejected,
    Blocked by Windows). Turning it off means `DELETE`. Turning it on needs the password first.
  - "Windows account", read-only, from `console_account`.
  - "Password", with **Check and save** (`POST`). Show the error under the field.
  - "Lock again when the stream ends": a switch that calls the settings route.
  - A warning (`InlineAlert`, warning tone) when `hello_only` is true.
  - Summary rows from B: "Allowed: Thor, zefiro-test", linking to Devices, and "Last unlock" with
    the newest event and its outcome.
- In the client permissions, the new checkbox **Unlock this PC**.
- Strings are English only, and the product name stays "Vibepollo" in the sources (`brand.mjs`
  rebrands at build time).

## Installer

- The DLL is already an install target (`tools/`), so the MSI picks it up.
- Uninstall: add `{"unlock-cleanup"sv, …}` to `cmd_to_func` in `src/main.cpp`, calling
  `session_unlock::cleanup_command()`.
- In `custom_actions.wxs`, add `SetUnlockCleanup` and `UnlockCleanup`, modelled on
  `SetRestoreNvPrefsUndo` and `RestoreNvPrefsUndo` (deferred, `Impersonate="no"`, `Return="ignore"`,
  guarded by `if exist sunshine.exe`).
- In `patch_custom_actions.wxs`, sequence them right after `RestoreNvPrefsUndo`, with
  `REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE`. An upgrade keeps the feature on: the provider path
  doesn't change.
- Leaving the provider registered while its DLL is missing is harmless: LogonUI skips a provider
  it can't load. The cleanup is about not leaving the secrets behind.

## CI

Add a step after the Windows build that fails if `tools/VibepolloUnlock.dll` imports anything
outside `KERNEL32`, `ADVAPI32`, `ole32`, `Secur32` and `api-ms-win-crt-*` (`objdump -p`). A
dependency on the toolchain's runtime DLLs would make LogonUI fail to load it.

## Testing on the PC

Before turning it on, keep a way back. From the Pi, removing the provider's registration (pass
`/reg:64` to `reg delete`) restores the stock lock screen:

```
ssh -i ~/.ssh/id_ed25519_pc "davide specchia"@192.168.86.186 reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{28F4FEFF-97F3-4EC9-8D56-192763B039C4}" /f
```

Steps:

1. Install the PR's MSI. Turn the feature on from the Web UI and grant "Unlock this PC" to one
   client.
2. Win+L, then start a stream from that client: the session should unlock and the game appear.
   End the stream: the session locks again.
3. Repeat with the screen off (the curtain case), with a wrong password (the badge says "Password
   rejected" and the stream shows the lock screen), and with Hello-only sign-in on.
4. Turn it off: the provider key and the secrets are gone. Check with `reg query` on the key above.
5. Uninstall: the same check.

The live test then needs no change: its client gets the permission and the Remote Monitor stream
shows real content.

### Security regression checks

- `test_fast_session_unlock_lifecycle` runs on any platform: late unlock after teardown, orphan
  deadline, existing streams, confirmed cancellation, retained ownership and manual locking.
- `test_component_session_unlock_windows` exercises real Windows APIs without authenticating an
  account: credential cancellation vs serialization (including concurrent calls), request-ID
  matching, successful/failed Windows results, rejection of a fake pipe server, identification-only
  tokens, bounded stalled reads/writes and safe reuse after I/O cancellation.
- On the PC, also delay LogonUI beyond 10 seconds, end the stream before the unlock arrives, and
  force a relock launch failure. Confirm a late unlock is tracked and `relocked` appears only
  after Windows actually locks. Test enable/disable and normal stream teardown as above.
