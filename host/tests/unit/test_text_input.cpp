/**
 * @file tests/unit/test_text_input.cpp
 * @brief Test src/text_input.* (NyxPlay text input focus extension).
 */
#include <src/text_input.h>

#include "../tests_common.h"

using namespace text_input;

namespace {
  element_info_t element(int type) {
    element_info_t info;
    info.control_type = type;
    return info;
  }
}  // namespace

TEST(TextInputClassifyTests, EditIsActiveByDefault) {
  auto state = classify(element(control_type::edit));
  EXPECT_TRUE(state.active);
  EXPECT_EQ(state.flags, FLAG_NONE);
}

TEST(TextInputClassifyTests, ReadOnlyEditIsInactive) {
  auto info = element(control_type::edit);
  info.value_read_only = true;
  EXPECT_FALSE(classify(info).active);
}

TEST(TextInputClassifyTests, DisabledEditIsInactive) {
  auto info = element(control_type::edit);
  info.enabled = false;
  EXPECT_FALSE(classify(info).active);
}

TEST(TextInputClassifyTests, PasswordEditCarriesPasswordFlag) {
  auto info = element(control_type::edit);
  info.is_password = true;
  auto state = classify(info);
  EXPECT_TRUE(state.active);
  EXPECT_EQ(state.flags & FLAG_PASSWORD, FLAG_PASSWORD);
}

TEST(TextInputClassifyTests, BrowserPageDocumentIsInactive) {
  // Web browsers expose the page as a read-only document
  auto info = element(control_type::document);
  info.value_read_only = true;
  info.has_text_pattern = true;
  EXPECT_FALSE(classify(info).active);
}

TEST(TextInputClassifyTests, EditableDocumentIsMultiline) {
  auto info = element(control_type::document);
  info.value_read_only = false;
  auto state = classify(info);
  EXPECT_TRUE(state.active);
  EXPECT_EQ(state.flags & FLAG_MULTILINE, FLAG_MULTILINE);
}

TEST(TextInputClassifyTests, RichEditDocumentWithoutValuePattern) {
  auto info = element(control_type::document);
  info.has_text_pattern = true;
  info.keyboard_focusable = true;
  EXPECT_TRUE(classify(info).active);
}

TEST(TextInputClassifyTests, ComboBoxRequiresEditableValue) {
  auto info = element(control_type::combo_box);
  EXPECT_FALSE(classify(info).active);
  info.value_read_only = false;
  EXPECT_TRUE(classify(info).active);
}

TEST(TextInputClassifyTests, ButtonsAndWindowsAreInactive) {
  EXPECT_FALSE(classify(element(50000)).active);  // Button
  EXPECT_FALSE(classify(element(50032)).active);  // Window
  EXPECT_FALSE(classify(element(control_type::pane)).active);
}

TEST(TextInputClassifyTests, InactiveStatesClearFlags) {
  auto info = element(50000);
  auto state = classify(info);
  EXPECT_FALSE(state.active);
  EXPECT_EQ(state.flags, FLAG_NONE);
}

TEST(TextInputPayloadTests, RoundTrip) {
  state_t state;
  state.active = true;
  state.flags = FLAG_PASSWORD | FLAG_MULTILINE;

  auto payload = to_payload(state);
  EXPECT_EQ(payload.version, PAYLOAD_VERSION);
  EXPECT_EQ(payload.active, 1);
  EXPECT_EQ(payload.reserved, 0);

  auto parsed = from_payload(std::string_view {(const char *) &payload, sizeof(payload)});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->active);
  EXPECT_EQ(parsed->flags, state.flags);
}

TEST(TextInputPayloadTests, InactivePayloadHasNoFlags) {
  state_t state;
  state.active = false;
  state.flags = FLAG_PASSWORD;
  auto payload = to_payload(state);
  EXPECT_EQ(payload.active, 0);
  EXPECT_EQ(payload.flags, FLAG_NONE);
}

TEST(TextInputPayloadTests, RejectsMalformedPayloads) {
  EXPECT_FALSE(from_payload(std::string_view {}).has_value());
  const char one_byte[] = {1};
  EXPECT_FALSE(from_payload(std::string_view {one_byte, 1}).has_value());
  const char bad_version[] = {0, 1, 0, 0};
  EXPECT_FALSE(from_payload(std::string_view {bad_version, 4}).has_value());
}

