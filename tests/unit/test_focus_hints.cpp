/**
 * @file tests/unit/test_focus_hints.cpp
 * @brief Test src/focus_hints.*.
 */
#include "../tests_common.h"
#include "src/focus_hints.h"

#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;
using focus_hints::kind_t;

namespace {
  using clock = focus_hints::debouncer_t::clock;

  focus_hints::automation_facts_t facts(int control_type, bool is_password = false, bool has_value = false, bool read_only = false) {
    return {control_type, is_password, has_value, read_only};
  }
}  // namespace

// These are shared with moonlight-common-c and a mismatch is silent on the wire, so they are
// pinned here rather than trusted.
TEST(FocusHintsWire, PayloadIsLittleEndianAndEightBytes) {
  const auto payload = focus_hints::encode_payload(kind_t::numeric, 0x01020304u);
  const std::array<std::uint8_t, 8> expected {1, 2, 0, 0, 0x04, 0x03, 0x02, 0x01};
  EXPECT_EQ(payload, expected);
}

TEST(FocusHintsWire, KindsAreTheWireValues) {
  EXPECT_EQ(static_cast<std::uint8_t>(kind_t::none), 0);
  EXPECT_EQ(static_cast<std::uint8_t>(kind_t::text), 1);
  EXPECT_EQ(static_cast<std::uint8_t>(kind_t::numeric), 2);
  EXPECT_EQ(static_cast<std::uint8_t>(kind_t::password), 3);
}

TEST(FocusHintsWire, PacketTypeAvoidsEveryKnownControlMessage) {
  EXPECT_EQ(focus_hints::packet_type, 0x3020);

  // Moonlight/Sunshine, Apollo, Vibeshine haptics and the IDs other forks are known to use
  for (const std::uint16_t taken : {0x0100, 0x0109, 0x010b, 0x010e, 0x0200, 0x0201, 0x0204, 0x0206, 0x0301, 0x0302, 0x0305, 0x0307, 0x3000, 0x3001, 0x3002, 0x3003, 0x5500, 0x5501, 0x5502, 0x5503, 0x5504, 0x550c, 0x5601}) {
    EXPECT_NE(focus_hints::packet_type, taken) << std::hex << taken;
  }
}

TEST(FocusHintsWire, FeatureBitsAvoidEveryKnownFlag) {
  EXPECT_EQ(focus_hints::client_feature_flag, 0x10000u);
  EXPECT_EQ(focus_hints::host_feature_flag, 0x10000u);

  // Client: FEC status, session ID v1, haptics PCM, and the bits Moonlight V+ uses
  EXPECT_EQ(focus_hints::client_feature_flag & 0xFFu, 0u);
  // Host: pen/touch and controller touch, and the bits Moonlight V+ hosts use
  EXPECT_EQ(focus_hints::host_feature_flag & 0x3FFu, 0u);
}

TEST(FocusHintsNegotiation, BothSidesHaveToAgree) {
  EXPECT_FALSE(focus_hints::negotiated(false, 0));
  EXPECT_FALSE(focus_hints::negotiated(false, focus_hints::client_feature_flag));
  EXPECT_FALSE(focus_hints::negotiated(true, 0x07));  // A stock Moonlight client
  EXPECT_TRUE(focus_hints::negotiated(true, 0x03 | focus_hints::client_feature_flag));
}

TEST(FocusHintsNegotiation, HostAdvertisesOnlyWhenEnabled) {
  EXPECT_EQ(focus_hints::host_feature_flags(0x03, false), 0x03u);
  EXPECT_EQ(focus_hints::host_feature_flags(0x03, true), 0x03u | focus_hints::host_feature_flag);
}

TEST(FocusHintsClassification, EditStyles) {
  using namespace focus_hints::edit_style;
  EXPECT_EQ(focus_hints::classify_edit_style(0), kind_t::text);
  EXPECT_EQ(focus_hints::classify_edit_style(number), kind_t::numeric);
  EXPECT_EQ(focus_hints::classify_edit_style(password), kind_t::password);
  EXPECT_EQ(focus_hints::classify_edit_style(read_only), kind_t::none);
  EXPECT_EQ(focus_hints::classify_edit_style(read_only | number), kind_t::none);
  EXPECT_EQ(focus_hints::classify_edit_style(read_only | password), kind_t::none);
}

TEST(FocusHintsClassification, AutomationEdits) {
  using namespace focus_hints::control_type;
  EXPECT_EQ(focus_hints::classify_automation(facts(edit)), kind_t::text);
  EXPECT_EQ(focus_hints::classify_automation(facts(edit, false, true, false)), kind_t::text);
  EXPECT_EQ(focus_hints::classify_automation(facts(edit, true)), kind_t::password);
  EXPECT_EQ(focus_hints::classify_automation(facts(edit, false, true, true)), kind_t::none);
  EXPECT_EQ(focus_hints::classify_automation(facts(edit, true, true, true)), kind_t::none);
}

