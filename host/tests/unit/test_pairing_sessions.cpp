/**
 * @file tests/unit/test_pairing_sessions.cpp
 * @brief Test the bounded pairing session store in src/nvhttp.*.
 */
#include <chrono>
#include <set>
#include <string>

#include <src/nvhttp.h>

#include "../tests_common.h"

namespace {
  using namespace std::chrono_literals;

  nvhttp::pair_session_t make_session(const std::string &unique_id, const std::string &address = "192.0.2.1") {
    nvhttp::pair_session_t sess;
    sess.client.uniqueID = unique_id;
    sess.client.name = "client-" + unique_id;
    sess.async_insert_pin.address = address;
    sess.async_insert_pin.salt = std::string(32, 'a');
    return sess;
  }

  bool is_hex32(const std::string &id) {
    return id.size() == 32 && nvhttp::is_valid_pairing_id(id);
  }

  const std::string zero_id(nvhttp::PAIRING_ID_SIZE, '0');
}  // namespace

class PairingSessionTest: public ::testing::Test {
protected:
  // The session store is global, so every test starts and ends with it empty.
  void SetUp() override {
    nvhttp::expire_pair_sessions(std::chrono::steady_clock::time_point::max());
  }

  void TearDown() override {
    nvhttp::expire_pair_sessions(std::chrono::steady_clock::time_point::max());
  }
};

TEST(PairingValidatorTest, PairingIdMustBeExactly32HexCharacters) {
  EXPECT_TRUE(nvhttp::is_valid_pairing_id(zero_id));
  EXPECT_TRUE(nvhttp::is_valid_pairing_id("0123456789abcdefABCDEF0123456789"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(""));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(std::string(nvhttp::PAIRING_ID_SIZE - 1, 'a')));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(std::string(nvhttp::PAIRING_ID_SIZE + 1, 'a')));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(std::string(nvhttp::PAIRING_ID_SIZE - 1, 'a') + "g"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(std::string(nvhttp::PAIRING_ID_SIZE - 1, 'a') + " "));
  EXPECT_FALSE(nvhttp::is_valid_pairing_id(std::string(nvhttp::PAIRING_ID_SIZE - 1, 'a') + std::string(1, '\0')));
}

TEST(PairingValidatorTest, PairingPinMustBeFourDigits) {
  EXPECT_TRUE(nvhttp::is_valid_pairing_pin("0000"));
  EXPECT_TRUE(nvhttp::is_valid_pairing_pin("1234"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin(""));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin("123"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin("12345"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin("12a4"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin("-123"));
  EXPECT_FALSE(nvhttp::is_valid_pairing_pin(" 123"));
}

TEST(PairingValidatorTest, PairingNameIsLimitedButMayBeEmpty) {
  EXPECT_TRUE(nvhttp::is_valid_pairing_name(""));
  EXPECT_TRUE(nvhttp::is_valid_pairing_name("laptop"));
  EXPECT_TRUE(nvhttp::is_valid_pairing_name(std::string(nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE, 'x')));
  EXPECT_FALSE(nvhttp::is_valid_pairing_name(std::string(nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE + 1, 'x')));
}

TEST_F(PairingSessionTest, InsertReturnsUniqueHexIds) {
  std::set<std::string> ids;
  for (int i = 0; i < 8; ++i) {
    std::string id;
    ASSERT_EQ(nvhttp::insert_pair_session(make_session("client" + std::to_string(i), "192.0.2." + std::to_string(i + 1)), id), nvhttp::pair_session_insert_e::ADDED);
    EXPECT_TRUE(is_hex32(id)) << id;
    ids.insert(id);
  }
  EXPECT_EQ(ids.size(), 8u);
}

TEST_F(PairingSessionTest, SameUniqueIdFromAnotherAddressKeepsSeparateSession) {
  std::string first_id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("dup", "192.0.2.1"), first_id), nvhttp::pair_session_insert_e::ADDED);

  std::string second_id;
  EXPECT_EQ(nvhttp::insert_pair_session(make_session("dup", "192.0.2.2"), second_id), nvhttp::pair_session_insert_e::ADDED);
  EXPECT_NE(first_id, second_id);

  // The original session is untouched
  EXPECT_TRUE(nvhttp::cancel_pairing(first_id));
  EXPECT_TRUE(nvhttp::cancel_pairing(second_id));
}

TEST_F(PairingSessionTest, SingleAddressCannotFillTheStore) {
  std::string id;
  for (std::size_t i = 0; i < nvhttp::MAX_PENDING_PAIRING_SESSIONS_PER_ADDRESS; ++i) {
    ASSERT_EQ(nvhttp::insert_pair_session(make_session("a" + std::to_string(i)), id), nvhttp::pair_session_insert_e::ADDED) << i;
  }

  EXPECT_EQ(nvhttp::insert_pair_session(make_session("extra"), id), nvhttp::pair_session_insert_e::FULL);
  EXPECT_TRUE(id.empty());
  EXPECT_EQ(nvhttp::insert_pair_session(make_session("other", "192.0.2.99"), id), nvhttp::pair_session_insert_e::ADDED);
}

TEST_F(PairingSessionTest, Ipv6AddressesOfOnePrefixShareTheCap) {
  std::string id;
  for (std::size_t i = 0; i < nvhttp::MAX_PENDING_PAIRING_SESSIONS_PER_ADDRESS; ++i) {
    ASSERT_EQ(nvhttp::insert_pair_session(make_session("v6a" + std::to_string(i), "2001:db8:1:2::" + std::to_string(i + 1)), id), nvhttp::pair_session_insert_e::ADDED) << i;
  }

  // A fresh address of the same /64 is counted against the same group
  EXPECT_EQ(nvhttp::insert_pair_session(make_session("v6extra", "2001:db8:1:2:aaaa::9"), id), nvhttp::pair_session_insert_e::FULL);
  // Another prefix is independent
  EXPECT_EQ(nvhttp::insert_pair_session(make_session("v6other", "2001:db8:1:3::1"), id), nvhttp::pair_session_insert_e::ADDED);
}

TEST_F(PairingSessionTest, SameUniqueIdFromSameAddressReplacesOlderSession) {
  std::string old_id;
  std::string new_id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("retry"), old_id), nvhttp::pair_session_insert_e::ADDED);
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("retry"), new_id), nvhttp::pair_session_insert_e::ADDED);
  EXPECT_NE(old_id, new_id);

  EXPECT_FALSE(nvhttp::cancel_pairing(old_id));
  EXPECT_TRUE(nvhttp::cancel_pairing(new_id));
}

