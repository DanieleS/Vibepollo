/**
 * @file src/platform/windows/scry_integration.cpp
 * @brief Supervises the `scry` telemetry helper and accumulates what it reports.
 */
#include "scry_integration.h"

#ifdef _WIN32

  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/windows/foreground_app.h"
  #include "src/platform/windows/ipc/process_handler.h"
  #include "src/platform/windows/misc.h"
  #include "src/process.h"
  #include "src/sync.h"

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <filesystem>
  #include <functional>
  #include <mutex>
  #include <string>
  #include <string_view>
  #include <thread>

  #include <windows.h>

namespace platf::scry {

  using namespace std::chrono_literals;

  namespace {

    /// How often the supervisor re-asks "what should I be attached to?". The
    /// answer changes on human timescales (a game launches, a launcher hands off
    /// to the real executable), so a second is generous and costs nothing.
    constexpr auto SUPERVISE_INTERVAL = 1s;

    /// After the helper exits on its own, wait this long before trying again.
    /// The common reason is the honest one — no profile fits this game — and
    /// respawning that in a tight loop would burn CPU scanning memory to reach
    /// the same conclusion forever.
    constexpr auto RESPAWN_BACKOFF = 30s;

    /// A helper that survived at least this long was working, not failing, so
    /// its exit does not earn the backoff above.
    constexpr auto MIN_HEALTHY_RUN = 5s;

    /// How long a reader parks when its pipe has nothing to say. Small enough
    /// that the exit checks around it stay responsive, large enough that an idle
    /// game costs nothing.
    constexpr auto POLL_INTERVAL = 25ms;

    /// A line longer than this is not something the helper is documented to
    /// produce; treat the stream as garbage rather than growing a buffer without
    /// bound on a misbehaving child.
    constexpr std::size_t MAX_LINE_BYTES = 1u << 20;

    /// The process the helper should be reading.
    struct target_t {
      DWORD pid {0};
      std::string exe;
      std::string slug;

      bool operator==(const target_t &other) const {
        return pid == other.pid && exe == other.exe && slug == other.slug;
      }
    };

    sync_util::sync_t<snapshot_t> g_snapshot;

    /// Monotonic across the whole process lifetime, never per-attach. A consumer
    /// remembers the last revision it acted on, and two games in quick
    /// succession must not be able to produce the same number twice — which is
    /// exactly what a counter restarting at 1 on every attach would do.
    std::atomic<std::uint64_t> g_revision {0};

    /// Stamp the snapshot as changed. Callers must already hold its lock.
    void bump(snapshot_t &snapshot) {
      snapshot.revision = g_revision.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    std::thread g_thread;
    std::mutex g_cv_mutex;
    std::condition_variable g_cv;
    std::atomic<bool> g_stop {false};

    /// Sleep that wakes early when the supervisor is asked to stop.
    void interruptible_wait(std::chrono::milliseconds duration) {
      std::unique_lock<std::mutex> lk(g_cv_mutex);
      g_cv.wait_for(lk, duration, [] {
        return g_stop.load(std::memory_order_acquire);
      });
    }

    /// `tools/scry.exe`, next to sunshine.exe — the same layout every other
    /// bundled helper uses.
    std::optional<std::filesystem::path> helper_path() {
      wchar_t module_path[MAX_PATH] = {};
      if (!GetModuleFileNameW(nullptr, module_path, _countof(module_path))) {
        return std::nullopt;
      }
      std::filesystem::path helper = std::filesystem::path(module_path).parent_path() / L"tools" / L"scry.exe";
      std::error_code ec;
      if (!std::filesystem::exists(helper, ec)) {
        return std::nullopt;
      }
      return helper;
    }

    /// Where per-game profiles live. Configurable because Phase 1 has no
    /// registry: a user (or a support tech) drops profiles in a folder by hand.
    std::optional<std::filesystem::path> profiles_dir() {
      const auto &configured = config::scry.profiles_dir;
      if (configured.empty()) {
        return std::nullopt;
      }
      std::filesystem::path dir = platf::from_utf8(configured);
      std::error_code ec;
      if (!std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
      }
      return dir;
    }

    /// Best-effort executable name out of a configured command line, which may be
    /// quoted and may carry arguments. Only ever used for logging and for the
    /// snapshot's `process` field before the helper reports the real one — the
    /// helper is attached by pid, so getting this wrong costs a cosmetic label
    /// and nothing else.
    std::string executable_from_command(const std::string &command) {
      std::string_view view = command;
      while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
        view.remove_prefix(1);
      }
      if (!view.empty() && view.front() == '"') {
        view.remove_prefix(1);
        if (const auto end = view.find('"'); end != std::string_view::npos) {
          view = view.substr(0, end);
        }
      } else if (const auto space = view.find(' '); space != std::string_view::npos) {
        view = view.substr(0, space);
      }
      return std::filesystem::path(std::string {view}).filename().string();
    }

