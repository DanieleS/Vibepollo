/**
 * @file tests/unit/test_input_activity.cpp
 * @brief Tests for the classification of input that boosts the encode cadence.
 */
#include "../tests_common.h"

#include <cstring>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <src/input_activity_policy.h>

namespace {
  using input::activity::sources_t;
  using input::activity::tracker_t;

  constexpr sources_t all_sources {true, true, true};

  // Builds packets the way clients put them on the wire: the size is big-endian,
  // the magic and controller fields little-endian, mouse deltas and scroll
  // amounts big-endian.
  template<typename Packet>
  Packet make_packet(const std::uint32_t magic) {
    Packet packet {};
    const auto size = static_cast<std::uint32_t>(sizeof(Packet) - sizeof(packet.header.size));
    auto *bytes = reinterpret_cast<std::uint8_t *>(&packet.header);
    bytes[0] = static_cast<std::uint8_t>(size >> 24);
    bytes[1] = static_cast<std::uint8_t>(size >> 16);
    bytes[2] = static_cast<std::uint8_t>(size >> 8);
    bytes[3] = static_cast<std::uint8_t>(size);
    bytes[4] = static_cast<std::uint8_t>(magic);
    bytes[5] = static_cast<std::uint8_t>(magic >> 8);
    bytes[6] = static_cast<std::uint8_t>(magic >> 16);
    bytes[7] = static_cast<std::uint8_t>(magic >> 24);
    return packet;
  }

  template<typename Packet>
  std::vector<std::uint8_t> bytes_of(const Packet &packet) {
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }

  void put_i16_le(void *field, const std::int16_t value) {
    auto *bytes = static_cast<std::uint8_t *>(field);
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
  }

  void put_i16_be(void *field, const std::int16_t value) {
    auto *bytes = static_cast<std::uint8_t *>(field);
    bytes[0] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
  }

  bool evaluate(tracker_t &tracker, const std::vector<std::uint8_t> &bytes, const sources_t &sources = all_sources) {
    return tracker.evaluate(bytes.data(), bytes.size(), sources);
  }

  NV_MULTI_CONTROLLER_PACKET controller(
    const std::int16_t number,
    const std::uint16_t buttons = 0,
    const std::uint8_t left_trigger = 0,
    const std::int16_t left_stick_x = 0
  ) {
    auto packet = make_packet<NV_MULTI_CONTROLLER_PACKET>(MULTI_CONTROLLER_MAGIC_GEN5);
    put_i16_le(&packet.controllerNumber, number);
    put_i16_le(&packet.buttonFlags, static_cast<std::int16_t>(buttons));
    packet.leftTrigger = left_trigger;
    put_i16_le(&packet.leftStickX, left_stick_x);
    return packet;
  }
}  // namespace

TEST(InputActivity, CountsKeyboardMouseButtonsAndScrolling) {
  tracker_t tracker;
  EXPECT_TRUE(evaluate(tracker, bytes_of(make_packet<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(make_packet<NV_KEYBOARD_PACKET>(KEY_UP_EVENT_MAGIC))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(make_packet<NV_UNICODE_PACKET>(UTF8_TEXT_EVENT_MAGIC))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(make_packet<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(make_packet<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5))));

  auto scroll = make_packet<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  EXPECT_FALSE(evaluate(tracker, bytes_of(scroll)));
  put_i16_be(&scroll.scrollAmt1, 120);
  EXPECT_TRUE(evaluate(tracker, bytes_of(scroll)));

  auto hscroll = make_packet<SS_HSCROLL_PACKET>(SS_HSCROLL_MAGIC);
  EXPECT_FALSE(evaluate(tracker, bytes_of(hscroll)));
  put_i16_be(&hscroll.scrollAmount, -120);
  EXPECT_TRUE(evaluate(tracker, bytes_of(hscroll)));
}

TEST(InputActivity, IgnoresMotionAndDeviceReports) {
  tracker_t tracker;
  auto relative = make_packet<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  put_i16_be(&relative.deltaX, 25);
  EXPECT_FALSE(evaluate(tracker, bytes_of(relative)));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<NV_ABS_MOUSE_MOVE_PACKET>(MOUSE_MOVE_ABS_MAGIC))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<SS_CONTROLLER_MOTION_PACKET>(SS_CONTROLLER_MOTION_MAGIC))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<SS_CONTROLLER_BATTERY_PACKET>(SS_CONTROLLER_BATTERY_MAGIC))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<SS_CONTROLLER_ARRIVAL_PACKET>(SS_CONTROLLER_ARRIVAL_MAGIC))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<NV_HAPTICS_PACKET>(ENABLE_HAPTICS_MAGIC))));
}

