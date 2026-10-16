/**
 * @file src/confighttp_unlock.cpp
 * @brief Web API for "unlock when streaming" (Windows-only). See docs/unlock-on-stream.md.
 *
 * Follows the confighttp_rtss.cpp pattern: it borrows authenticate/print_req/send_response from
 * confighttp.cpp and defers everything else to platf::session_unlock. The password in a POST body
 * is never logged.
 */

#ifdef _WIN32

// standard includes
  #include <string>

// third-party includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/server_https.hpp>

// local includes
  #include "confighttp.h"
  #include "src/platform/windows/session_unlock.h"

namespace confighttp {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Forward declarations for helpers defined in confighttp.cpp
  bool authenticate(resp_https_t response, req_https_t request);
  void print_req(const req_https_t &request);
  void send_response(resp_https_t response, const nlohmann::json &output_tree);

  // GET /api/unlock — the current state for the Web UI.
  void getUnlock(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    send_response(response, platf::session_unlock::status());
  }

  // POST /api/unlock — turn the feature on with the console account's password. Never log the body.
  void postUnlock(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    // Deliberately not print_req here: the body carries the password.

    std::string password;
    try {
      auto body = nlohmann::json::parse(request->content.string());
      password = body.value("password", "");
    } catch (const std::exception &) {
      send_response(response, {{"status", false}, {"error", "Invalid request."}});
      return;
    }

    std::string error = platf::session_unlock::enable(password);
    if (!error.empty()) {
      send_response(response, {{"status", false}, {"error", error}});
      return;
    }
    send_response(response, platf::session_unlock::status());
  }

  // DELETE /api/unlock — turn the feature off and remove the stored credentials.
  void deleteUnlock(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    platf::session_unlock::disable();
    send_response(response, platf::session_unlock::status());
  }

  // POST /api/unlock/settings — { "relock": bool }: lock again when the stream ends, or not.
  void postUnlockSettings(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    try {
      auto body = nlohmann::json::parse(request->content.string());
      if (body.contains("relock")) {
        platf::session_unlock::set_relock(body["relock"].get<bool>());
      }
    } catch (const std::exception &) {
      send_response(response, {{"status", false}, {"error", "Invalid request."}});
      return;
    }
    send_response(response, platf::session_unlock::status());
  }

}  // namespace confighttp

#endif  // _WIN32
