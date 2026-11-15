/**
 * @file tests/unit/test_rtsp_peer_binding.cpp
 * @brief Test the address binding and the replay protection of the RTSP handshake of src/rtsp.*
 */
#include <src/config.h>
#include <src/rtsp.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../tests_common.h"

namespace {
  namespace ip = boost::asio::ip;

  ip::address addr(const char *text) {
    return ip::make_address(text);
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch(uint32_t id, const char *client, bool encrypted) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->id = id;
    session->unique_id = "client";
    session->remote_address = rtsp_stream::peer_address_key(addr(client));

    if (encrypted) {
      crypto::aes_t key(16, 0x42);
      session->gcm_key = key;
      session->rtsp_cipher = crypto::cipher::gcm_t {key, false};
    }

    return session;
  }

  /// The IV of a client originated RTSP message, as built by the handshake.
  crypto::aes_t client_iv(std::uint32_t seq) {
    crypto::aes_t iv(12);
    std::copy_n((std::uint8_t *) &seq, sizeof(seq), std::begin(iv));
    iv[10] = 'C';
    iv[11] = 'R';
    return iv;
  }
}  // namespace

TEST(RtspPeerBinding, PeerAddressKeyNormalisesTheAddress) {
  EXPECT_EQ(rtsp_stream::peer_address_key(addr("192.168.1.5")), "192.168.1.5");
  EXPECT_EQ(rtsp_stream::peer_address_key(addr("::ffff:192.168.1.5")), "192.168.1.5");
  EXPECT_EQ(rtsp_stream::peer_address_key(addr("fe80::1%3")), "fe80::1");
  EXPECT_EQ(rtsp_stream::peer_address_key(addr("fe80::1%7")), rtsp_stream::peer_address_key(addr("fe80::1")));
  EXPECT_EQ(rtsp_stream::peer_address_key(addr("2001:DB8::A")), rtsp_stream::peer_address_key(addr("2001:db8::a")));
  EXPECT_NE(rtsp_stream::peer_address_key(addr("192.168.1.5")), rtsp_stream::peer_address_key(addr("192.168.1.6")));
}

TEST(RtspPeerBinding, EncryptedLaunchAcceptsAnotherAddress) {
  // Carrier NAT, VPN or a dual-stack fallback move the RTSP connection to another address: the key is the proof
  auto launch = make_launch(1, "203.0.113.7", true);

  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("203.0.113.7")));
  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("198.51.100.99")));
  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("2001:db8::1")));
}

TEST(RtspPeerBinding, PlaintextLaunchRefusesAnotherAddress) {
  auto launch = make_launch(1, "192.168.1.5", false);

  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.5")));
  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("::ffff:192.168.1.5")));
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.6")));
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("203.0.113.7")));
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("2001:db8::1")));
}

TEST(RtspPeerBinding, PlaintextLaunchWithoutRecordedAddressServesNobody) {
  auto launch = make_launch(1, "192.168.1.5", false);
  launch->remote_address.clear();

  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.5")));
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("127.0.0.1")));
}

TEST(RtspPeerBinding, PlaintextRepeatedAnnounceFromAnotherAddressRefused) {
  using rtsp_stream::launch_slot_t;

  launch_slot_t slot;
  auto launch = make_launch(1, "192.168.1.5", false);
  slot.raise(launch);

  // The launching client announces first, the stream exists
  ASSERT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.5")));
  ASSERT_TRUE(launch_slot_t::claim_announce(*launch));

  // A repeated announce is only a retry when it comes from the same address
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.66")));
  EXPECT_FALSE(rtsp_stream::rtsp_peer_allowed(*launch, addr("10.0.0.1")));
  EXPECT_TRUE(rtsp_stream::rtsp_peer_allowed(*launch, addr("192.168.1.5")));
}

TEST(RtspPeerBinding, EncryptedSequenceNumbersAreAcceptedOnceInOrder) {
  auto launch = make_launch(1, "203.0.113.7", true);

  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*launch, 0));
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*launch, 1));
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*launch, 2));

  // Replay of an earlier message
  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*launch, 2));
  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*launch, 1));

  // A client may skip numbers, but never go back
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*launch, 9));
  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*launch, 5));
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*launch, 10));
}