TEST(InputActivity, CountsContactEdgesButNotContactMotion) {
  tracker_t tracker;
  for (const auto event_type : {LI_TOUCH_EVENT_DOWN, LI_TOUCH_EVENT_UP}) {
    auto touch = make_packet<SS_TOUCH_PACKET>(SS_TOUCH_MAGIC);
    touch.eventType = event_type;
    EXPECT_TRUE(evaluate(tracker, bytes_of(touch)));

    auto pad = make_packet<SS_CONTROLLER_TOUCH_PACKET>(SS_CONTROLLER_TOUCH_MAGIC);
    pad.eventType = event_type;
    EXPECT_TRUE(evaluate(tracker, bytes_of(pad)));
  }

  auto pen_button = make_packet<SS_PEN_PACKET>(SS_PEN_MAGIC);
  pen_button.eventType = LI_TOUCH_EVENT_BUTTON_ONLY;
  EXPECT_TRUE(evaluate(tracker, bytes_of(pen_button)));

  for (const auto event_type : {LI_TOUCH_EVENT_MOVE, LI_TOUCH_EVENT_HOVER, LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOUCH_EVENT_CANCEL_ALL}) {
    auto touch = make_packet<SS_TOUCH_PACKET>(SS_TOUCH_MAGIC);
    touch.eventType = event_type;
    EXPECT_FALSE(evaluate(tracker, bytes_of(touch)));

    auto pen = make_packet<SS_PEN_PACKET>(SS_PEN_MAGIC);
    pen.eventType = event_type;
    EXPECT_FALSE(evaluate(tracker, bytes_of(pen)));
  }
}

TEST(InputActivity, DisabledSourcesNeverCount) {
  tracker_t tracker;
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC)), {false, true, true}));
  EXPECT_FALSE(evaluate(tracker, bytes_of(make_packet<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5)), {true, false, true}));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0x1000)), {true, true, false}));

  auto touch = make_packet<SS_TOUCH_PACKET>(SS_TOUCH_MAGIC);
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  EXPECT_FALSE(evaluate(tracker, bytes_of(touch), {true, false, true}));
}

TEST(InputActivity, CountsControllerButtonChangesOnly) {
  tracker_t tracker;
  // A resting controller announcing itself is not an action.
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0, 0x1000))));
  // Holding the button repeats the same state.
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0x1000))));
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0))));

  // Each controller keeps its own state.
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(1, 0x0001))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0))));
}

TEST(InputActivity, FiltersAnalogJitter) {
  tracker_t tracker;
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 0, 300))));
  // Drift inside the deadzone.
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 3, -900))));
  // Crossing the deadzone counts; moving while held, however far, does not.
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0, 0, 3, 12000))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 3, 12500))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 3, 32000))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 3, 20000))));
  // Trigger pulled past its threshold, then squeezed further.
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0, 0, 200, 20000))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 255, 20000))));
  // Releasing back into the deadzone counts.
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0, 0, 0, 0))));

  // reset() forgets the resting state: a held stick then counts as a fresh action.
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 0, 20000)), {true, true, false}));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(0, 0, 0, 20000))));
  tracker.reset();
  EXPECT_TRUE(evaluate(tracker, bytes_of(controller(0, 0, 0, 20000))));
}

TEST(InputActivity, RejectsOutOfRangeControllersAndTruncatedPackets) {
  tracker_t tracker;
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(16, 0x1000))));
  EXPECT_FALSE(evaluate(tracker, bytes_of(controller(-1, 0x1000))));

  auto key = bytes_of(make_packet<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC));
  key.resize(sizeof(NV_INPUT_HEADER) - 1);
  EXPECT_FALSE(evaluate(tracker, key));

  auto scroll = make_packet<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  put_i16_be(&scroll.scrollAmt1, 120);
  auto scroll_bytes = bytes_of(scroll);
  scroll_bytes.resize(sizeof(NV_SCROLL_PACKET) - 1);
  EXPECT_FALSE(evaluate(tracker, scroll_bytes));

  auto pad = bytes_of(controller(0, 0x1000));
  pad.resize(sizeof(NV_MULTI_CONTROLLER_PACKET) - 1);
  EXPECT_FALSE(evaluate(tracker, pad));

  EXPECT_FALSE(tracker.evaluate(nullptr, 64, all_sources));
}
