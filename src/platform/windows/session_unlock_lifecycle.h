/** Platform-independent ownership policy. Callers serialize access. */
#pragma once

#include <chrono>

namespace platf::session_unlock {
  struct lifecycle_t {
    using clock = std::chrono::steady_clock;
    bool pending = false;
    bool owned = false;
    bool stream_active = false;
    bool stream_finished = false;
    clock::time_point orphan_deadline;

    void requested(clock::time_point now) {
      pending = true;
      stream_finished = false;
      orphan_deadline = now + std::chrono::seconds(90);
    }

    void unlocked() {
      if (pending) {
        pending = false;
        owned = true;
      }
    }

    void cancelled() {
      pending = false;
    }

    void locked() {
      // A still-pending serialization may unlock after this observation.
      owned = false;
    }

    void started() {
      stream_active = true;
    }

    void ended() {
      stream_active = false;
      stream_finished = true;
    }

    bool needs_relock(clock::time_point now) const {
      return owned && !stream_active && (stream_finished || now >= orphan_deadline);
    }

    bool monitoring() const {
      return pending || owned;
    }
  };
}  // namespace platf::session_unlock