TEST(TextInputPublishTests, DeduplicatesAndIncrementsGeneration) {
  publish(false, FLAG_NONE);
  auto base = current();

  EXPECT_TRUE(publish(true, FLAG_NONE));
  auto first = current();
  EXPECT_TRUE(first.active);
  EXPECT_NE(first.generation, base.generation);
  EXPECT_NE(first.generation, 0u);

  // Same state again must not bump the generation
  EXPECT_FALSE(publish(true, FLAG_NONE));
  EXPECT_EQ(current().generation, first.generation);

  // Flags are part of the state
  EXPECT_TRUE(publish(true, FLAG_PASSWORD));
  EXPECT_EQ(current().flags, FLAG_PASSWORD);

  // Inactive states drop their flags
  EXPECT_TRUE(publish(false, FLAG_PASSWORD));
  EXPECT_FALSE(current().active);
  EXPECT_EQ(current().flags, FLAG_NONE);
}

TEST(TextInputPacketTests, PacketTypeIsOutsideKnownExtensionRanges) {
  EXPECT_NE(PACKET_TYPE & 0xFF00, 0x5500);  // Sunshine extensions
  EXPECT_NE(PACKET_TYPE & 0xFF00, 0x3000);  // Apollo extensions
  EXPECT_EQ(sizeof(payload_t), 4u);
}

#ifdef _WIN32
  #include <chrono>
  #include <thread>
  #include <windows.h>

namespace {
  /**
   * Off-screen, never activated top level window hosting real Win32 controls, so UI Automation
   * classification can be verified without touching the user's desktop focus.
   */
  struct test_window_t {
    HWND parent = nullptr;

    test_window_t() {
      parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"NyxPlay text input test",
                               WS_POPUP, -32000, -32000, 400, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (parent) {
        ShowWindow(parent, SW_SHOWNOACTIVATE);
      }
    }

    ~test_window_t() {
      if (parent) {
        DestroyWindow(parent);
      }
    }

    HWND add(const wchar_t *cls, DWORD style, const wchar_t *text = L"") const {
      return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 200, 24, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
  };
}  // namespace

TEST(TextInputWindowsTests, EditControlIsClassifiedAsTextField) {
  test_window_t window;
  ASSERT_NE(window.parent, nullptr);
  auto edit = window.add(L"EDIT", WS_BORDER | ES_AUTOHSCROLL);
  ASSERT_NE(edit, nullptr);

  auto state = classify_window(edit);
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->active);
  EXPECT_EQ(state->flags & FLAG_PASSWORD, 0);
}

TEST(TextInputWindowsTests, PasswordEditCarriesPasswordFlag) {
  test_window_t window;
  auto edit = window.add(L"EDIT", WS_BORDER | ES_PASSWORD);
  ASSERT_NE(edit, nullptr);

  auto state = classify_window(edit);
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->active);
  EXPECT_EQ(state->flags & FLAG_PASSWORD, FLAG_PASSWORD);
}

TEST(TextInputWindowsTests, ReadOnlyEditIsNotATextField) {
  test_window_t window;
  auto edit = window.add(L"EDIT", WS_BORDER | ES_READONLY, L"read only");
  ASSERT_NE(edit, nullptr);

  auto state = classify_window(edit);
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->active);
}

TEST(TextInputWindowsTests, ButtonIsNotATextField) {
  test_window_t window;
  auto button = window.add(L"BUTTON", BS_PUSHBUTTON, L"OK");
  ASSERT_NE(button, nullptr);

  auto state = classify_window(button);
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->active);
}

TEST(TextInputWindowsTests, MonitorStartsAndPublishesAState) {
  using namespace std::chrono_literals;

  auto before = current().generation;
  {
    auto guard = acquire();
    ASSERT_NE(guard, nullptr);
    // A second acquire shares the same monitor
    auto second = acquire();
    EXPECT_EQ(guard.get(), second.get());

    // The monitor evaluates the currently focused element right after subscribing
    for (int i = 0; i < 50 && current().generation == before; i++) {
      std::this_thread::sleep_for(100ms);
    }
  }
  // Releasing the last guard stops the monitor and resets the state to inactive
  EXPECT_FALSE(current().active);
  EXPECT_NE(current().generation, 0u);
}
#endif
