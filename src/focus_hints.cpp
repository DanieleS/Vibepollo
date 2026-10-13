/**
 * @file src/focus_hints.cpp
 * @brief Platform-neutral protocol and policy for text field focus hints.
 */
// this include
#include "focus_hints.h"

// standard includes
#include <algorithm>

namespace focus_hints {

  std::array<std::uint8_t, payload_size> encode_payload(const kind_t kind, const std::uint32_t sequence) {
    return {
      wire_version,
      static_cast<std::uint8_t>(kind),
      0,  // reserved
      0,
      static_cast<std::uint8_t>(sequence),
      static_cast<std::uint8_t>(sequence >> 8),
      static_cast<std::uint8_t>(sequence >> 16),
      static_cast<std::uint8_t>(sequence >> 24),
    };
  }

  const char *to_string(const kind_t kind) {
    switch (kind) {
      case kind_t::none:
        return "none";
      case kind_t::text:
        return "text";
      case kind_t::numeric:
        return "numeric";
      case kind_t::password:
        return "password";
    }
    return "unknown";
  }

  kind_t classify_edit_style(const std::uint32_t style, const bool rich_edit) {
    if (style & edit_style::read_only) {
      // Focusable but not typeable, so a keyboard would be wrong
      return kind_t::none;
    }
    if (style & edit_style::password) {
      return kind_t::password;
    }
    if (!rich_edit && (style & edit_style::number)) {
      return kind_t::numeric;
    }
    return kind_t::text;
  }

  kind_t classify_automation(const automation_facts_t &facts) {
    const bool editable_value = facts.has_value_pattern && !facts.value_read_only;

    switch (facts.control_type) {
      case control_type::edit:
        // Plenty of edits only expose the Text pattern, so a missing Value pattern is not
        // a reason to doubt them. Only an explicit read-only one is.
        if (facts.has_value_pattern && facts.value_read_only) {
          return kind_t::none;
        }
        return facts.is_password ? kind_t::password : kind_t::text;
      case control_type::document:
        // Rich editors are documents, but so is every web page a browser shows. Only the
        // ones that say they can be edited count.
        if (!editable_value) {
          return kind_t::none;
        }
        return facts.is_password ? kind_t::password : kind_t::text;
      case control_type::combo_box:
        // Editable combo boxes take text, fixed lists don't
        return editable_value ? kind_t::text : kind_t::none;
      case control_type::spinner:
        // A spinner's value is a number by definition, but only an editable one is typed into
        return editable_value ? kind_t::numeric : kind_t::none;
      default:
        return kind_t::none;
    }
  }

  debouncer_t::debouncer_t(const clock::duration settle, const clock::duration min_interval):
      settle_ {settle},
      min_interval_ {min_interval} {
  }

  void debouncer_t::focus_changed(const clock::time_point now, const focus_target_t &target) {
    settle_at_ = now + settle_;
    pending_target_ = target;
    retried_ = false;
  }

  bool debouncer_t::classify_due(const clock::time_point now) const {
    const auto wait = time_until_due(now);
    return wait && *wait == clock::duration::zero();
  }

  std::optional<debouncer_t::clock::duration> debouncer_t::time_until_due(const clock::time_point now) const {
    if (!settle_at_) {
      return std::nullopt;
    }

    auto due_at = *settle_at_;
    if (last_sent_) {
      due_at = std::max(due_at, *last_sent_ + min_interval_);
    }
    return std::max(clock::duration::zero(), due_at - now);
  }

  bool debouncer_t::classified(const kind_t kind, const clock::time_point now) {
    settle_at_.reset();
    retried_ = false;

    // Moving straight from one text field to another is news to a client whose user closed
    // the keyboard in the first one, so that is resent. The same element focused again, as
    // in a focus storm, is not.
    const bool moved = pending_target_ != classified_target_;
    classified_target_ = pending_target_;
    if (kind == reported_ && (kind == kind_t::none || !moved)) {
      return false;
    }

    reported_ = kind;
    last_sent_ = now;
    return true;
  }

  void debouncer_t::classification_failed(const clock::time_point now) {
    if (retried_) {
      settle_at_.reset();
      return;
    }
    retried_ = true;
    settle_at_ = now + settle_;
  }

#ifndef _WIN32
  // Classifying the focused element needs an accessibility API; only Windows has a detector.
  subscription_t start_monitor(callback_t) {
    return 0;
  }

  void stop_monitor(subscription_t) {
  }
#endif

}  // namespace focus_hints
