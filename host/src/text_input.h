/**
 * @file src/text_input.h
 * @brief Detection of focused text input fields on the host (NyxPlay protocol extension).
 *
 * NyxPlay clients can ask the host to report whenever an editable text field gains or
 * loses keyboard focus. Touch clients use this to show their on-screen keyboard
 * automatically when the user taps a text box on the streamed desktop.
 *
 * Clients opt in with the `nyxTextInput=1` launch/resume parameter. Only sessions that
 * opted in (and that hold the keyboard input permission) receive the state messages.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace text_input {

  /**
   * @brief Packet type of the control stream message carrying the text input state.
   * @note Chosen outside of the Sunshine (0x55xx) and Apollo (0x30xx) extension ranges.
   */
  constexpr std::uint16_t PACKET_TYPE = 0x4E01;

  /**
   * @brief Version of the payload layout sent to clients.
   */
  constexpr std::uint8_t PAYLOAD_VERSION = 1;

  /**
   * @brief Additional hints describing the focused field.
   */
  enum flags_e : std::uint8_t {
    FLAG_NONE = 0x00,
    FLAG_PASSWORD = 0x01,  ///< The field hides its content (password box)
    FLAG_MULTILINE = 0x02,  ///< The field accepts multiple lines (document / rich edit)
  };

  /**
   * @brief Snapshot of the host text input focus.
   */
  struct state_t {
    std::uint32_t generation = 0;  ///< Incremented on every published change, 0 means "unknown"
    bool active = false;  ///< True while an editable text field has keyboard focus
    std::uint8_t flags = FLAG_NONE;  ///< Combination of flags_e
  };

  /**
   * @brief Wire layout of the payload that follows the control header.
   */
#pragma pack(push, 1)

  struct payload_t {
    std::uint8_t version;  ///< PAYLOAD_VERSION
    std::uint8_t active;  ///< 1 when a text field is focused
    std::uint8_t flags;  ///< flags_e
    std::uint8_t reserved;  ///< Always 0
  };

#pragma pack(pop)

  static_assert(sizeof(payload_t) == 4, "text input payload must be 4 bytes");

  /**
   * @brief Minimal description of a focused UI element used for classification.
   *
   * Control type identifiers follow the UI Automation numbering so the logic can be
   * unit tested on every platform.
   */
  struct element_info_t {
    int control_type = 0;  ///< UIA control type id (e.g. 50004 for Edit)
    bool enabled = true;  ///< UIA IsEnabled
    bool keyboard_focusable = true;  ///< UIA IsKeyboardFocusable
    bool is_password = false;  ///< UIA IsPassword
    std::optional<bool> value_read_only;  ///< ValuePattern.IsReadOnly if the pattern exists
    bool has_text_pattern = false;  ///< TextPattern support
  };

  /// UI Automation control type identifiers relevant for text entry.
  namespace control_type {
    constexpr int combo_box = 50003;
    constexpr int edit = 50004;
    constexpr int document = 50030;
    constexpr int custom = 50025;
    constexpr int pane = 50033;
    constexpr int group = 50026;
  }  // namespace control_type

  /**
   * @brief Decide whether a focused element accepts typed text.
   * @param info Element description.
   * @return The resulting state (generation is left at 0).
   */
  state_t classify(const element_info_t &info);

  /**
   * @brief Get the most recently published state.
   * @note Thread safe.
   */
  state_t current();

  /**
   * @brief Publish a new state. Duplicate states are ignored.
   * @param active True if a text field is focused.
   * @param flags Combination of flags_e.
   * @return True if the state changed (and the generation was incremented).
   */
  bool publish(bool active, std::uint8_t flags);

  /**
   * @brief Start (or keep alive) the platform focus monitor.
   *
   * The monitor runs while at least one returned guard is alive. On platforms without
   * an implementation the guard is still returned but no state is ever published.
   *
   * @return Guard object controlling the monitor lifetime.
   */
  std::shared_ptr<void> acquire();

  /**
   * @brief Build the wire payload for a state.
   */
  payload_t to_payload(const state_t &state);

  /**
   * @brief Parse a wire payload (used by tests and tools).
   * @return std::nullopt if the payload is malformed.
   */
  std::optional<state_t> from_payload(std::string_view payload);

#ifdef _WIN32
  /**
   * @brief Classify a window (or control) through UI Automation.
   * @param hwnd Native window handle (HWND).
   * @return The classification, or std::nullopt if UI Automation could not inspect the window.
   * @note Used by diagnostics and tests; the monitor itself reacts to focus change events.
   */
  std::optional<state_t> classify_window(void *hwnd);
#endif
}  // namespace text_input
