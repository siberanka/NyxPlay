/**
 * @file tests/unit/test_launch_slot.cpp
 * @brief Test the pending launch slot of src/rtsp.*
 */
#include <src/rtsp.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../tests_common.h"

namespace {
  std::shared_ptr<rtsp_stream::launch_session_t> make_launch(uint32_t id, const std::string &uuid) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->id = id;
    session->unique_id = uuid;
    return session;
  }
}  // namespace

using rtsp_stream::launch_slot_t;

TEST(LaunchSlot, RaiseIntoEmptySlot) {
  launch_slot_t slot;
  auto result = slot.raise(make_launch(1, "A"));
  EXPECT_EQ(result.result, launch_slot_t::result_e::raised);
  ASSERT_TRUE(slot.pending());
  EXPECT_EQ(slot.pending()->id, 1u);
}

TEST(LaunchSlot, ReplaceSameClient) {
  launch_slot_t slot;
  auto first = make_launch(1, "A");
  slot.raise(first);
  EXPECT_TRUE(launch_slot_t::claim_announce(*first));

  auto second = make_launch(2, "A");
  auto result = slot.raise(second);
  EXPECT_EQ(result.result, launch_slot_t::result_e::replaced);
  ASSERT_TRUE(result.replaced);
  EXPECT_EQ(result.replaced->id, 1u);
  EXPECT_EQ(slot.pending()->id, 2u);

  // The retry can announce again, the old launch is gone
  EXPECT_TRUE(launch_slot_t::claim_announce(*second));
  EXPECT_FALSE(slot.clear(1));
  EXPECT_EQ(slot.pending()->id, 2u);
}

TEST(LaunchSlot, OtherClientRejected) {
  launch_slot_t slot;
  slot.raise(make_launch(1, "A"));

  auto result = slot.raise(make_launch(2, "B"));
  EXPECT_EQ(result.result, launch_slot_t::result_e::busy);
  EXPECT_FALSE(result.replaced);
  EXPECT_EQ(slot.pending()->id, 1u);
  EXPECT_TRUE(slot.blocked_for("B"));
  EXPECT_FALSE(slot.blocked_for("A"));

  // Once the first launch connected the other client can launch
  EXPECT_TRUE(slot.clear(1));
  EXPECT_FALSE(slot.blocked_for("B"));
  EXPECT_EQ(slot.raise(make_launch(3, "B")).result, launch_slot_t::result_e::raised);
}

TEST(LaunchSlot, ClearIgnoresStaleIds) {
  launch_slot_t slot;
  slot.raise(make_launch(5, "A"));
  EXPECT_FALSE(slot.clear(4));
  ASSERT_TRUE(slot.pending());
  EXPECT_TRUE(slot.clear(5));
  EXPECT_FALSE(slot.pending());
  EXPECT_FALSE(slot.clear(5));
}

TEST(LaunchSlot, AnnounceClaim) {
  auto session = make_launch(1, "A");
  EXPECT_TRUE(launch_slot_t::claim_announce(*session));
  EXPECT_FALSE(launch_slot_t::claim_announce(*session));

  launch_slot_t::release_announce(*session);
  EXPECT_TRUE(launch_slot_t::claim_announce(*session));
}

TEST(LaunchSlot, AnnounceClaimHasOneWinner) {
  auto session = make_launch(1, "A");
  std::atomic<int> winners {0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&]() {
      if (launch_slot_t::claim_announce(*session)) {
        ++winners;
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(winners.load(), 1);
}

TEST(LaunchSlot, DropForUuid) {
  launch_slot_t slot;
  slot.raise(make_launch(1, "A"));

  EXPECT_FALSE(slot.drop("B"));
  ASSERT_TRUE(slot.pending());

  auto dropped = slot.drop("A");
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped->id, 1u);
  EXPECT_FALSE(slot.pending());
  EXPECT_FALSE(slot.drop("A"));
}

TEST(LaunchSlot, DropAny) {
  launch_slot_t slot;
  EXPECT_FALSE(slot.drop_any());
  slot.raise(make_launch(1, "A"));
  EXPECT_TRUE(slot.drop_any());
  EXPECT_FALSE(slot.pending());
}
