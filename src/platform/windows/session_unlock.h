/**
 * @file src/platform/windows/session_unlock.h
 * @brief Unlocking a locked Windows session when an allowed client starts a stream.
 *
 * Fork addition (Scirocco). When a client holding the `unlock` permission starts or resumes a
 * stream while the signed-in session is locked, the host asks its credential provider
 * (tools/unlock_provider) to unlock it with the stored account password, and locks the session
 * again once the last stream ends. See session_unlock_contract.h for how the two sides talk.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace platf::session_unlock {
  /**
   * @brief Before a stream starts or resumes for a client that holds the unlock permission.
   * @details Does nothing unless the feature is on and the session is locked. Otherwise asks the
   * provider to unlock and waits, a few seconds at most, for the session to turn unlocked. The
   * outcome goes to the log and the unlock history; the stream goes ahead either way.
   */
  void before_stream(const std::string &client_name);

  /// The first stream session has started.
  void stream_started();

  /// The last stream session has ended: lock the session again if this host unlocked it.
  void stream_ended();

  /// What the Web UI shows: whether it's on, for which account, what Windows allows, recent events.
  nlohmann::json status();

  /**
   * @brief Turns the feature on for the account signed in on the console.
   * @details Checks the password with Windows, stores account and password as LSA secrets and
   * registers the credential provider.
   * @return Empty on success, otherwise what went wrong, for the Web UI.
   */
  std::string enable(const std::string &password);

  /// Turns the feature off: unregisters the provider and deletes the stored secrets.
  void disable();

  /// Whether to lock the session again when the stream that unlocked it ends.
  void set_relock(bool relock);

  /// `sunshine.exe --unlock-cleanup`, run by the uninstaller: the same as disable().
  int cleanup_command();
}  // namespace platf::session_unlock
