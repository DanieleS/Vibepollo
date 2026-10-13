/**
 * @file src/focus_hints.h
 * @brief Platform-neutral protocol and policy for text field focus hints.
 *
 * A handheld client has no way to tell from the video that the host just focused a
 * text field, so the user has to raise the on-screen keyboard by hand every time. The
 * host can see it, so it says so: it classifies the focused element and sends the
 * result on the control stream.
 *
 * This is a negotiated extension. The client asks for it with
 * `client_feature_flag` in `x-ml-general.featureFlags`; the host only then watches
 * focus and sends `packet_type` messages, and only when `focus_hints` is enabled.
 * Every other client sees exactly the traffic it saw before.
 *
 * Apart from `start_monitor()`/`stop_monitor()`, nothing here touches the operating system,
 * so it is unit tested on every platform. The detector lives in
 * `platform/windows/focus_monitor.cpp`.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace focus_hints {

  /**
   * @brief What kind of input the focused element takes.
   * @details These are the wire values, shared with moonlight-common-c's LI_TEXT_FOCUS_*.
   */
  enum class kind_t : std::uint8_t {
    none = 0,  ///< Nothing focused takes text
    text = 1,  ///< An ordinary text field
    numeric = 2,  ///< A field that only takes digits. Only reported when certain.
    password = 3,  ///< A password field
  };

  /// `x-ml-general.featureFlags` bit: the client wants focus hints.
  /// Far from the low bits, which upstream allocates in order and other forks already use
  /// (0x04 is DualSense haptics PCM here, 0x08 and 0x10 are taken in Moonlight V+).
  inline constexpr std::uint32_t client_feature_flag = 0x10000;

  /// `x-ss-general.featureFlags` bit: the host sends focus hints. Same bit, same reasoning;
  /// the low bits of this field are `platf::platform_caps`.
  inline constexpr std::uint32_t host_feature_flag = 0x10000;

  /// Control stream message type, host to client. Unused by Moonlight, Sunshine, Apollo and
  /// Vibeshine; drunkitguy/apollo2-vibe uses it for this same payload.
  inline constexpr std::uint16_t packet_type = 0x3020;

  /// First byte of the payload. A later version may append fields, never reorder these.
  inline constexpr std::uint8_t wire_version = 1;

  /// Version (u8), kind (u8), reserved (u16), sequence number (u32), little endian.
  inline constexpr std::size_t payload_size = 8;

  /**
   * @brief Serialize one hint.
   * @param sequence Monotonic within a session, starting at 1. The client drops anything
   *                 that is not newer than what it already acted on.
   */
  std::array<std::uint8_t, payload_size> encode_payload(kind_t kind, std::uint32_t sequence);

  /// Human-readable name, for logs.
  const char *to_string(kind_t kind);

  /// Whether this build has a detector. Elsewhere the option is accepted but never advertised.
#ifdef _WIN32
  inline constexpr bool detector_available = true;
#else
  inline constexpr bool detector_available = false;