    /**
     * @brief Decide which process the helper should read.
     *
     * The process Vibepollo launched is frequently *not* the game: Steam, Epic
     * and Playnite all spawn the real executable and may outlive or precede it.
     * Rather than re-solve that here, this reuses the identity Vibepollo already
     * maintains for display and HDR policy — the foreground app, when it has been
     * confirmed to belong to the running app — and falls back to the launched
     * process only when there is no better answer.
     */
    std::optional<target_t> resolve_target() {
      const auto app = proc::proc.running_app_state();
      if (!app.has_active_app) {
        return std::nullopt;
      }

      target_t target;
      // The app name is the stable handle Vibepollo has for "which game is this".
      // Phase 2 replaces it with a registry slug; nothing else here changes.
      target.slug = app.name;

      const auto foreground = platf::foreground_app::snapshot();
      if (foreground.matches_active_app && foreground.foreground_pid != 0 && !foreground.foreground_exe.empty()) {
        target.pid = foreground.foreground_pid;
        target.exe = std::filesystem::path(foreground.foreground_exe).filename().string();
        return target;
      }

      if (app.root_pid != 0) {
        target.pid = static_cast<DWORD>(app.root_pid);
        target.exe = executable_from_command(app.command);
        return target;
      }

      return std::nullopt;
    }

    enum class pump_e {
      data,  ///< Bytes were appended to the buffer.
      idle,  ///< The pipe is open but has nothing to say right now.
      closed,  ///< End of stream: the child exited or the pipe broke.
    };

    /**
     * @brief Move whatever is available from `pipe` into `buffer`, without blocking.
     *
     * A blocking `ReadFile` would be a trap here: scry stays **silent** on a tick
     * where nothing changed, which for a paused game is every tick. A reader
     * parked in `ReadFile` would then never notice that the game exited, that the
     * target changed, or that we are shutting down — and shutdown would hang
     * waiting to join it. So the pipe is peeked first and read only when it has
     * something, leaving the caller free to check its own conditions in between.
     */
    pump_e pump(HANDLE pipe, std::string &buffer) {
      DWORD available = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        return pump_e::closed;
      }
      if (available == 0) {
        return pump_e::idle;
      }
      char chunk[4096];
      DWORD read = 0;
      const DWORD want = std::min<DWORD>(available, sizeof(chunk));
      if (!ReadFile(pipe, chunk, want, &read, nullptr) || read == 0) {
        return pump_e::closed;
      }
      buffer.append(chunk, read);
      return pump_e::data;
    }

    /// Split complete lines out of `buffer`, handing each to `on_line`.
    void drain_lines(std::string &buffer, const std::function<void(std::string_view)> &on_line) {
      std::size_t start = 0;
      for (std::size_t nl = buffer.find('\n', start); nl != std::string::npos; nl = buffer.find('\n', start)) {
        std::string_view line(buffer.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }
        if (!line.empty()) {
          on_line(line);
        }
        start = nl + 1;
      }
      buffer.erase(0, start);
      if (buffer.size() > MAX_LINE_BYTES) {
        BOOST_LOG(warning) << "scry: helper produced an implausibly long line; dropping the buffer";
        buffer.clear();
      }
    }

