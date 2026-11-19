/**
 * @file tests/unit/test_control_intercept.cpp
 * @brief Test the inspection of control stream datagrams (src/control_intercept.*), the anti-replay window and the
 *        decryption of control messages (src/stream.*).
 */
#include <src/replay_window.h>
#include <src/rtsp.h>
#include <src/stream.h>

#include "enet_scenarios.h"
#include "../tests_common.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

TEST(ControlIntercept, ForeignCommandsAreIgnored) {
  EXPECT_EQ(scn::foreign_commands_are_ignored(), 0);
}

TEST(ControlIntercept, ForeignPayloadNeedsAuthentication) {
  EXPECT_EQ(scn::foreign_payload_needs_authentication(), 0);
}

TEST(ControlIntercept, AuthenticatedPayloadMovesThePeer) {
  EXPECT_EQ(scn::authenticated_payload_moves_the_peer(), 0);
}

TEST(ControlIntercept, RoamingClientWithABacklogKeepsItsConnection) {
  EXPECT_EQ(scn::roaming_client_with_a_backlog_keeps_its_connection(), 0);
}

TEST(ControlIntercept, BogusConnectsDoNotBlockALegitimateOne) {
  EXPECT_EQ(scn::bogus_connects_do_not_block_a_legitimate_one(), 0);
}

TEST(ControlIntercept, ForeignDisconnectIsAcceptedWithoutTheInspection) {
  EXPECT_EQ(scn::foreign_disconnect_is_accepted_without_the_inspection(), 0);
}

TEST(ControlIntercept, BogusConnectsFillAnUnprotectedHost) {
  EXPECT_EQ(scn::bogus_connects_fill_an_unprotected_host(), 0);
}

TEST(ControlIntercept, WithoutHooksEverythingForeignAndUnexpectedIsDropped) {
  scn::pair_t pair;
  scn::injector_t injector;
  ASSERT_EQ(scn::open(pair, 0x42), 0);
  ASSERT_TRUE(injector.open());

  control_intercept::set_hooks(nullptr);

  auto datagram = scn::header(pair, pair.server_peer->incomingSessionID);
  scn::add_unsequenced(datagram, 1, "AUTH1");
  injector.send(pair.server_address, datagram);
  injector.send(pair.server_address, scn::connect_request(1, 0x42));
  scn::pump(pair, 4);

  EXPECT_TRUE(pair.received.empty());
  EXPECT_EQ(scn::peers_in_use(pair.server), 1);
  EXPECT_EQ(pair.server_peer->state, ENET_PEER_STATE_CONNECTED);
  scn::close(pair);
}

namespace {
  constexpr std::uint16_t encrypted_type = 0x0001;

  /// An encrypted control packet as a client builds it (type, length, sequence number, tag, ciphertext).
  std::string client_packet(crypto::cipher::gcm_t &cipher, bool v2, std::uint32_t seq, const std::string &plaintext) {
    crypto::aes_t iv;
    if (v2) {
      iv.assign(12, 0);
      std::memcpy(iv.data(), &seq, sizeof(seq));
      iv[10] = 'C';
      iv[11] = 'C';
    } else {
      iv.assign(16, 0);
      iv[0] = static_cast<std::uint8_t>(seq);
    }

    std::vector<std::uint8_t> tagged(plaintext.size() + 64);
    const int bytes = cipher.encrypt(plaintext, tagged.data(), &iv);
    EXPECT_GT(bytes, 0);

    // bytes is the size of the ciphertext, the tag precedes it
    const std::uint16_t length = static_cast<std::uint16_t>(bytes + crypto::cipher::tag_size + sizeof(seq));
    std::string packet;
    packet.append(reinterpret_cast<const char *>(&encrypted_type), sizeof(encrypted_type));
    packet.append(reinterpret_cast<const char *>(&length), sizeof(length));
    packet.append(reinterpret_cast<const char *>(&seq), sizeof(seq));
    packet.append(reinterpret_cast<const char *>(tagged.data()), bytes + crypto::cipher::tag_size);
    return packet;
  }

  crypto::aes_t test_key() {
    return crypto::aes_t(16, 0x5A);
  }
}  // namespace

