/**
 * @file tests/unit/test_confighttp_clients.cpp
 * @brief Test the validation of client update requests in src/confighttp.cpp.
 */
#include <string>

#include <nlohmann/json.hpp>

#include <src/confighttp.h>
#include <src/crypto.h>

#include "../tests_common.h"

namespace {
  crypto::named_cert_t stored_client() {
    crypto::named_cert_t client {};
    client.name = "Phone";
    client.uuid = "11111111-1111-1111-1111-111111111111";
    client.display_mode = "1920x1080x60";
    client.perm = crypto::PERM::_default;
    client.do_cmds.push_back({"stored-do", false});
    client.undo_cmds.push_back({"stored-undo", true});
    client.enable_legacy_ordering = false;
    client.allow_client_commands = false;
    client.always_use_virtual_display = true;
    return client;
  }

  std::string parse(const nlohmann::json &input, confighttp::client_update_t &update) {
    return confighttp::parse_client_update(input, stored_client(), update);
  }

  nlohmann::json request(const nlohmann::json &fields) {
    nlohmann::json input = {{"uuid", "11111111-1111-1111-1111-111111111111"}};
    input.update(fields);
    return input;
  }
}  // namespace

TEST(ClientUpdateTest, UuidIsRequired) {
  confighttp::client_update_t update;
  EXPECT_FALSE(parse(nlohmann::json::object(), update).empty());
  EXPECT_FALSE(parse({{"uuid", ""}}, update).empty());
  EXPECT_FALSE(parse({{"uuid", 5}}, update).empty());
  EXPECT_FALSE(parse(nlohmann::json::array(), update).empty());
}

TEST(ClientUpdateTest, MissingFieldsKeepTheStoredValues) {
  confighttp::client_update_t update;
  ASSERT_EQ(parse(request(nlohmann::json::object()), update), "");
  EXPECT_EQ(update.name, "Phone");
  EXPECT_EQ(update.display_mode, "1920x1080x60");
  EXPECT_EQ(update.perm, crypto::PERM::_default);
  ASSERT_EQ(update.do_cmds.size(), 1u);
  EXPECT_EQ(update.do_cmds.front().cmd, "stored-do");
  ASSERT_EQ(update.undo_cmds.size(), 1u);
  EXPECT_FALSE(update.enable_legacy_ordering);
  EXPECT_FALSE(update.allow_client_commands);
  EXPECT_TRUE(update.always_use_virtual_display);
}

TEST(ClientUpdateTest, GivenFieldsReplaceTheStoredValues) {
  confighttp::client_update_t update;
  ASSERT_EQ(parse(request({{"name", "Tablet"}, {"perm", static_cast<uint32_t>(crypto::PERM::_all)}, {"do", nlohmann::json::array()}, {"allow_client_commands", true}, {"display_mode", ""}}), update), "");
  EXPECT_EQ(update.name, "Tablet");
  EXPECT_EQ(update.perm, crypto::PERM::_all);
  EXPECT_TRUE(update.do_cmds.empty());
  EXPECT_EQ(update.undo_cmds.size(), 1u);
  EXPECT_TRUE(update.allow_client_commands);
  EXPECT_TRUE(update.display_mode.empty());
}

TEST(ClientUpdateTest, InvalidPermissionValuesAreRejected) {
  confighttp::client_update_t update;
  EXPECT_NE(parse(request({{"perm", -1}}), update), "");
  EXPECT_NE(parse(request({{"perm", 4294967295u}}), update), "");
  EXPECT_NE(parse(request({{"perm", 4294967296ull}}), update), "");
  EXPECT_NE(parse(request({{"perm", "abc"}}), update), "");
  EXPECT_NE(parse(request({{"perm", 1.5}}), update), "");
  EXPECT_NE(parse(request({{"perm", nullptr}}), update), "");
  // The reserved bit is not part of the known permissions
  EXPECT_NE(parse(request({{"perm", static_cast<uint32_t>(crypto::PERM::_all) | 1u}}), update), "");
  EXPECT_EQ(parse(request({{"perm", 0}}), update), "");
  EXPECT_EQ(update.perm, crypto::PERM::_no);
}

TEST(ClientUpdateTest, NameIsBounded) {
  confighttp::client_update_t update;
  EXPECT_EQ(parse(request({{"name", std::string(nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE, 'x')}}), update), "");
  EXPECT_NE(parse(request({{"name", std::string(nvhttp::MAX_PAIRING_CLIENT_NAME_SIZE + 1, 'x')}}), update), "");
  EXPECT_NE(parse(request({{"name", 5}}), update), "");
  // Parentheses are allowed in names assigned by the operator
  ASSERT_EQ(parse(request({{"name", "Living Room (TV)"}}), update), "");
  EXPECT_EQ(update.name, "Living Room (TV)");
}

TEST(ClientUpdateTest, DisplayModeMustBeWellFormed) {
  confighttp::client_update_t update;
  EXPECT_EQ(parse(request({{"display_mode", ""}}), update), "");
  EXPECT_EQ(parse(request({{"display_mode", "1920x1080"}}), update), "");
  EXPECT_EQ(parse(request({{"display_mode", "1920x1080x59.94"}}), update), "");
  EXPECT_EQ(parse(request({{"display_mode", "1920x1080x0"}}), update), "");
  EXPECT_NE(parse(request({{"display_mode", "99999999x1x1"}}), update), "");
  EXPECT_NE(parse(request({{"display_mode", "abc"}}), update), "");
  EXPECT_NE(parse(request({{"display_mode", "1920x"}}), update), "");
  EXPECT_NE(parse(request({{"display_mode", 5}}), update), "");
}

TEST(ClientUpdateTest, FlagsAndCommandListsHaveToHaveTheRightType) {
  confighttp::client_update_t update;
  EXPECT_NE(parse(request({{"allow_client_commands", "yes"}}), update), "");
  EXPECT_NE(parse(request({{"do", "cmd"}}), update), "");
  EXPECT_NE(parse(request({{"undo", 5}}), update), "");
}