TEST_F(PairingSessionTest, StoreIsBounded) {
  std::string id;
  std::string first_id;
  for (std::size_t i = 0; i < nvhttp::MAX_PENDING_PAIRING_SESSIONS; ++i) {
    ASSERT_EQ(nvhttp::insert_pair_session(make_session("c" + std::to_string(i), "198.51.100." + std::to_string(i / nvhttp::MAX_PENDING_PAIRING_SESSIONS_PER_ADDRESS)), id), nvhttp::pair_session_insert_e::ADDED) << i;
    if (i == 0) {
      first_id = id;
    }
  }

  EXPECT_EQ(nvhttp::insert_pair_session(make_session("overflow", "203.0.113.1"), id), nvhttp::pair_session_insert_e::FULL);
  EXPECT_TRUE(id.empty());

  // Freeing one slot makes room again
  EXPECT_TRUE(nvhttp::cancel_pairing(first_id));
  EXPECT_EQ(nvhttp::insert_pair_session(make_session("overflow", "203.0.113.1"), id), nvhttp::pair_session_insert_e::ADDED);
}

TEST_F(PairingSessionTest, ExpireOnlyRemovesSessionsPastTheirDeadline) {
  std::string id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("exp"), id), nvhttp::pair_session_insert_e::ADDED);

  // A point in the past keeps the session
  nvhttp::expire_pair_sessions(std::chrono::steady_clock::now() - 1h);
  EXPECT_TRUE(nvhttp::cancel_pairing(id));

  // The far future removes it
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("exp"), id), nvhttp::pair_session_insert_e::ADDED);
  nvhttp::expire_pair_sessions(std::chrono::steady_clock::time_point::max());
  EXPECT_FALSE(nvhttp::cancel_pairing(id));
}

TEST_F(PairingSessionTest, CancelRequiresKnownValidId) {
  EXPECT_FALSE(nvhttp::cancel_pairing(""));
  EXPECT_FALSE(nvhttp::cancel_pairing("not-a-pairing-id"));
  EXPECT_FALSE(nvhttp::cancel_pairing(zero_id));

  std::string id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("cancel"), id), nvhttp::pair_session_insert_e::ADDED);
  EXPECT_FALSE(nvhttp::cancel_pairing(zero_id));
  EXPECT_TRUE(nvhttp::cancel_pairing(id));
  EXPECT_FALSE(nvhttp::cancel_pairing(id));
}

TEST_F(PairingSessionTest, PinRejectsInvalidArguments) {
  std::string id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("pin-invalid"), id), nvhttp::pair_session_insert_e::ADDED);

  EXPECT_FALSE(nvhttp::pin("bad-id", "1234", ""));
  EXPECT_FALSE(nvhttp::pin(id, "12", ""));
  EXPECT_FALSE(nvhttp::pin(id, "abcd", ""));
  EXPECT_FALSE(nvhttp::pin(id, "1234", std::string(nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE + 1, 'x')));
  EXPECT_FALSE(nvhttp::pin(zero_id, "1234", ""));

  // Invalid arguments must not consume the session
  EXPECT_TRUE(nvhttp::cancel_pairing(id));
}

TEST_F(PairingSessionTest, PinWithoutWaitingResponseFailsAndRemovesSession) {
  std::string id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("pin-no-response"), id), nvhttp::pair_session_insert_e::ADDED);

  // No HTTP response is parked in the session, so there is nobody to answer
  EXPECT_FALSE(nvhttp::pin(id, "1234", "laptop"));
  EXPECT_FALSE(nvhttp::cancel_pairing(id));
}

TEST_F(PairingSessionTest, PendingListOnlyContainsSessionsWaitingForTheWebUi) {
  // Only requests parked for the Web UI PIN (those holding an HTTP response) are listed.
  // Sessions without a waiting response, e.g. already approved by an OTP, are hidden.
  std::string id;
  ASSERT_EQ(nvhttp::insert_pair_session(make_session("hidden"), id), nvhttp::pair_session_insert_e::ADDED);
  EXPECT_TRUE(nvhttp::get_pending_pairings().empty());
}

TEST_F(PairingSessionTest, DeliveringThePinExtendsTheSessionDeadline) {
  auto sess = make_session("late-pin");
  const auto now = std::chrono::steady_clock::now();

  // Close to expiry: the remaining phases need more time than is left
  sess.async_insert_pin.expires_at = now + 1s;
  nvhttp::extend_pair_session_deadline(sess, now);
  EXPECT_GE(sess.async_insert_pin.expires_at, now + 60s);

  // A deadline that is already further away is not shortened
  sess.async_insert_pin.expires_at = now + 1h;
  nvhttp::extend_pair_session_deadline(sess, now);
  EXPECT_EQ(sess.async_insert_pin.expires_at, now + 1h);
}