TEST(ControlDecrypt, RoundTrip) {
  for (bool v2 : {true, false}) {
    crypto::cipher::gcm_t client {test_key()};
    crypto::cipher::gcm_t host {test_key()};
    const auto packet = client_packet(client, v2, 77, "0123456789abcdef-payload");

    crypto::aes_t iv;
    std::vector<std::uint8_t> plaintext;
    std::uint32_t seq = 0;
    ASSERT_EQ(stream::decrypt_control_message(host, iv, v2, packet, plaintext, seq), stream::control_decrypt_e::ok);
    EXPECT_EQ(seq, 77U);
    EXPECT_EQ(std::string(plaintext.begin(), plaintext.end()).substr(0, 16), "0123456789abcdef");
  }
}

TEST(ControlDecrypt, TamperedAndMalformedPackets) {
  crypto::cipher::gcm_t client {test_key()};
  crypto::cipher::gcm_t host {test_key()};
  const auto packet = client_packet(client, true, 5, "0123456789abcdef-payload");

  crypto::aes_t iv;
  std::vector<std::uint8_t> plaintext;
  std::uint32_t seq = 0;

  auto tampered = packet;
  tampered.back() ^= 0x01;
  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, tampered, plaintext, seq), stream::control_decrypt_e::bad_tag);

  // A different sequence number is a different IV
  auto resequenced = packet;
  resequenced[4] ^= 0x01;
  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, resequenced, plaintext, seq), stream::control_decrypt_e::bad_tag);

  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, std::string_view {packet}.substr(0, 6), plaintext, seq), stream::control_decrypt_e::malformed);
  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, std::string_view {packet}.substr(0, packet.size() - 1), plaintext, seq), stream::control_decrypt_e::malformed);
  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, std::string_view {}, plaintext, seq), stream::control_decrypt_e::malformed);

  auto short_length = packet;
  short_length[2] = 8;
  short_length[3] = 0;
  EXPECT_EQ(stream::decrypt_control_message(host, iv, true, short_length, plaintext, seq), stream::control_decrypt_e::malformed);

  auto other_key = crypto::cipher::gcm_t {crypto::aes_t(16, 0x11)};
  EXPECT_EQ(stream::decrypt_control_message(other_key, iv, true, packet, plaintext, seq), stream::control_decrypt_e::bad_tag);
}

TEST(ReplayWindow, AcceptsEachNumberOnce) {
  replay_window_t window;
  EXPECT_TRUE(window.accept(0));
  EXPECT_FALSE(window.accept(0));
  EXPECT_TRUE(window.accept(1));
  EXPECT_TRUE(window.accept(2));
  EXPECT_FALSE(window.accept(1));
  EXPECT_FALSE(window.accept(2));
  EXPECT_TRUE(window.accept(3));
}

TEST(ReplayWindow, FirstNumberMayBeAnything) {
  replay_window_t window;
  EXPECT_TRUE(window.accept(1000));
  EXPECT_FALSE(window.accept(1000));
  // Earlier numbers that arrive late are fine once
  EXPECT_TRUE(window.accept(998));
  EXPECT_FALSE(window.accept(998));
  EXPECT_TRUE(window.accept(0));
}

TEST(ReplayWindow, ReorderingWithinTheWindow) {
  replay_window_t window;
  for (std::uint32_t seq : {10U, 12U, 11U, 15U, 13U, 14U}) {
    EXPECT_TRUE(window.accept(seq)) << seq;
  }
  for (std::uint32_t seq = 10; seq <= 15; ++seq) {
    EXPECT_FALSE(window.accept(seq)) << seq;
  }
}

TEST(ReplayWindow, TooOldIsRejected) {
  replay_window_t window;
  ASSERT_TRUE(window.accept(5));
  ASSERT_TRUE(window.accept(5 + replay_window_t::size));
  EXPECT_FALSE(window.accept(5));
  EXPECT_FALSE(window.fresh(5));
  EXPECT_TRUE(window.accept(6));
  EXPECT_FALSE(window.accept(6));
}

