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
    EXPECT_FALSE(valid(make_input_packet(magic, declared_size + 1, size + 1))) << "long declared, magic=" << magic;
    EXPECT_FALSE(valid(make_input_packet(magic, declared_size, size - 1))) << "truncated, magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  EXPECT_TRUE(valid(make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER))));
}