TEST(RtspPeerBinding, SequenceNumbersAreTrackedPerLaunch) {
  auto first = make_launch(1, "203.0.113.7", true);
  auto second = make_launch(2, "203.0.113.7", true);

  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*first, 7));
  // A new launch has a new key and starts over
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*second, 1));
  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*first, 7));
}

TEST(RtspPeerBinding, ReplayedEncryptedAnnounceIsRefused) {
  auto launch = make_launch(1, "203.0.113.7", true);

  constexpr std::uint32_t seq = 6;
  const std::string announce = "ANNOUNCE streamid=video/0/0 RTSP/1.0\r\nCSeq: 6\r\n\r\n";

  // Sealed like the client does it: the tag followed by the ciphertext
  std::string wire(announce.size() + 16, '\0');
  auto iv = client_iv(seq);
  crypto::cipher::gcm_t client_cipher {launch->gcm_key, false};
  ASSERT_GE(client_cipher.encrypt(announce, (std::uint8_t *) wire.data(), &iv), 0);

  // The tag only proves the key: a recorded message verifies every time it is delivered
  for (int delivery = 0; delivery < 2; ++delivery) {
    std::vector<std::uint8_t> plain;
    auto verify_iv = client_iv(seq);
    EXPECT_EQ(launch->rtsp_cipher->decrypt(wire, plain, &verify_iv), 0);
    EXPECT_EQ(std::string(plain.begin(), plain.end()), announce);
  }

  // The sequence check lets the first delivery pass and stops the replay
  EXPECT_TRUE(rtsp_stream::rtsp_accept_sequence(*launch, seq));
  EXPECT_FALSE(rtsp_stream::rtsp_accept_sequence(*launch, seq));

  // A forged message has no valid tag, whatever its sequence number is
  std::string forged = wire;
  forged[20] ^= 0x01;
  std::vector<std::uint8_t> plain;
  auto forged_iv = client_iv(seq + 1);
  EXPECT_NE(launch->rtsp_cipher->decrypt(forged, plain, &forged_iv), 0);
}

