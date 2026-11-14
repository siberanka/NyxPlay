/**
 * @file tests/unit/test_client_registry.cpp
 * @brief Test the paired client registry and its state file in src/nvhttp.*.
 */
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <src/config.h>
#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>

#include "../tests_common.h"

namespace fs = std::filesystem;

namespace {
  const std::string &test_cert(const std::size_t index) {
    static const std::vector<std::string> certs = [] {
      std::vector<std::string> result;
      for (int i = 0; i < 3; ++i) {
        result.push_back(crypto::gen_creds("Registry Client " + std::to_string(i), 2048).x509);
      }
      return result;
    }();
    return certs.at(index);
  }

  crypto::p_named_cert_t make_client(const std::string &name, const std::string &uuid, const std::size_t cert_index, const crypto::PERM perm = crypto::PERM::_default) {
    auto named = std::make_shared<crypto::named_cert_t>();
    named->name = name;
    named->uuid = uuid;
    named->cert = test_cert(cert_index);
    named->perm = perm;
    named->enable_legacy_ordering = true;
    named->allow_client_commands = false;
    named->always_use_virtual_display = false;
    return named;
  }

  void write_file(const fs::path &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  nlohmann::json state_entry(const std::size_t cert_index, const std::string &uuid) {
    return {{"name", "n" + uuid}, {"cert", test_cert(cert_index)}, {"uuid", uuid}};
  }

  const std::string uuid_a = "11111111-1111-1111-1111-111111111111";
  const std::string uuid_b = "22222222-2222-2222-2222-222222222222";
  const std::string uuid_c = "33333333-3333-3333-3333-333333333333";
}  // namespace

class ClientRegistryTest: public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> counter {0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    scratch = fs::temp_directory_path() / ("nyx-registry-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    fs::create_directories(scratch);

    saved_state_file = config::nvhttp.file_state;
    saved_unique_id = http::unique_id;
    config::nvhttp.file_state = (scratch / "state.json").string();
    nvhttp::test_access::reset();
    http::unique_id = "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f";
  }

  void TearDown() override {
    nvhttp::test_access::reset();
    config::nvhttp.file_state = saved_state_file;
    http::unique_id = saved_unique_id;
    std::error_code ec;
    fs::remove_all(scratch, ec);
  }

  /// Make writing the state file fail: its temporary file name is taken by a directory.
  void block_state_writes() {
    fs::create_directories(config::nvhttp.file_state + ".tmp");
  }

  void unblock_state_writes() {
    std::error_code ec;
    fs::remove_all(config::nvhttp.file_state + ".tmp", ec);
  }

  /// Number of files in the scratch directory whose name contains the given text.
  int count_files_containing(const std::string &text) const {
    int count = 0;
    for (const auto &entry : fs::directory_iterator(scratch)) {
      if (entry.path().filename().string().find(text) != std::string::npos) {
        ++count;
      }
    }
    return count;
  }

  /// Bind a connection to the stored record of a client.
  static std::shared_ptr<void> bind(const std::string &uuid) {
    return std::const_pointer_cast<crypto::named_cert_t>(nvhttp::get_client(uuid));
  }

  fs::path scratch;
  std::string saved_state_file;
  std::string saved_unique_id;
};

TEST_F(ClientRegistryTest, SaveThenLoadRoundTrip) {
  auto first = make_client("Phone", uuid_a, 0, crypto::PERM::_all);
  first->display_mode = "1920x1080x60";
  first->do_cmds.push_back({"do-cmd", true});
  first->undo_cmds.push_back({"undo-cmd", false});
  first->always_use_virtual_display = true;
  ASSERT_TRUE(nvhttp::test_access::add_client(first));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Tablet", uuid_b, 1)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("TV", uuid_c, 2)));

  nvhttp::test_access::reset();
  ASSERT_EQ(nvhttp::test_access::client_count(), 0u);
  http::unique_id = "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f";
  nvhttp::test_access::load();

  ASSERT_EQ(nvhttp::test_access::client_count(), 3u);
  auto loaded = nvhttp::get_client(uuid_a);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->name, "Phone");
  EXPECT_EQ(loaded->display_mode, "1920x1080x60");
  EXPECT_EQ(loaded->perm, crypto::PERM::_all);
  EXPECT_TRUE(loaded->always_use_virtual_display);
  ASSERT_EQ(loaded->do_cmds.size(), 1u);
  EXPECT_EQ(loaded->do_cmds.front().cmd, "do-cmd");
  EXPECT_TRUE(loaded->do_cmds.front().elevated);
  ASSERT_EQ(loaded->undo_cmds.size(), 1u);
  EXPECT_EQ(nvhttp::get_client(uuid_b)->perm, crypto::PERM::_default);
  EXPECT_EQ(http::unique_id, "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f");
}

