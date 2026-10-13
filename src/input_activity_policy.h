/**
 * @file src/input_activity_policy.h
 * @brief Portable classification of client input that should boost the encode cadence.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>

namespace input::activity {
  /**
   * @brief Input sources the host currently forwards to the OS (config::input.*).
   */
  struct sources_t {
    bool keyboard = true;
    bool mouse = true;
    bool controller = true;
  };

  /**
   * @brief Decides whether an input packet is a deliberate user action.
   * @details Only discrete actions count: key presses, typed text, mouse buttons,
   *          non-zero scrolling, touch/pen contacts, controller touchpad contacts,
   *          controller buttons, and triggers/sticks crossing their deadzone.
   *          Pointer and touch motion, analog movement while held, controller motion
   *          sensors, battery and arrival reports never count: they are either
   *          continuous streams or already produce capture updates on their own,
   *          and letting them keep the boost alive is what made the reference
   *          implementation compete with the game for the GPU.
   *
   *          The tracker keeps per-controller state and is not thread-safe; use it
   *          from the single input worker only.
   */
  class tracker_t {
  public:
    /**
     * @brief Evaluate one (possibly batched) input packet.
     * @param packet The raw packet bytes, starting with NV_INPUT_HEADER.
     * @param size The number of bytes available at packet.
     * @param sources The input sources the host forwards; disabled sources never count.
     * @return true when the packet should count as user activity.
     */
    bool evaluate(const std::uint8_t *packet, std::size_t size, const sources_t &sources);

    /**
     * @brief Forget the per-controller state.
     */
    void reset();

  private:
    struct controller_state_t {
      bool initialized = false;
      std::uint32_t button_flags = 0;
      std::uint8_t left_trigger = 0;
      std::uint8_t right_trigger = 0;
      std::int16_t left_stick_x = 0;
      std::int16_t left_stick_y = 0;
      std::int16_t right_stick_x = 0;
      std::int16_t right_stick_y = 0;
    };

    bool evaluate_controller(const std::uint8_t *packet, std::size_t size);

    // activeGamepadMask is 16 bits wide, so controller numbers are bounded by 16.
    std::array<controller_state_t, 16> controllers_ {};
  };
}  // namespace input::activity