    /**
     * @brief Apply one JSON event from the helper to the shared snapshot.
     *
     * Unknown event types are ignored on purpose — that is the helper's
     * documented compatibility rule, and it is what lets a newer scry add events
     * without this code having to know about them.
     */
    void apply_event(const nlohmann::json &event, const target_t &target) {
      const auto type = event.value("event", std::string {});

      if (type == "attached") {
        auto lg = g_snapshot.lock();
        auto &snap = g_snapshot.raw;
        snap.attached = true;
        snap.slug = target.slug;
        snap.process = event.value("process", target.exe);
        snap.profile = event.value("profile", std::string {});
        snap.profile_file = event.value("profile_file", std::string {});
        snap.contract_version.reset();
        if (auto it = event.find("contract_version"); it != event.end() && it->is_number_unsigned()) {
          snap.contract_version = it->get<std::uint32_t>();
        }
        snap.values = nlohmann::json::object();
        bump(snap);
        BOOST_LOG(info) << "scry: attached to " << snap.process << " with profile '" << snap.profile
                        << "' (contract " << (snap.contract_version ? std::to_string(*snap.contract_version) : "none") << ")";
        return;
      }

      if (type == "values") {
        const auto values = event.find("values");
        if (values == event.end() || !values->is_object()) {
          return;
        }
        auto lg = g_snapshot.lock();
        auto &snap = g_snapshot.raw;
        if (!snap.attached) {
          // Values before an attach would be values we cannot label. Drop them.
          return;
        }
        // The helper sends only what changed; the full picture is ours to keep.
        for (const auto &[name, value] : values->items()) {
          snap.values[name] = value;
        }
        bump(snap);
        return;
      }

      if (type == "detached") {
        auto lg = g_snapshot.lock();
        g_snapshot.raw = snapshot_t {};
        return;
      }
    }

    /// Clear the shared picture. Called whenever the helper is no longer a
    /// trustworthy source — it exited, the target changed, the feature was
    /// switched off — so that a consumer never renders a dead game's numbers.
    void clear_snapshot() {
      auto lg = g_snapshot.lock();
      g_snapshot.raw = snapshot_t {};
    }