TEST_F(ClientRegistryTest, NamesWithParenthesesSurviveRoundTrip) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Living Room (TV)", uuid_a, 0)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Living Room (TV)", uuid_b, 1)));

  nvhttp::test_access::load();
  EXPECT_EQ(nvhttp::get_client(uuid_a)->name, "Living Room (TV)");
  EXPECT_EQ(nvhttp::get_client(uuid_b)->name, "Living Room (TV)");
}

TEST_F(ClientRegistryTest, SaveLeavesNoTemporaryFile) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  ASSERT_TRUE(nvhttp::test_access::save());
  EXPECT_TRUE(fs::exists(config::nvhttp.file_state));
  EXPECT_FALSE(fs::exists(config::nvhttp.file_state + ".tmp"));
}

TEST_F(ClientRegistryTest, FailedSaveKeepsPreviousFileAndReportsFailure) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  const auto before = read_file(config::nvhttp.file_state);

  block_state_writes();
  EXPECT_FALSE(nvhttp::test_access::save());
  EXPECT_EQ(read_file(config::nvhttp.file_state), before);
}

TEST_F(ClientRegistryTest, PairingFailsWhenStateCannotBePersisted) {
  block_state_writes();
  EXPECT_FALSE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  EXPECT_EQ(nvhttp::test_access::client_count(), 0u);
  EXPECT_EQ(nvhttp::get_client(uuid_a), nullptr);

  unblock_state_writes();
  EXPECT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  EXPECT_EQ(nvhttp::test_access::client_count(), 1u);
}

TEST_F(ClientRegistryTest, LoadSkipsMalformedEntries) {
  nlohmann::json valid = state_entry(0, uuid_a);
  nlohmann::json state = {
    {"root", {{"uniqueid", "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f"}, {"named_devices", nlohmann::json::array({1, nullptr, "text", nlohmann::json {{"cert", 5}}, nlohmann::json {{"cert", ""}}, nlohmann::json {{"cert", "not a certificate"}}, nlohmann::json {{"name", 7}, {"cert", test_cert(1)}}, valid})}}},
  };
  write_file(config::nvhttp.file_state, state.dump());

  EXPECT_NO_THROW(nvhttp::test_access::load());
  ASSERT_EQ(nvhttp::test_access::client_count(), 1u);
  EXPECT_NE(nvhttp::get_client(uuid_a), nullptr);
}

TEST_F(ClientRegistryTest, LoadWithNonStringUniqueIdGeneratesOne) {
  http::unique_id.clear();
  write_file(config::nvhttp.file_state, nlohmann::json({{"root", {{"uniqueid", 5}, {"named_devices", nlohmann::json::array()}}}}).dump());

  EXPECT_NO_THROW(nvhttp::test_access::load());
  EXPECT_EQ(http::unique_id.size(), 36u);
  EXPECT_EQ(http::unique_id[8], '-');
}

TEST_F(ClientRegistryTest, LoadWithBadUniqueIdGeneratesOne) {
  http::unique_id.clear();
  write_file(config::nvhttp.file_state, nlohmann::json({{"root", {{"uniqueid", "x"}}}}).dump());

  nvhttp::test_access::load();
  EXPECT_EQ(http::unique_id.size(), 36u);
}

TEST_F(ClientRegistryTest, GarbageStateFileIsMovedAsideAndHostContinues) {
  http::unique_id.clear();
  write_file(config::nvhttp.file_state, "this is { not json");

  EXPECT_NO_THROW(nvhttp::test_access::load());
  EXPECT_EQ(http::unique_id.size(), 36u);
  EXPECT_FALSE(fs::exists(config::nvhttp.file_state));
  EXPECT_EQ(count_files_containing(".corrupt-"), 1);

  // New pairings can be stored again
  EXPECT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  EXPECT_TRUE(fs::exists(config::nvhttp.file_state));
}

