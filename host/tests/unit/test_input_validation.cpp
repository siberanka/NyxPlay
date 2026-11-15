/**
 * @file tests/unit/test_input_validation.cpp
 * @brief Test input::validate_input_packet() bounds checking of untrusted input packets.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <src/input.h>
#include <src/utility.h>

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include "../tests_common.h"

namespace {
  struct input_packet_spec_t {
    std::uint32_t magic;
    std::size_t size;
  };

  /**
   * @brief Build a raw packet buffer with a header followed by zeroed bytes.
   * @param magic Packet magic (host order).
   * @param declared_size Size value written to the header (excludes the size field itself).
   * @param actual_size Number of bytes in the returned buffer.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

  bool valid(const std::vector<std::uint8_t> &packet) {
    return input::validate_input_packet(packet);
  }
}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(valid(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(valid(packet)) << "declared_size=" << declared_size;
  }

  EXPECT_FALSE(valid(make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER))));
  EXPECT_FALSE(valid(make_input_packet(unknown_magic, std::numeric_limits<std::uint32_t>::max(), sizeof(NV_INPUT_HEADER))));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  EXPECT_TRUE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER))));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  EXPECT_TRUE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(std::uint32_t) + maximum_declared_size)));

  // One byte over the text limit is rejected even though the buffer holds it
  EXPECT_FALSE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size + 1, sizeof(std::uint32_t) + maximum_declared_size + 1)));

  // Declared text longer than the buffer
  EXPECT_FALSE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER))));
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));

    EXPECT_TRUE(valid(make_input_packet(magic, declared_size, size))) << "magic=" << magic;
    EXPECT_TRUE(valid(make_input_packet(magic, declared_size, size + 8))) << "trailing, magic=" << magic;
    EXPECT_FALSE(valid(make_input_packet(magic, declared_size - 1, size))) << "short declared, magic=" << magic;
    // Newer clients may send longer packets, the extra bytes are ignored
    EXPECT_TRUE(valid(make_input_packet(magic, declared_size + 1, size + 1))) << "long declared, magic=" << magic;
    EXPECT_FALSE(valid(make_input_packet(magic, declared_size + 1, size))) << "declared beyond buffer, magic=" << magic;
    EXPECT_FALSE(valid(make_input_packet(magic, declared_size, size - 1))) << "truncated, magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  EXPECT_TRUE(valid(make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER))));
}

TEST(InputPacketValidationTest, AcceptsMaxLengthUtf8AndRejectsLonger) {
  for (std::uint32_t text_size : {1u, 31u, 32u}) {
    const auto declared = static_cast<std::uint32_t>(sizeof(std::uint32_t)) + text_size;
    EXPECT_TRUE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, declared, sizeof(std::uint32_t) + declared))) << text_size;
  }
  EXPECT_FALSE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t) + 33, sizeof(std::uint32_t) + sizeof(std::uint32_t) + 33)));
  EXPECT_FALSE(valid(make_input_packet(UTF8_TEXT_EVENT_MAGIC, 0xFFFFFFF0u, 64)));
}

namespace {
  constexpr std::array known_magics {
    std::uint32_t {MOUSE_MOVE_REL_MAGIC_GEN5},
    std::uint32_t {MOUSE_MOVE_ABS_MAGIC},
    std::uint32_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5},
    std::uint32_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5},
    std::uint32_t {SCROLL_MAGIC_GEN5},
    std::uint32_t {SS_HSCROLL_MAGIC},
    std::uint32_t {KEY_DOWN_EVENT_MAGIC},
    std::uint32_t {KEY_UP_EVENT_MAGIC},
    std::uint32_t {UTF8_TEXT_EVENT_MAGIC},
    std::uint32_t {MULTI_CONTROLLER_MAGIC_GEN5},
    std::uint32_t {SS_TOUCH_MAGIC},
    std::uint32_t {SS_PEN_MAGIC},
    std::uint32_t {SS_CONTROLLER_ARRIVAL_MAGIC},
    std::uint32_t {SS_CONTROLLER_TOUCH_MAGIC},
    std::uint32_t {SS_CONTROLLER_MOTION_MAGIC},
    std::uint32_t {SS_CONTROLLER_BATTERY_MAGIC},
  };

  crypto::PERM owner_of(std::uint32_t magic) {
    switch (magic) {
      case MULTI_CONTROLLER_MAGIC_GEN5:
      case SS_CONTROLLER_ARRIVAL_MAGIC:
      case SS_CONTROLLER_TOUCH_MAGIC:
      case SS_CONTROLLER_MOTION_MAGIC:
      case SS_CONTROLLER_BATTERY_MAGIC:
        return crypto::PERM::input_controller;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
      case UTF8_TEXT_EVENT_MAGIC:
        return crypto::PERM::input_kbd;
      case SS_TOUCH_MAGIC:
        return crypto::PERM::input_touch;
      case SS_PEN_MAGIC:
        return crypto::PERM::input_pen;
      default:
        return crypto::PERM::input_mouse;
    }
  }

  std::vector<std::uint8_t> header_only(std::uint32_t magic) {
    return make_input_packet(magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  }

  short be16(int value) {
    return util::endian::big(static_cast<short>(value));
  }

  int host16(short value) {
    return util::endian::big(value);
  }

  template<typename Packet>
  std::vector<std::uint8_t> make_typed(std::uint32_t magic) {
    return make_input_packet(magic, sizeof(Packet) - sizeof(std::uint32_t), sizeof(Packet));
  }

  template<typename Packet>
  Packet *as(std::vector<std::uint8_t> &v) {
    return reinterpret_cast<Packet *>(v.data());
  }
}  // namespace

TEST(InputPermissionTest, EachGroupNeedsItsOwnBit) {
  constexpr std::array bits {
    crypto::PERM::input_controller,
    crypto::PERM::input_touch,
    crypto::PERM::input_pen,
    crypto::PERM::input_mouse,
    crypto::PERM::input_kbd,
  };
  for (auto magic : known_magics) {
    const auto packet = header_only(magic);
    for (auto bit : bits) {
      EXPECT_EQ(input::testing::is_permitted(bit, packet), bit == owner_of(magic)) << "magic=" << magic;
    }
    EXPECT_TRUE(input::testing::is_permitted(crypto::PERM::_all_inputs, packet)) << "magic=" << magic;
  }
}

TEST(InputPermissionTest, NoInputBitsDropsEverything) {
  for (auto magic : known_magics) {
    EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::_no, header_only(magic)));
    EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::_default, header_only(magic)));
    EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::_all_opeiations, header_only(magic)));
  }
}

TEST(InputPermissionTest, UnknownMagicIsNeverQueued) {
  const auto unknown = header_only(0x12345678);
  EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::input_mouse, unknown));
  EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::_all_inputs, unknown));
  EXPECT_FALSE(input::testing::is_permitted(crypto::PERM::_all, unknown));
}

TEST(InputBatchTest, RelativeMouseSumsAndStopsOnOverflow) {
  using input::testing::batch_outcome_e;
  auto dest = make_typed<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  auto src = dest;
  as<NV_REL_MOUSE_MOVE_PACKET>(dest)->deltaX = be16(100);
  as<NV_REL_MOUSE_MOVE_PACKET>(src)->deltaX = be16(200);
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::batched);
  EXPECT_EQ(host16(as<NV_REL_MOUSE_MOVE_PACKET>(dest)->deltaX), 300);

  as<NV_REL_MOUSE_MOVE_PACKET>(dest)->deltaX = be16(32000);
  as<NV_REL_MOUSE_MOVE_PACKET>(src)->deltaX = be16(1000);
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);
  EXPECT_EQ(host16(as<NV_REL_MOUSE_MOVE_PACKET>(dest)->deltaX), 32000);

  as<NV_REL_MOUSE_MOVE_PACKET>(dest)->deltaX = be16(-32000);
  as<NV_REL_MOUSE_MOVE_PACKET>(src)->deltaX = be16(-1000);
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);
}

TEST(InputBatchTest, ScrollOverflowTerminates) {
  using input::testing::batch_outcome_e;
  auto dest = make_typed<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  auto src = dest;
  as<NV_SCROLL_PACKET>(dest)->scrollAmt1 = be16(120);
  as<NV_SCROLL_PACKET>(src)->scrollAmt1 = be16(120);
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::batched);
  EXPECT_EQ(as<NV_SCROLL_PACKET>(dest)->scrollAmt1, as<NV_SCROLL_PACKET>(dest)->scrollAmt2);
  EXPECT_EQ(host16(as<NV_SCROLL_PACKET>(dest)->scrollAmt1), 240);

  as<NV_SCROLL_PACKET>(dest)->scrollAmt1 = be16(32767);
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);
}

TEST(InputBatchTest, DifferentMagicNeverBatches) {
  using input::testing::batch_outcome_e;
  auto key = make_typed<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC);
  auto mouse = make_typed<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  EXPECT_EQ(input::testing::batch_messages(key, mouse), batch_outcome_e::terminate_batch);
  EXPECT_EQ(input::testing::batch_messages(mouse, key), batch_outcome_e::terminate_batch);
  auto key2 = key;
  EXPECT_EQ(input::testing::batch_messages(key, key2), batch_outcome_e::terminate_batch);
}

TEST(InputBatchTest, TouchBatchesOnlySamePointerMoves) {
  using input::testing::batch_outcome_e;
  auto dest = make_typed<SS_TOUCH_PACKET>(SS_TOUCH_MAGIC);
  auto src = dest;
  as<SS_TOUCH_PACKET>(dest)->eventType = LI_TOUCH_EVENT_MOVE;
  as<SS_TOUCH_PACKET>(src)->eventType = LI_TOUCH_EVENT_MOVE;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::batched);

  as<SS_TOUCH_PACKET>(src)->pointerId = 7;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::not_batchable);

  as<SS_TOUCH_PACKET>(src)->pointerId = 0;
  as<SS_TOUCH_PACKET>(src)->eventType = LI_TOUCH_EVENT_UP;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);

  as<SS_TOUCH_PACKET>(dest)->eventType = LI_TOUCH_EVENT_DOWN;
  as<SS_TOUCH_PACKET>(src)->eventType = LI_TOUCH_EVENT_MOVE;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);
}

TEST(InputBatchTest, ControllerBatchingRules) {
  using input::testing::batch_outcome_e;
  auto dest = make_typed<NV_MULTI_CONTROLLER_PACKET>(MULTI_CONTROLLER_MAGIC_GEN5);
  auto src = dest;
  as<NV_MULTI_CONTROLLER_PACKET>(src)->leftStickX = 1234;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::batched);
  EXPECT_EQ(as<NV_MULTI_CONTROLLER_PACKET>(dest)->leftStickX, 1234);

  as<NV_MULTI_CONTROLLER_PACKET>(src)->controllerNumber = 1;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::not_batchable);

  as<NV_MULTI_CONTROLLER_PACKET>(src)->controllerNumber = 0;
  as<NV_MULTI_CONTROLLER_PACKET>(src)->buttonFlags = 1;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);

  as<NV_MULTI_CONTROLLER_PACKET>(src)->buttonFlags = 0;
  as<NV_MULTI_CONTROLLER_PACKET>(src)->activeGamepadMask = 3;
  EXPECT_EQ(input::testing::batch_messages(dest, src), batch_outcome_e::terminate_batch);
}

TEST(InputFloatTest, ClampedRejectsNonFinite) {
  constexpr float inf = std::numeric_limits<float>::infinity();
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_EQ(input::testing::clamp_or_min(nan, 0.0f, 1.0f), 0.0f);
  EXPECT_EQ(input::testing::clamp_or_min(inf, 0.0f, 1.0f), 0.0f);
  EXPECT_EQ(input::testing::clamp_or_min(-inf, 0.0f, 1.0f), 0.0f);
  EXPECT_EQ(input::testing::clamp_or_min(2.0f, 0.0f, 1.0f), 1.0f);
  EXPECT_EQ(input::testing::clamp_or_min(-1.0f, 0.0f, 1.0f), 0.0f);
  EXPECT_EQ(input::testing::clamp_or_min(0.25f, 0.0f, 1.0f), 0.25f);
}

TEST(InputFloatTest, FiniteLimitsMotionValues) {
  constexpr float inf = std::numeric_limits<float>::infinity();
  EXPECT_EQ(input::testing::limit_finite(std::numeric_limits<float>::quiet_NaN(), 4000.0f), 0.0f);
  EXPECT_EQ(input::testing::limit_finite(inf, 4000.0f), 0.0f);
  EXPECT_EQ(input::testing::limit_finite(1e30f, 4000.0f), 4000.0f);
  EXPECT_EQ(input::testing::limit_finite(-1e30f, 4000.0f), -4000.0f);
  EXPECT_EQ(input::testing::limit_finite(12.5f, 4000.0f), 12.5f);
}
