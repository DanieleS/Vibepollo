/**
 * @file src/input_activity_policy.cpp
 * @brief Definitions for the portable input activity classification.
 */
#include "input_activity_policy.h"

// standard includes
#include <cstdlib>
#include <cstring>

extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

namespace input::activity {
  namespace {
    // Analog sticks and triggers only count when they cross their deadzone, in
    // either direction. Moving while held is a continuous stream, like pointer
    // motion, and must not keep re-arming the boost.
    constexpr int STICK_DEADZONE = 2048;
    constexpr int TRIGGER_THRESHOLD = 8;

    // Packets are already validated by the time they are classified, but the
    // classifier is defensive on its own: every field read is bounds-checked and
    // byte-wise, so it is independent of host endianness and alignment.
    template<typename Packet>
    bool fits(const std::size_t size) {
      return size >= sizeof(Packet);
    }

    std::uint32_t read_u32_le(const std::uint8_t *data) {
      return static_cast<std::uint32_t>(data[0]) |
             (static_cast<std::uint32_t>(data[1]) << 8) |
             (static_cast<std::uint32_t>(data[2]) << 16) |
             (static_cast<std::uint32_t>(data[3]) << 24);
    }

    std::int16_t read_i16_le(const std::uint8_t *data) {
      return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0] | (data[1] << 8)));
    }

    std::int16_t read_i16_be(const std::uint8_t *data) {
      return static_cast<std::int16_t>(static_cast<std::uint16_t>((data[0] << 8) | data[1]));
    }

    bool stick_outside_deadzone(const int value) {
      return std::abs(value) >= STICK_DEADZONE;
    }

    bool trigger_crossed_threshold(const int previous, const int current) {
      return (previous >= TRIGGER_THRESHOLD) != (current >= TRIGGER_THRESHOLD);
    }

    bool stick_crossed_deadzone(const int previous, const int current) {
      return stick_outside_deadzone(previous) != stick_outside_deadzone(current);
    }

    // A contact starting or ending is a deliberate action; hover and move are motion.
    bool is_contact_edge(const std::uint8_t event_type) {
      return event_type == LI_TOUCH_EVENT_DOWN ||
             event_type == LI_TOUCH_EVENT_UP ||
             event_type == LI_TOUCH_EVENT_BUTTON_ONLY;
    }
  }  // namespace

  bool tracker_t::evaluate_controller(const std::uint8_t *packet, const std::size_t size) {
    if (!fits<NV_MULTI_CONTROLLER_PACKET>(size)) {
      return false;
    }

    const auto field = [packet](const std::size_t offset) {
      return packet + offset;
    };
    const int controller_number = read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, controllerNumber)));
    if (controller_number < 0 || controller_number >= static_cast<int>(controllers_.size())) {
      return false;
    }

    const auto button_flags =
      static_cast<std::uint32_t>(static_cast<std::uint16_t>(read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, buttonFlags))))) |
      (static_cast<std::uint32_t>(static_cast<std::uint16_t>(read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, buttonFlags2))))) << 16);
    const std::uint8_t left_trigger = *field(offsetof(NV_MULTI_CONTROLLER_PACKET, leftTrigger));
    const std::uint8_t right_trigger = *field(offsetof(NV_MULTI_CONTROLLER_PACKET, rightTrigger));
    const auto left_stick_x = read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, leftStickX)));
    const auto left_stick_y = read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, leftStickY)));
    const auto right_stick_x = read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, rightStickX)));
    const auto right_stick_y = read_i16_le(field(offsetof(NV_MULTI_CONTROLLER_PACKET, rightStickY)));

    auto &state = controllers_[controller_number];
    bool active;
    if (!state.initialized) {
      active = button_flags != 0 ||
               left_trigger >= TRIGGER_THRESHOLD ||
               right_trigger >= TRIGGER_THRESHOLD ||
               stick_outside_deadzone(left_stick_x) ||
               stick_outside_deadzone(left_stick_y) ||
               stick_outside_deadzone(right_stick_x) ||
               stick_outside_deadzone(right_stick_y);
    } else {
      active = button_flags != state.button_flags ||
               trigger_crossed_threshold(state.left_trigger, left_trigger) ||
               trigger_crossed_threshold(state.right_trigger, right_trigger) ||
               stick_crossed_deadzone(state.left_stick_x, left_stick_x) ||
               stick_crossed_deadzone(state.left_stick_y, left_stick_y) ||
               stick_crossed_deadzone(state.right_stick_x, right_stick_x) ||
               stick_crossed_deadzone(state.right_stick_y, right_stick_y);
    }

    state = {
      true,
      button_flags,
      left_trigger,
      right_trigger,
      left_stick_x,
      left_stick_y,
      right_stick_x,
      right_stick_y,
    };
    return active;
  }

  bool tracker_t::evaluate(const std::uint8_t *packet, const std::size_t size, const sources_t &sources) {
    if (packet == nullptr || !fits<NV_INPUT_HEADER>(size)) {
      return false;
    }

    switch (read_u32_le(packet + offsetof(NV_INPUT_HEADER, magic))) {
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        return sources.keyboard && fits<NV_KEYBOARD_PACKET>(size);
      case UTF8_TEXT_EVENT_MAGIC:
        return sources.keyboard;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return sources.mouse && fits<NV_MOUSE_BUTTON_PACKET>(size);
      case SCROLL_MAGIC_GEN5:
        return sources.mouse && fits<NV_SCROLL_PACKET>(size) &&
               (read_i16_be(packet + offsetof(NV_SCROLL_PACKET, scrollAmt1)) != 0 ||
                read_i16_be(packet + offsetof(NV_SCROLL_PACKET, scrollAmt2)) != 0);
      case SS_HSCROLL_MAGIC:
        return sources.mouse && fits<SS_HSCROLL_PACKET>(size) &&
               read_i16_be(packet + offsetof(SS_HSCROLL_PACKET, scrollAmount)) != 0;
      case SS_TOUCH_MAGIC:
        return sources.mouse && fits<SS_TOUCH_PACKET>(size) &&
               is_contact_edge(packet[offsetof(SS_TOUCH_PACKET, eventType)]);
      case SS_PEN_MAGIC:
        return sources.mouse && fits<SS_PEN_PACKET>(size) &&
               is_contact_edge(packet[offsetof(SS_PEN_PACKET, eventType)]);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return sources.controller && fits<SS_CONTROLLER_TOUCH_PACKET>(size) &&
               is_contact_edge(packet[offsetof(SS_CONTROLLER_TOUCH_PACKET, eventType)]);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        // Track state even while controllers are disabled so a later enable
        // does not count the current stick position as a fresh action.
        return evaluate_controller(packet, size) && sources.controller;
      default:
        // Pointer motion, controller motion/battery/arrival and unknown packets.
        return false;
    }
  }

  void tracker_t::reset() {
    controllers_ = {};
  }
}  // namespace input::activity
