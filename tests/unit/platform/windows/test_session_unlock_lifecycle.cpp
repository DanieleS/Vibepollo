#include "src/platform/windows/session_unlock_lifecycle.h"

#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
}  // namespace

int main() {
  using platf::session_unlock::lifecycle_t;
  using namespace std::chrono_literals;
  auto now = lifecycle_t::clock::now();

  lifecycle_t late;
  late.requested(now);
  late.ended();
  require(late.monitoring(), "teardown must not abandon a pending Windows submission");
  late.unlocked();
  require(late.needs_relock(now + 11s), "a late unlock after teardown must relock immediately");
  require(late.owned, "attempting relock must retain ownership until Windows confirms it");
  require(late.needs_relock(now + 20s), "a failed relock must remain retryable");
  late.locked();
  require(!late.monitoring(), "confirmed lock retires ownership");

  lifecycle_t orphan;
  orphan.requested(now);
  orphan.unlocked();
  require(!orphan.needs_relock(now + 89s), "an orphan gets the launch grace period");
  require(orphan.needs_relock(now + 90s), "an orphan unlock must relock at its deadline");

  lifecycle_t active;
  active.started();
  active.requested(now);  // another client's request while a transport already exists
  active.unlocked();
  require(!active.needs_relock(now + 120s), "unlock must not erase an existing active stream");
  active.ended();
  require(active.needs_relock(now + 121s), "the last stream ending must trigger relock");

  lifecycle_t cancelled;
  cancelled.requested(now);
  cancelled.cancelled();
  require(!cancelled.monitoring(), "confirmed cancellation retires a request");
  cancelled.unlocked();
  require(!cancelled.owned, "a later manual unlock must not become host-owned");

  lifecycle_t still_pending;
  still_pending.requested(now);
  still_pending.locked();
  require(still_pending.monitoring(), "a currently locked desktop cannot disprove a pending unlock");

  lifecycle_t manual_lock;
  manual_lock.requested(now);
  manual_lock.unlocked();
  manual_lock.started();
  manual_lock.locked();
  manual_lock.unlocked();
  manual_lock.ended();
  require(!manual_lock.needs_relock(now + 100s), "manual lock ends ownership of the previous unlock");

  std::puts("session unlock lifecycle: all checks passed");
}