TEST(ReplayWindow, LargeJumpForgetsTheOldNumbers) {
  replay_window_t window;
  ASSERT_TRUE(window.accept(1));
  ASSERT_TRUE(window.accept(1 + 3 * replay_window_t::size));
  EXPECT_FALSE(window.accept(1 + 3 * replay_window_t::size));
  EXPECT_TRUE(window.accept(2 + 3 * replay_window_t::size));
  EXPECT_TRUE(window.accept(3 * replay_window_t::size));
}

TEST(ReplayWindow, FreshDoesNotChangeTheWindow) {
  replay_window_t window;
  EXPECT_TRUE(window.fresh(4));
  EXPECT_TRUE(window.fresh(4));
  EXPECT_TRUE(window.accept(4));
  EXPECT_FALSE(window.fresh(4));
}

TEST(ReplayWindow, SurvivesTheWrapOfTheSequenceNumber) {
  replay_window_t window;
  EXPECT_TRUE(window.accept(0xFFFFFFFEU));
  EXPECT_TRUE(window.accept(0xFFFFFFFFU));
  EXPECT_TRUE(window.accept(0));
  EXPECT_TRUE(window.accept(1));
  EXPECT_FALSE(window.accept(0xFFFFFFFFU));
  EXPECT_FALSE(window.accept(0));
}

TEST(ControlIntercept, EncryptedMessageFromANewAddressMovesTheConnectionOnce) {
  crypto::cipher::gcm_t client {test_key()};
  crypto::cipher::gcm_t host {test_key()};
  crypto::aes_t iv;
  replay_window_t window;

  scn::pair_t pair;
  scn::injector_t first;
  scn::injector_t second;
  scn::injector_t third;
  ASSERT_EQ(scn::open(pair, 0x77), 0);
  ASSERT_TRUE(first.open());
  ASSERT_TRUE(second.open());
  ASSERT_TRUE(third.open());

  pair.hooks.authenticate = [&](ENetPeer *, std::string_view message) {
    std::vector<std::uint8_t> plaintext;
    std::uint32_t seq = 0;
    if (stream::decrypt_control_message(host, iv, true, message, plaintext, seq) != stream::control_decrypt_e::ok) {
      return control_intercept::message_e::invalid;
    }
    return window.fresh(seq) ? control_intercept::message_e::fresh : control_intercept::message_e::replayed;
  };

  const auto send_from = [&](scn::injector_t &source, std::uint32_t seq, const std::string &packet) {
    auto datagram = scn::header(pair, pair.server_peer->incomingSessionID);
    scn::add_unsequenced(datagram, seq + 1, packet);
    source.send(pair.server_address, datagram);
    scn::pump(pair, 4);
  };

  // Message 0 arrives from a new address: the connection moves, the message is delivered
  const auto message0 = client_packet(client, true, 0, "message zero....");
  send_from(first, 0, message0);
  EXPECT_TRUE(enet_address_equal(&pair.server_peer->address, &first.address));
  ASSERT_EQ(pair.received.size(), 1U);
  EXPECT_EQ(pair.received[0], message0);
  // The handler of the application accepts it
  ASSERT_TRUE(window.accept(0));

  // A copy of it from yet another address does nothing
  pair.received.clear();
  send_from(second, 1, message0);
  EXPECT_TRUE(enet_address_equal(&pair.server_peer->address, &first.address));
  EXPECT_TRUE(pair.received.empty());

  // A tampered new message does nothing
  auto tampered = client_packet(client, true, 1, "message one.....");
  tampered.back() ^= 0x40;
  send_from(third, 2, tampered);
  EXPECT_TRUE(enet_address_equal(&pair.server_peer->address, &first.address));
  EXPECT_TRUE(pair.received.empty());

  // A fresh message from the second address moves the connection again
  const auto message1 = client_packet(client, true, 1, "message one.....");
  send_from(second, 3, message1);
  EXPECT_TRUE(enet_address_equal(&pair.server_peer->address, &second.address));
  EXPECT_EQ(pair.received.size(), 1U);
  EXPECT_EQ(pair.server_disconnects, 0);

  scn::close(pair);
}