TEST_F(ClientRegistryTest, StoredPermissionsAreValidatedOnLoad) {
  auto entry = [&](const std::string &uuid, const nlohmann::json &perm) {
    auto node = state_entry(0, uuid);
    node["perm"] = perm;
    return node;
  };
  nlohmann::json state = {
    {"root", {{"uniqueid", "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f"}, {"named_devices", nlohmann::json::array({entry(uuid_a, -1), entry(uuid_b, "abc"), entry(uuid_c, 4294967295u), entry("44444444-4444-4444-4444-444444444444", 1.5), entry("55555555-5555-5555-5555-555555555555", static_cast<uint32_t>(crypto::PERM::_all_actions))})}}},
  };
  write_file(config::nvhttp.file_state, state.dump());
  nvhttp::test_access::load();

  ASSERT_EQ(nvhttp::test_access::client_count(), 5u);
  EXPECT_EQ(nvhttp::get_client(uuid_a)->perm, crypto::PERM::_default);
  EXPECT_EQ(nvhttp::get_client(uuid_b)->perm, crypto::PERM::_default);
  EXPECT_EQ(nvhttp::get_client(uuid_c)->perm, crypto::PERM::_all);
  EXPECT_EQ(nvhttp::get_client("44444444-4444-4444-4444-444444444444")->perm, crypto::PERM::_default);
  EXPECT_EQ(nvhttp::get_client("55555555-5555-5555-5555-555555555555")->perm, crypto::PERM::_all_actions);
}

TEST_F(ClientRegistryTest, GetVerifiedCertReturnsRevokedRecordAfterUnpair) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0, crypto::PERM::_all)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Tablet", uuid_b, 1)));
  auto userp = bind(uuid_a);
  ASSERT_NE(userp, nullptr);
  EXPECT_EQ(nvhttp::resolve_verified_cert(userp)->perm, crypto::PERM::_all);

  EXPECT_TRUE(nvhttp::unpair_client(uuid_a));
  auto *revoked = nvhttp::resolve_verified_cert(userp);
  EXPECT_EQ(revoked->perm, crypto::PERM::_no);
  // The shared revoked record is never modified or replaced
  EXPECT_EQ(nvhttp::resolve_verified_cert(userp), revoked);
  EXPECT_EQ(nvhttp::get_client(uuid_a), nullptr);
}

TEST_F(ClientRegistryTest, GetVerifiedCertWithoutBoundRecordHasNoPermissions) {
  std::shared_ptr<void> none;
  EXPECT_EQ(nvhttp::resolve_verified_cert(none)->perm, crypto::PERM::_no);
}

TEST_F(ClientRegistryTest, GetVerifiedCertRebindsAfterReload) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  auto userp = bind(uuid_a);
  auto *old_record = static_cast<crypto::named_cert_t *>(userp.get());

  // Pairing another client reloads the state, which replaces every record
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Tablet", uuid_b, 1)));
  auto *resolved = nvhttp::resolve_verified_cert(userp);
  EXPECT_EQ(resolved->uuid, uuid_a);
  EXPECT_NE(resolved, old_record);
  EXPECT_EQ(userp.get(), resolved);
}

TEST_F(ClientRegistryTest, GetVerifiedCertMatchesUuidAndCert) {
  // Clients without an identifier must not be mixed up
  nlohmann::json state = {
    {"root", {{"uniqueid", "0f0f0f0f-0f0f-0f0f-0f0f-0f0f0f0f0f0f"}, {"named_devices", nlohmann::json::array({state_entry(0, ""), state_entry(1, "")})}}},
  };
  write_file(config::nvhttp.file_state, state.dump());
  nvhttp::test_access::load();
  ASSERT_EQ(nvhttp::test_access::client_count(), 2u);

  auto userp = bind("");  // the first stored record
  ASSERT_NE(userp, nullptr);
  const auto first_cert = static_cast<crypto::named_cert_t *>(userp.get())->cert;

  state["root"]["named_devices"] = nlohmann::json::array({state_entry(first_cert == test_cert(0) ? 1 : 0, "")});
  write_file(config::nvhttp.file_state, state.dump());
  nvhttp::test_access::load();

  EXPECT_EQ(nvhttp::resolve_verified_cert(userp)->perm, crypto::PERM::_no);
}

