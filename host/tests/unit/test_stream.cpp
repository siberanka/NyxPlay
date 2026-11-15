/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}

#include <src/rtsp.h>
#include <src/stream.h>

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

#include <atomic>
#include <chrono>
#include <thread>

#include <boost/asio.hpp>

#include <src/rtsp.h>
#include <src/rtsp.h>
#include <src/stream.h>
#include <src/thread_safe.h>

namespace stream {
  using message_queue_t = std::shared_ptr<safe::queue_t<std::pair<boost::asio::ip::udp::endpoint, std::string>>>;

  int await_ping(const message_queue_t &messages, bool legacy_ping_allowed, std::string_view expected_payload, safe::event_t<bool> &shutdown_event, std::chrono::milliseconds timeout, boost::asio::ip::udp::endpoint &peer);
}

namespace {
  stream::message_queue_t make_ping_queue() {
    return std::make_shared<stream::message_queue_t::element_type>(30);
  }

  boost::asio::ip::udp::endpoint make_peer(unsigned short port) {
    return {boost::asio::ip::make_address("127.0.0.1"), port};
  }
}  // namespace

TEST(AwaitPing, StopsOnShutdown) {
  auto messages = make_ping_queue();
  safe::event_t<bool> shutdown;
  boost::asio::ip::udp::endpoint peer;

  std::thread raiser([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    shutdown.raise(true);
  });

  auto start = std::chrono::steady_clock::now();
  auto res = stream::await_ping(messages, false, "payload", shutdown, std::chrono::seconds(30), peer);
  auto elapsed = std::chrono::steady_clock::now() - start;
  raiser.join();

  EXPECT_EQ(res, -1);
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
}

TEST(AwaitPing, MatchingPingIsAccepted) {
  auto messages = make_ping_queue();
  safe::event_t<bool> shutdown;
  boost::asio::ip::udp::endpoint peer;

  messages->raise(make_peer(5000), std::string {"xxpayloadxx"});
  EXPECT_EQ(stream::await_ping(messages, false, "payload", shutdown, std::chrono::seconds(5), peer), 0);
  EXPECT_EQ(peer.port(), 5000);
}

TEST(AwaitPing, LegacyPingOnlyWhenAllowed) {
  safe::event_t<bool> shutdown;
  boost::asio::ip::udp::endpoint peer;

  auto messages = make_ping_queue();
  messages->raise(make_peer(5001), std::string {"PING"});
  EXPECT_EQ(stream::await_ping(messages, true, "payload", shutdown, std::chrono::seconds(5), peer), 0);

  messages = make_ping_queue();
  messages->raise(make_peer(5002), std::string {"PING"});
  EXPECT_EQ(stream::await_ping(messages, false, "payload", shutdown, std::chrono::milliseconds(300), peer), -1);
}

TEST(AwaitPing, OtherPayloadsDoNotExtendTheDeadline) {
  auto messages = make_ping_queue();
  safe::event_t<bool> shutdown;
  boost::asio::ip::udp::endpoint peer;

  std::atomic<bool> done {false};
  std::thread feeder([&]() {
    while (!done) {
      messages->raise(make_peer(5003), std::string {"noise"});
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  auto start = std::chrono::steady_clock::now();
  auto res = stream::await_ping(messages, false, "payload", shutdown, std::chrono::milliseconds(400), peer);
  auto elapsed = std::chrono::steady_clock::now() - start;
  done = true;
  feeder.join();

  EXPECT_EQ(res, -1);
  EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
}

TEST(SessionLifecycle, StopAndJoinOfUnstartedSession) {
  rtsp_stream::launch_session_t launch;
  launch.id = 1;
  launch.unique_id = "A";
  launch.gcm_key = crypto::aes_t(16);
  launch.iv = crypto::aes_t(16);
  launch.av_ping_payload = "0123456789abcdef";

  stream::config_t config {};
  auto session = stream::session::alloc(config, launch);
  ASSERT_TRUE(session);

  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);
  EXPECT_EQ(stream::session::launch_id(*session), 1u);

  EXPECT_NO_THROW(stream::session::stop(*session));
  EXPECT_NO_THROW(stream::session::join(*session));
  EXPECT_NO_THROW(stream::session::join(*session));
  EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPED);
}

TEST(FecParity, DisabledWithoutPercentageOrData) {
  EXPECT_EQ(stream::fec::plan_parity(10, 0, 5).parity_shards, 0u);
  EXPECT_EQ(stream::fec::plan_parity(0, 20, 2).parity_shards, 0u);
}

TEST(FecParity, ConfiguredPercentageIsKeptWhenMinimumIsMet) {
  auto plan = stream::fec::plan_parity(10, 20, 2);
  EXPECT_EQ(plan.parity_shards, 2u);
  EXPECT_EQ(plan.percentage, 20u);
}

TEST(FecParity, MinimumParityRaisesPercentage) {
  auto plan = stream::fec::plan_parity(1, 20, 2);
  EXPECT_EQ(plan.parity_shards, 2u);
  EXPECT_GT(plan.percentage, 100u);

  plan = stream::fec::plan_parity(3, 1, 2);
  EXPECT_EQ(plan.parity_shards, 2u);
}

TEST(FecParity, PercentageNeverExceedsHeaderField) {
  for (size_t min_parity : {2u, 16u, 254u}) {
    for (size_t data = 1; data <= 252; ++data) {
      auto plan = stream::fec::plan_parity(data, 1, min_parity);
      EXPECT_LE(plan.percentage, 255u) << data << '/' << min_parity;
      EXPECT_LE(data + plan.parity_shards, 255u) << data << '/' << min_parity;
    }
  }
}

TEST(FecParity, ClientRecomputesTheSameParityCount) {
  for (size_t min_parity : {0u, 2u, 16u}) {
    for (size_t data = 1; data <= 254; ++data) {
      auto plan = stream::fec::plan_parity(data, 20, min_parity);
      if (plan.parity_shards == 0) {
        continue;
      }
      EXPECT_EQ((data * plan.percentage + 99) / 100, plan.parity_shards) << data << '/' << min_parity;
    }
  }
}