TEST(FocusHintsClassification, AutomationDocumentsOnlyWhenEditable) {
  using namespace focus_hints::control_type;
  // A web page: a document, but not one to type into
  EXPECT_EQ(focus_hints::classify_automation(facts(document)), kind_t::none);
  EXPECT_EQ(focus_hints::classify_automation(facts(document, false, true, true)), kind_t::none);
  EXPECT_EQ(focus_hints::classify_automation(facts(document, false, true, false)), kind_t::text);
}

TEST(FocusHintsClassification, AutomationOtherControls) {
  using namespace focus_hints::control_type;
  EXPECT_EQ(focus_hints::classify_automation(facts(combo_box)), kind_t::none);
  EXPECT_EQ(focus_hints::classify_automation(facts(combo_box, false, true, false)), kind_t::text);
  EXPECT_EQ(focus_hints::classify_automation(facts(spinner, false, true, false)), kind_t::numeric);
  EXPECT_EQ(focus_hints::classify_automation(facts(spinner)), kind_t::none);
  EXPECT_EQ(focus_hints::classify_automation(facts(50000)), kind_t::none);  // Button
  EXPECT_EQ(focus_hints::classify_automation(facts(50000, true)), kind_t::none);
}

TEST(FocusHintsDebouncer, NothingDueWithoutAnEvent) {
  focus_hints::debouncer_t debouncer;
  const auto t0 = clock::time_point {} + 1h;
  EXPECT_FALSE(debouncer.classify_due(t0));
  EXPECT_FALSE(debouncer.time_until_due(t0).has_value());
}

TEST(FocusHintsDebouncer, WaitsForFocusToSettle) {
  focus_hints::debouncer_t debouncer {120ms, 100ms};
  const auto t0 = clock::time_point {} + 1h;

  debouncer.focus_changed(t0);
  EXPECT_FALSE(debouncer.classify_due(t0 + 119ms));
  EXPECT_EQ(*debouncer.time_until_due(t0 + 20ms), 100ms);

  // A storm keeps pushing the deadline out, so only the settled state is classified
  debouncer.focus_changed(t0 + 100ms);
  EXPECT_FALSE(debouncer.classify_due(t0 + 200ms));
  EXPECT_TRUE(debouncer.classify_due(t0 + 220ms));
}

TEST(FocusHintsDebouncer, SendsOnlyChanges) {
  focus_hints::debouncer_t debouncer {120ms, 100ms};
  const auto t0 = clock::time_point {} + 1h;

  debouncer.focus_changed(t0);
  EXPECT_FALSE(debouncer.classified(kind_t::none, t0 + 120ms));  // Same as the initial state
  EXPECT_FALSE(debouncer.classify_due(t0 + 1s));

  debouncer.focus_changed(t0 + 1s);
  EXPECT_TRUE(debouncer.classified(kind_t::text, t0 + 1s + 120ms));
  EXPECT_EQ(debouncer.reported(), kind_t::text);

  // Tabbing to another text field is not news
  debouncer.focus_changed(t0 + 2s);
  EXPECT_FALSE(debouncer.classified(kind_t::text, t0 + 2s + 120ms));

  debouncer.focus_changed(t0 + 3s);
  EXPECT_TRUE(debouncer.classified(kind_t::none, t0 + 3s + 120ms));
}

TEST(FocusHintsDebouncer, SpacesHintsByTheMinimumInterval) {
  focus_hints::debouncer_t debouncer {10ms, 100ms};
  const auto t0 = clock::time_point {} + 1h;

  debouncer.focus_changed(t0);
  ASSERT_TRUE(debouncer.classified(kind_t::text, t0 + 10ms));

  debouncer.focus_changed(t0 + 20ms);
  EXPECT_FALSE(debouncer.classify_due(t0 + 30ms));  // Settled, but too soon after the last hint
  EXPECT_EQ(*debouncer.time_until_due(t0 + 30ms), 80ms);
  EXPECT_TRUE(debouncer.classify_due(t0 + 110ms));
}

TEST(FocusHintsNames, EveryKindHasOne) {
  EXPECT_STREQ(focus_hints::to_string(kind_t::none), "none");
  EXPECT_STREQ(focus_hints::to_string(kind_t::text), "text");
  EXPECT_STREQ(focus_hints::to_string(kind_t::numeric), "numeric");
  EXPECT_STREQ(focus_hints::to_string(kind_t::password), "password");
}

TEST(FocusHintsNegotiation, OnlyOfferedWhereThereIsADetector) {
  EXPECT_FALSE(focus_hints::offered(false));
  EXPECT_EQ(focus_hints::offered(true), IS_WINDOWS);
}