#endif

  /// Whether the host offers hints: the option is on and there is something to produce them.
  [[nodiscard]] constexpr bool offered(const bool configured) {
    return configured && detector_available;
  }

  /// The `x-ss-general.featureFlags` value to advertise.
  [[nodiscard]] constexpr std::uint32_t host_feature_flags(const std::uint32_t platform_caps, const bool enabled) {
    return enabled ? (platform_caps | host_feature_flag) : platform_caps;
  }

  /// Whether a session gets hints: only when the host has them on and the client asked.
  [[nodiscard]] constexpr bool negotiated(const bool enabled, const std::uint32_t client_feature_flags) {
    return enabled && (client_feature_flags & client_feature_flag) != 0;
  }

  /**
   * @brief Win32 edit control style bits, mirrored so the policy builds everywhere.
   * @details The Windows detector static_asserts these against the SDK.
   */
  namespace edit_style {
    inline constexpr std::uint32_t password = 0x0020;  ///< ES_PASSWORD
    inline constexpr std::uint32_t read_only = 0x0800;  ///< ES_READONLY
    inline constexpr std::uint32_t number = 0x2000;  ///< ES_NUMBER
  }  // namespace edit_style

  /**
   * @brief Classify a classic Win32 edit control from its window style alone.
   * @details This is the only reliable way to know a field takes digits: UI Automation has
   *          no property for it, and a plain edit and an `ES_NUMBER` edit look identical
   *          through it.
   */
  kind_t classify_edit_style(std::uint32_t style);

  /**
   * @brief UI Automation control type ids, mirrored for the same reason as `edit_style`.
   */
  namespace control_type {
    inline constexpr int combo_box = 50003;  ///< UIA_ComboBoxControlTypeId
    inline constexpr int edit = 50004;  ///< UIA_EditControlTypeId
    inline constexpr int spinner = 50016;  ///< UIA_SpinnerControlTypeId
    inline constexpr int document = 50030;  ///< UIA_DocumentControlTypeId
  }  // namespace control_type

  /// What UI Automation said about the focused element.
  struct automation_facts_t {
    int control_type {};
    bool is_password {};
    bool has_value_pattern {};  ///< The element exposes the Value pattern
    bool value_read_only {};  ///< Only meaningful with `has_value_pattern`
  };

  /**
   * @brief Classify the focused element from what UI Automation reports about it.
   * @details Errs towards `none`: a keyboard that pops up uninvited, say every time a web
   *          page is clicked, is worse than one the user has to raise by hand. A document
   *          only counts when it says it is editable, because a browser's page is a
   *          document too.
   */
  kind_t classify_automation(const automation_facts_t &facts);

  /**
   * @brief Turns a stream of focus events into the few hints worth sending.
   * @details Focus storms are normal: tabbing through a dialog, or a window activating
   *          and handing focus to its default control, fires several events within a few
   *          milliseconds. Classification waits for focus to settle, hints are spaced by
   *          a minimum interval, and a classification equal to the last one sent is never
   *          sent again. The control stream also carries input, so it stays quiet.
   *
   *          Single-threaded: owned by the detector thread.
   */
  class debouncer_t {
  public:
    using clock = std::chrono::steady_clock;

    static constexpr clock::duration default_settle = std::chrono::milliseconds {120};
    static constexpr clock::duration default_min_interval = std::chrono::milliseconds {100};

    explicit debouncer_t(clock::duration settle = default_settle, clock::duration min_interval = default_min_interval);

    /// Focus moved: classify once it has been still for the settle time.
    void focus_changed(clock::time_point now);

    /// Whether the focused element should be classified now.
    [[nodiscard]] bool classify_due(clock::time_point now) const;

    /// How long until `classify_due()` can become true, or nothing when no event is pending.
    [[nodiscard]] std::optional<clock::duration> time_until_due(clock::time_point now) const;

    /**
     * @brief Record the result of a classification.
     * @return `true` when it differs from the last hint, which means it has to be sent.
     */
    bool classified(kind_t kind, clock::time_point now);

    [[nodiscard]] kind_t reported() const {
      return reported_;
    }

  private:
    clock::duration settle_;
    clock::duration min_interval_;
    std::optional<clock::time_point> settle_at_;
    std::optional<clock::time_point> last_sent_;
    kind_t reported_ = kind_t::none;
  };

  /// Invoked on the detector thread when the classification changes. Must not block.
  using callback_t = std::function<void(kind_t)>;

  /**
   * @brief Start the platform detector, which reports the current focus and then every change.
   * @details One detector serves the host: while it runs for one session, another session
   *          that negotiated hints gets none. Only Windows has a detector.
   * @return `true` when this caller started it and has to call `stop_monitor()`.
   */
  bool start_monitor(callback_t on_change);

  /**
   * @brief Stop the detector and join its thread.
   * @details Safe to call when nothing is running. Once it returns the callback is never
   *          invoked again.
   */
  void stop_monitor();

}  // namespace focus_hints