TEST(RtspPeerBinding, ConcurrentDeliveryOfOneSequenceNumberIsAcceptedOnce) {
  auto launch = make_launch(1, "203.0.113.7", true);
  std::atomic<int> accepted {0};

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&]() {
      if (rtsp_stream::rtsp_accept_sequence(*launch, 42)) {
        ++accepted;
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(accepted.load(), 1);
}

TEST(RtspPlaintextOption, DefaultIsLocalNetworkOnly) {
  EXPECT_EQ(config::stream.rtsp_plaintext, config::RTSP_PLAINTEXT_LAN_ONLY);
}

TEST(RtspPlaintextOption, NeverRefusesEveryClient) {
  for (auto text : {"127.0.0.1", "192.168.1.5", "10.1.2.3", "::1", "fe80::1", "203.0.113.7", "2001:db8::1"}) {
    EXPECT_FALSE(rtsp_stream::plaintext_rtsp_allowed(config::RTSP_PLAINTEXT_NEVER, addr(text))) << text;
  }
}

TEST(RtspPlaintextOption, LanOnlyAcceptsLocalNetworksOnly) {
  const int mode = config::RTSP_PLAINTEXT_LAN_ONLY;

  for (auto text : {"127.0.0.1", "::1", "192.168.1.5", "172.16.4.4", "10.1.2.3", "100.64.0.9", "169.254.1.1", "fe80::1", "fc00::5", "::ffff:192.168.1.5"}) {
    EXPECT_TRUE(rtsp_stream::plaintext_rtsp_allowed(mode, addr(text))) << text;
  }

  for (auto text : {"203.0.113.7", "8.8.8.8", "2001:db8::1", "::ffff:203.0.113.7", "172.32.0.1"}) {
    EXPECT_FALSE(rtsp_stream::plaintext_rtsp_allowed(mode, addr(text))) << text;
  }
}

TEST(RtspPlaintextOption, AlwaysAcceptsEveryClient) {
  for (auto text : {"127.0.0.1", "192.168.1.5", "203.0.113.7", "2001:db8::1"}) {
    EXPECT_TRUE(rtsp_stream::plaintext_rtsp_allowed(config::RTSP_PLAINTEXT_ALWAYS, addr(text))) << text;
  }
}

TEST(RtspPlaintextOption, UnknownValueIsTreatedAsNever) {
  EXPECT_FALSE(rtsp_stream::plaintext_rtsp_allowed(-1, addr("192.168.1.5")));
  EXPECT_FALSE(rtsp_stream::plaintext_rtsp_allowed(3, addr("192.168.1.5")));
  EXPECT_FALSE(rtsp_stream::plaintext_rtsp_allowed(99, addr("127.0.0.1")));
}

TEST(RtspConnectionLimit, GroupsIpv6ByPrefix) {
  EXPECT_EQ(rtsp_stream::peer_group_key(addr("2001:db8:1:2::1")), rtsp_stream::peer_group_key(addr("2001:db8:1:2:ffff::9")));
  EXPECT_NE(rtsp_stream::peer_group_key(addr("2001:db8:1:2::1")), rtsp_stream::peer_group_key(addr("2001:db8:1:3::1")));
  EXPECT_EQ(rtsp_stream::peer_group_key(addr("::ffff:192.168.1.5")), "192.168.1.5");
  EXPECT_NE(rtsp_stream::peer_group_key(addr("192.168.1.5")), rtsp_stream::peer_group_key(addr("192.168.1.6")));
}

TEST(RtspConnectionLimit, RefusesTheConnectionAfterThePerPeerCap) {
  rtsp_stream::connection_limiter_t limiter {64, 3, 4};

  EXPECT_TRUE(limiter.try_acquire("a", false));
  EXPECT_TRUE(limiter.try_acquire("a", false));
  EXPECT_TRUE(limiter.try_acquire("a", false));
  EXPECT_FALSE(limiter.try_acquire("a", false));
  EXPECT_EQ(limiter.count("a"), 3);

  // Another address is not affected
  EXPECT_TRUE(limiter.try_acquire("b", false));
  EXPECT_EQ(limiter.total(), 4);
}

TEST(RtspConnectionLimit, LaunchingPeerGetsOneMore) {
  rtsp_stream::connection_limiter_t limiter {64, 3, 4};

  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(limiter.try_acquire("a", true)) << i;
  }
  EXPECT_FALSE(limiter.try_acquire("a", true));
  EXPECT_EQ(limiter.count("a"), 4);
}

TEST(RtspConnectionLimit, GlobalCapAppliesToAllPeers) {
  rtsp_stream::connection_limiter_t limiter {5, 3, 4};

  for (auto key : {"a", "a", "a", "b", "b"}) {
    EXPECT_TRUE(limiter.try_acquire(key, false));
  }
  EXPECT_FALSE(limiter.try_acquire("c", false));
  EXPECT_FALSE(limiter.try_acquire("c", true));
  EXPECT_EQ(limiter.total(), 5);
}

TEST(RtspConnectionLimit, ReleasedSlotsReturnToZero) {
  rtsp_stream::connection_limiter_t limiter {64, 3, 4};

  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(limiter.try_acquire("a", false));
  }
  ASSERT_FALSE(limiter.try_acquire("a", false));

  for (int i = 0; i < 3; ++i) {
    limiter.release("a");
  }
  EXPECT_EQ(limiter.count("a"), 0);
  EXPECT_EQ(limiter.total(), 0);

  // Extra or unknown releases do not drive the counters negative
  limiter.release("a");
  limiter.release("never");
  EXPECT_EQ(limiter.total(), 0);
  EXPECT_TRUE(limiter.try_acquire("a", false));
}

TEST(RtspConnectionLimit, ConcurrentUseKeepsTheCaps) {
  rtsp_stream::connection_limiter_t limiter {64, 3, 4};
  std::atomic<int> granted {0};
  std::vector<std::thread> threads;

  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 100; ++i) {
        if (limiter.try_acquire("a", false)) {
          ++granted;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(granted.load(), 3);
  EXPECT_EQ(limiter.count("a"), 3);
}