TEST_F(ClientRegistryTest, UpdateDeviceInfoReplacesTheRecord) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  auto old_record = nvhttp::get_client(uuid_a);
  auto userp = bind(uuid_a);

  ASSERT_TRUE(nvhttp::update_device_info(uuid_a, "Renamed", "1280x720x60", {{"do", false}}, {{"undo", true}}, crypto::PERM::_all, false, true, true));

  // A record that other threads may still read is left untouched
  EXPECT_EQ(old_record->name, "Phone");
  EXPECT_EQ(old_record->perm, crypto::PERM::_default);
  EXPECT_TRUE(old_record->do_cmds.empty());
  EXPECT_TRUE(old_record->display_mode.empty());

  auto current = nvhttp::get_client(uuid_a);
  ASSERT_NE(current, nullptr);
  EXPECT_NE(current, old_record);
  EXPECT_EQ(current->name, "Renamed");
  EXPECT_EQ(current->perm, crypto::PERM::_all);
  EXPECT_EQ(current->display_mode, "1280x720x60");
  EXPECT_EQ(current->do_cmds.size(), 1u);
  EXPECT_EQ(current->cert, old_record->cert);

  // The connection bound to the old record sees the new values
  EXPECT_EQ(nvhttp::resolve_verified_cert(userp)->name, "Renamed");

  // And the change is persisted
  nvhttp::test_access::load();
  EXPECT_EQ(nvhttp::get_client(uuid_a)->name, "Renamed");
}

TEST_F(ClientRegistryTest, UpdateDeviceInfoWithUnknownUuidChangesNothing) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  const auto before = read_file(config::nvhttp.file_state);

  EXPECT_FALSE(nvhttp::update_device_info(uuid_b, "x", "", {}, {}, crypto::PERM::_all, true, true, false));
  EXPECT_EQ(nvhttp::get_client(uuid_a)->name, "Phone");
  EXPECT_EQ(read_file(config::nvhttp.file_state), before);
}

TEST_F(ClientRegistryTest, UpdateDeviceInfoIsRolledBackWhenSavingFails) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  auto old_record = nvhttp::get_client(uuid_a);

  block_state_writes();
  EXPECT_FALSE(nvhttp::update_device_info(uuid_a, "Renamed", "", {}, {}, crypto::PERM::_all, true, true, false));
  EXPECT_EQ(nvhttp::get_client(uuid_a), old_record);
}

TEST_F(ClientRegistryTest, UnpairRemovesEveryRecordOfTheUuid) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone copy", uuid_a, 1)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Tablet", uuid_b, 2)));
  ASSERT_EQ(nvhttp::test_access::client_count(), 3u);

  EXPECT_TRUE(nvhttp::unpair_client(uuid_a));
  EXPECT_EQ(nvhttp::test_access::client_count(), 1u);
  EXPECT_FALSE(nvhttp::unpair_client(uuid_a));
  EXPECT_NE(nvhttp::get_client(uuid_b), nullptr);

  nvhttp::test_access::load();
  EXPECT_EQ(nvhttp::test_access::client_count(), 1u);
}

TEST_F(ClientRegistryTest, EraseAllClientsClearsMemoryAndFile) {
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Phone", uuid_a, 0)));
  ASSERT_TRUE(nvhttp::test_access::add_client(make_client("Tablet", uuid_b, 1)));
  auto userp = bind(uuid_a);

  nvhttp::erase_all_clients();
  EXPECT_EQ(nvhttp::test_access::client_count(), 0u);
  EXPECT_EQ(nvhttp::resolve_verified_cert(userp)->perm, crypto::PERM::_no);

  nvhttp::test_access::load();
  EXPECT_EQ(nvhttp::test_access::client_count(), 0u);
}

TEST(LaunchSessionTest, ShortRikeyIsRejectedWithAClearReason) {
  crypto::named_cert_t client {};
  client.uuid = "66666666-6666-6666-6666-666666666666";
  client.perm = crypto::PERM::_all;

  nvhttp::args_t args;
  args.emplace("rikey", "abcd");
  args.emplace("rikeyid", "1");
  try {
    nvhttp::make_launch_session(false, false, args, &client);
    FAIL() << "a short rikey must be rejected";
  } catch (const std::invalid_argument &e) {
    EXPECT_STREQ(e.what(), "Invalid rikey");
  }
}