    /**
     * @brief Run the helper against `target` until it exits or we are stopped.
     *
     * Returns when there is nothing left to read: the child died, the target
     * changed under us, or shutdown was requested.
     */
    void run_helper(const std::filesystem::path &helper, const std::filesystem::path &profiles, const target_t &target) {
      std::wstring args = L"watch --pid " + std::to_wstring(target.pid) +
                          L" --profiles \"" + profiles.wstring() + L"\"" +
                          L" --format json --tick " + std::to_wstring(std::clamp(config::scry.tick_ms, 10, 1000));

      helper_pipes_t pipes;
      ProcessHandler child(true);
      if (!child.start(helper.wstring(), args, false, &pipes)) {
        BOOST_LOG(warning) << "scry: could not start the telemetry helper";
        return;
      }

      BOOST_LOG(info) << "scry: watching " << target.exe << " (pid " << target.pid << ") for app '" << target.slug << "'";

      // Diagnostics are a separate stream by design: merging them into stdout
      // would corrupt the JSON the parser below depends on. They are the only
      // place the fail-safe explains itself ("no profile fits"), so they are
      // logged rather than discarded.
      std::atomic<bool> readers_stop {false};
      std::thread stderr_reader([raw = pipes.stderr_read.get(), &readers_stop]() {
        std::string buffer;
        while (!readers_stop.load(std::memory_order_acquire)) {
          const auto result = pump(raw, buffer);
          if (result == pump_e::closed) {
            break;
          }
          if (result == pump_e::idle) {
            std::this_thread::sleep_for(POLL_INTERVAL);
            continue;
          }
          drain_lines(buffer, [](std::string_view line) {
            BOOST_LOG(debug) << "scry: " << line;
          });
        }
      });

      // Re-checking the target costs a foreground-window query and a lock on
      // `proc`; the read loop spins every POLL_INTERVAL, which is far too often
      // to pay that. The answer only changes on human timescales anyway.
      auto next_target_check = std::chrono::steady_clock::now() + SUPERVISE_INTERVAL;

      std::string buffer;
      while (!g_stop.load(std::memory_order_acquire)) {
        const auto result = pump(pipes.stdout_read.get(), buffer);
        if (result == pump_e::closed) {
          break;
        }
        if (result == pump_e::data) {
          drain_lines(buffer, [&](std::string_view line) {
            auto event = nlohmann::json::parse(line, nullptr, false);
            if (event.is_discarded() || !event.is_object()) {
              BOOST_LOG(warning) << "scry: helper wrote a line that is not a JSON object; ignoring it";
              return;
            }
            apply_event(event, target);
          });
        } else {
          std::this_thread::sleep_for(POLL_INTERVAL);
        }

        // A target that changed out from under us (the game exited, the user
        // switched to a different one) makes this helper stale immediately.
        if (const auto now = std::chrono::steady_clock::now(); now >= next_target_check) {
          next_target_check = now + SUPERVISE_INTERVAL;
          const auto current = resolve_target();
          if (!current || !(*current == target)) {
            break;
          }
        }
      }

      readers_stop.store(true, std::memory_order_release);
      child.terminate();
      if (stderr_reader.joinable()) {
        stderr_reader.join();
      }
      clear_snapshot();
    }

    void supervisor_loop() {
      std::optional<std::chrono::steady_clock::time_point> retry_after;

      while (!g_stop.load(std::memory_order_acquire)) {
        if (!config::scry.enabled) {
          clear_snapshot();
          interruptible_wait(SUPERVISE_INTERVAL);
          continue;
        }

        const auto helper = helper_path();
        const auto profiles = profiles_dir();
        const auto target = resolve_target();

        if (!helper || !profiles || !target) {
          clear_snapshot();
          interruptible_wait(SUPERVISE_INTERVAL);
          continue;
        }

        if (retry_after && std::chrono::steady_clock::now() < *retry_after) {
          interruptible_wait(SUPERVISE_INTERVAL);
          continue;
        }

        const auto started_at = std::chrono::steady_clock::now();
        run_helper(*helper, *profiles, *target);
        const auto ran_for = std::chrono::steady_clock::now() - started_at;

        if (ran_for < MIN_HEALTHY_RUN) {
          // It gave up almost immediately. The likeliest reason is the honest
          // one — no profile fits this game — and that will still be true a
          // millisecond from now, so back off rather than spin.
          retry_after = std::chrono::steady_clock::now() + RESPAWN_BACKOFF;
        } else {
          // A helper that ran and then stopped was doing its job until the game
          // or the target changed. Penalising that would delay telemetry for the
          // *next* game by half a minute for no reason.
          retry_after.reset();
        }
      }

      clear_snapshot();
    }

    struct deinit_t: public ::platf::deinit_t {
      ~deinit_t() override {
        g_stop.store(true, std::memory_order_release);
        g_cv.notify_all();
        if (g_thread.joinable()) {
          g_thread.join();
        }
        clear_snapshot();
      }
    };

  }  // namespace

  std::unique_ptr<::platf::deinit_t> start() {
    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(supervisor_loop);
    return std::make_unique<deinit_t>();
  }

  snapshot_t latest() {
    auto lg = g_snapshot.lock();
    return g_snapshot.raw;
  }

  bool helper_available() {
    return helper_path().has_value();
  }

}  // namespace platf::scry

#endif  // _WIN32
