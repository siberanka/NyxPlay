/**
 * @file tests/unit/test_common_c.cpp
 * @brief Test the host side of the shared moonlight-common-c copy (common/moonlight-common-c):
 *        C++ compatibility of the headers, the shared protocol extension constants and the
 *        encrypted control message vectors that common/tests/t_control.c checks on the library side.
 */
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}
#include <moonlight-common-c/src/NyxExtensions.h>

#include <src/crypto.h>
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/text_input.h>

#include "../tests_common.h"

namespace {
  std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back((std::uint8_t) std::stoi(std::string {hex.substr(i, 2)}, nullptr, 16));
    }
    return out;
  }

  std::string_view view(const std::vector<std::uint8_t> &v) {
    return {(const char *) v.data(), v.size()};
  }

  crypto::aes_t vector_key() {
    crypto::aes_t key(16);
    for (std::size_t i = 0; i < key.size(); i++) {
      key[i] = (std::uint8_t) i;
    }
    return key;
  }

  // Same vectors as common/tests/t_control.c
  constexpr std::string_view client_to_host_hex = "01002000050000009139b8d9f73482e1312dc4aa17844b3ca7fe544c76f9157c1ce78319";
  constexpr std::string_view host_to_client_hex = "01001c0009000000cbc66978f1d6e8c86fb230b21ce46cafc6eafffb12646ff3";
}  // namespace

TEST(CommonC, HeadersAreUsableFromCxx20) {
  // The library headers use C11 atomics; lc_atomic.h maps them to std::atomic for C++
  ::atomic_bool flag {false};
  ::atomic_int counter {0};
  flag = true;
  counter++;
  EXPECT_TRUE(flag);
  EXPECT_EQ(counter, 1);
  EXPECT_EQ(CTRL_CHANNEL_SERVERCTL, 0x08);
}

TEST(CommonC, ExtensionConstantsAreShared) {
  EXPECT_EQ(text_input::PACKET_TYPE, 0x4E01);
  EXPECT_EQ(text_input::PACKET_TYPE, LI_PACKET_TYPE_SET_TEXT_INPUT_STATE);
  EXPECT_EQ(text_input::PAYLOAD_VERSION, LI_TEXT_INPUT_PAYLOAD_VERSION);
  EXPECT_EQ(text_input::FLAG_PASSWORD, LI_TEXT_INPUT_FLAG_PASSWORD);
  EXPECT_EQ(text_input::FLAG_MULTILINE, LI_TEXT_INPUT_FLAG_MULTILINE);
  EXPECT_EQ(sizeof(text_input::payload_t), (std::size_t) LI_TEXT_INPUT_PAYLOAD_SIZE);
  EXPECT_EQ(LI_PACKET_TYPE_EXEC_SERVER_CMD, 0x3000);
  EXPECT_EQ(LI_PACKET_TYPE_SET_CLIPBOARD, 0x3001);
  EXPECT_EQ(LI_PACKET_TYPE_FILE_TRANSFER_NONCE_REQUEST, 0x3002);
  EXPECT_EQ(LI_PACKET_TYPE_SET_ADAPTIVE_TRIGGERS, 0x5503);
}

TEST(CommonC, ClientMessageSealedByTheLibraryDecryptsOnTheHost) {
  auto packet = from_hex(client_to_host_hex);
  crypto::cipher::gcm_t cipher {vector_key(), false};
  crypto::aes_t iv;
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  ASSERT_EQ(stream::decrypt_control_message(cipher, iv, true, view(packet), plaintext, seq), stream::control_decrypt_e::ok);
  EXPECT_EQ(seq, 5u);
  // Control header v2 (type 0x0206 input data, length 8) and the payload
  EXPECT_EQ(plaintext, from_hex("06020800" "0102030405060708"));

  // A flipped bit in the ciphertext, and a flipped bit in the sequence number, fail the tag check
  auto tampered = packet;
  tampered.back() ^= 0x01;
  EXPECT_EQ(stream::decrypt_control_message(cipher, iv, true, view(tampered), plaintext, seq), stream::control_decrypt_e::bad_tag);
  tampered = packet;
  tampered[4] ^= 0x01;
  EXPECT_EQ(stream::decrypt_control_message(cipher, iv, true, view(tampered), plaintext, seq), stream::control_decrypt_e::bad_tag);
}

TEST(CommonC, HostMessageEqualsTheVectorTheLibraryDecrypts) {
  // The host builds the text input state message exactly like this (stream.cpp encode_control)
  text_input::state_t state;
  state.active = true;
  state.flags = text_input::FLAG_PASSWORD | text_input::FLAG_MULTILINE;
  auto payload = text_input::to_payload(state);

  std::vector<std::uint8_t> plaintext {
    (std::uint8_t) (text_input::PACKET_TYPE & 0xFF), (std::uint8_t) (text_input::PACKET_TYPE >> 8), 4, 0
  };
  const auto *p = (const std::uint8_t *) &payload;
  plaintext.insert(plaintext.end(), p, p + sizeof(payload));
  ASSERT_EQ(plaintext, from_hex("014e0400" "01010300"));

  crypto::cipher::gcm_t cipher {vector_key(), false};
  crypto::aes_t iv(12);
  std::uint32_t seq = 9;
  iv[0] = (std::uint8_t) seq;
  iv[10] = 'H';  // Host originated
  iv[11] = 'C';  // Control stream

  std::vector<std::uint8_t> sealed(sizeof(std::uint32_t) + crypto::cipher::tag_size + plaintext.size());
  int bytes = cipher.encrypt(view(plaintext), sealed.data() + 4, &iv);
  ASSERT_EQ(bytes, (int) plaintext.size());  // the length of the ciphertext without the tag, as encode_control() uses it

  std::vector<std::uint8_t> packet {0x01, 0x00, (std::uint8_t) (sealed.size()), 0x00, (std::uint8_t) seq, 0, 0, 0};
  packet.insert(packet.end(), sealed.begin() + 4, sealed.end());
  EXPECT_EQ(packet, from_hex(host_to_client_hex));
}
