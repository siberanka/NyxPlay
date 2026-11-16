/**
 * @file tests/unit/test_config_import.cpp
 * @brief Test src/config_import.* (migration from Apollo / Sunshine).
 */
#include <src/config_import.h>
#include <src/crypto.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

#include "../tests_common.h"

namespace fs = std::filesystem;
using namespace config_import;

namespace {
  std::string to_u8(const fs::path &p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
  }

  fs::path from_u8(const std::string &s) {
    return fs::path(std::u8string(s.begin(), s.end()));
  }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  nlohmann::json read_json(const fs::path &path) {
    return nlohmann::json::parse(read(path));
  }

  bool has_warning(const result_t &r, const std::string &needle) {
    for (auto &w : r.warnings) {
      if (w.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  const std::string key_bytes = std::string("PRIVATE\0KEY\r\n\xFF\x01", 17);
  const std::string cert_bytes = "CERTIFICATE\r\nDATA";
}  // namespace

class ConfigImportTest: public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> counter {0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    scratch = fs::temp_directory_path() / ("nyx-config-import-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    fs::create_directories(scratch);
    install = scratch / "Apollo";
    src = install / "config";
    dst = scratch / "nyx" / "config";
    fs::create_directories(src);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(scratch, ec);
  }

  /// A source host with settings, apps, state and identity.
  void populate_source(bool with_identity = true) {
    write(src / "sunshine.conf",
          "# comment\n"
          "sunshine_name = Gaming PC  # trailing comment\n"
          "port = 47989\n"
          "external_ip = 203.0.113.7\n"
          "file_apps = apps.json\n"
          "file_state = sunshine_state.json\n"
          "credentials_file = sunshine_state.json\n"
          "pkey = credentials/cakey.pem\n"
          "cert = credentials/cacert.pem\n"
          "log_path = sunshine.log\n"
          "global_prep_cmd = [\n"
          "  {\"do\": \"a.bat\", \"undo\": \"\"},\n"
          "  {\"do\": \"b.bat\", \"undo\": \"c.bat\"}\n"
          "]\n"
          "server_cmd = " + to_u8(install) + "/tools/run.exe\n");

    auto cover = to_u8(src / "covers" / "game.png");
    nlohmann::json apps = {
      {"env", {{"PATH", "x"}}},
      {"apps",
       nlohmann::json::array({
         {{"name", "Desktop"}, {"image-path", to_u8(install / "assets" / "desktop.png")}, {"custom", 1}},
         {{"name", "Game"}, {"image-path", cover}, {"cmd", "game.exe"}},
         {{"name", "Other"}, {"image-path", "D:\\somewhere\\else.png"}},
       })},
    };
    write(src / "apps.json", apps.dump(4));
    write(src / "covers" / "game.png", "PNG");
    write(src / "covers" / "unused.png", "PNG2");

    nlohmann::json state = {
      {"username", "admin"},
      {"salt", "SALT"},
      {"password", "HASH"},
      {"root", {{"uniqueid", "0123456789ABCDEF"}, {"named_devices", nlohmann::json::array({{{"name", "Phone"}, {"cert", "CERT1"}, {"uuid", "u1"}}})}}},
    };
    write(src / "sunshine_state.json", state.dump(4));

    if (with_identity) {
      write(src / "credentials" / "cakey.pem", key_bytes);
      write(src / "credentials" / "cacert.pem", cert_bytes);
    }
  }

  fs::path scratch, install, src, dst;
};

TEST_F(ConfigImportTest, SettingsFilterPathKeysAndKeepMultilineValues) {
  populate_source();
  options_t o;
  o.apps = o.clients = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;

  auto conf = read(dst / "sunshine.conf");
  EXPECT_NE(conf.find("sunshine_name = Gaming PC\n"), std::string::npos);
  EXPECT_NE(conf.find("external_ip = 203.0.113.7"), std::string::npos);
  EXPECT_NE(conf.find("port = 47989"), std::string::npos);
  for (auto key : {"file_apps", "file_state", "credentials_file", "pkey", "cert", "log_path"}) {
    EXPECT_EQ(conf.find(std::string(key) + " ="), std::string::npos) << key;
  }
  EXPECT_EQ(conf.find("comment"), std::string::npos);
  EXPECT_NE(conf.find("global_prep_cmd = [\n  {\"do\": \"a.bat\", \"undo\": \"\"},\n  {\"do\": \"b.bat\", \"undo\": \"c.bat\"}\n]\n"), std::string::npos) << conf;
  EXPECT_TRUE(has_warning(r, "port"));
  EXPECT_TRUE(has_warning(r, "server_cmd"));
  EXPECT_FALSE(fs::exists(dst / "apps.json"));
  EXPECT_TRUE(r.backup_dir.empty());
}

TEST_F(ConfigImportTest, UnknownSettingsKeysAreReported) {
  populate_source();
  write(src / "sunshine.conf", read(src / "sunshine.conf") + "unknown_future_key = 1\n");
  options_t o;
  o.apps = o.clients = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_TRUE(has_warning(r, "\"unknown_future_key\" is not a known setting"));
  EXPECT_FALSE(has_warning(r, "\"sunshine_name\" is not a known setting"));
  EXPECT_FALSE(has_warning(r, "\"external_ip\" is not a known setting"));
  EXPECT_FALSE(has_warning(r, "\"global_prep_cmd\" is not a known setting"));
}

TEST_F(ConfigImportTest, DropPortWhenRequested) {
  populate_source();
  options_t o;
  o.apps = o.clients = false;
  o.keep_port = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(read(dst / "sunshine.conf").find("port ="), std::string::npos);
  EXPECT_FALSE(has_warning(r, "port"));
}

TEST_F(ConfigImportTest, AppsRewriteImagePathsAndCopyCovers) {
  populate_source();
  options_t o;
  o.settings = o.clients = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;

  auto apps = read_json(dst / "apps.json");
  ASSERT_EQ(apps["apps"].size(), 3u);
  EXPECT_EQ(apps["apps"][0]["image-path"], "desktop.png");
  EXPECT_EQ(apps["apps"][0]["custom"], 1);
  EXPECT_EQ(apps["env"]["PATH"], "x");
  EXPECT_TRUE(fs::equivalent(from_u8(apps["apps"][1]["image-path"].get<std::string>()), dst / "covers" / "game.png"));
  EXPECT_EQ(apps["apps"][1]["cmd"], "game.exe");
  EXPECT_EQ(apps["apps"][2]["image-path"], "D:\\somewhere\\else.png");
  EXPECT_EQ(read(dst / "covers" / "game.png"), "PNG");
  EXPECT_EQ(read(dst / "covers" / "unused.png"), "PNG2");
  EXPECT_FALSE(fs::exists(dst / "sunshine.conf"));
}

TEST_F(ConfigImportTest, ClientsMergeKeepsTargetKeysAndCopiesIdentity) {
  populate_source();
  write(dst / "sunshine_state.json", nlohmann::json({{"extra", "keep"}, {"root", {{"uniqueid", "OLD"}, {"other", 5}}}}).dump(4));
  options_t o;
  o.settings = o.apps = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;

  auto state = read_json(dst / "sunshine_state.json");
  EXPECT_EQ(state["extra"], "keep");
  EXPECT_EQ(state["root"]["other"], 5);
  EXPECT_EQ(state["root"]["uniqueid"], "0123456789ABCDEF");
  ASSERT_EQ(state["root"]["named_devices"].size(), 1u);
  EXPECT_EQ(state["root"]["named_devices"][0]["cert"], "CERT1");
  EXPECT_FALSE(state.contains("password"));  // web credentials are opt-in
  EXPECT_EQ(read(dst / "credentials" / "cakey.pem"), key_bytes);
  EXPECT_EQ(read(dst / "credentials" / "cacert.pem"), cert_bytes);
  EXPECT_FALSE(r.backup_dir.empty());
  EXPECT_TRUE(fs::exists(r.backup_dir / "sunshine_state.json"));
}

TEST_F(ConfigImportTest, LegacyDevicesLayoutIsCarriedOver) {
  populate_source();
  write(src / "sunshine_state.json", nlohmann::json({{"root", {{"uniqueid", "ID"}, {"devices", nlohmann::json::array({{{"uniqueid", "x"}, {"certs", nlohmann::json::array({"C1", "C2"})}}})}}}}).dump(4));
  options_t o;
  o.settings = o.apps = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  auto state = read_json(dst / "sunshine_state.json");
  ASSERT_EQ(state["root"]["named_devices"].size(), 2u);
  EXPECT_EQ(state["root"]["named_devices"][0]["perm"], static_cast<uint32_t>(crypto::PERM::_default));
  EXPECT_FALSE(state["root"]["named_devices"][0]["allow_client_commands"].get<bool>());
}

TEST_F(ConfigImportTest, LegacyDevicesAreNormalizedLikeNamedDevices) {
  populate_source();
  write(src / "sunshine_state.json", nlohmann::json({{"root", {{"uniqueid", "ID"}, {"devices", nlohmann::json::array({{{"uniqueid", "x"}, {"certs", nlohmann::json::array({"C1", "C1", "C2"})}}})}}}}).dump(4));
  options_t o;
  o.settings = o.apps = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  auto state = read_json(dst / "sunshine_state.json");
  EXPECT_FALSE(state["root"].contains("devices"));
  // The repeated certificate is dropped and every client keeps its own identifier
  ASSERT_EQ(state["root"]["named_devices"].size(), 2u);
  EXPECT_EQ(state["root"]["named_devices"][0]["cert"], "C1");
  EXPECT_EQ(state["root"]["named_devices"][1]["cert"], "C2");
  EXPECT_NE(state["root"]["named_devices"][0]["uuid"], state["root"]["named_devices"][1]["uuid"]);
}

TEST_F(ConfigImportTest, MissingIdentitySkipsClientsWithWarning) {
  populate_source(false);
  options_t o;
  o.settings = o.apps = false;
  auto r = import_from(src, dst, o);
  EXPECT_FALSE(r.ok);  // nothing else was selected
  EXPECT_TRUE(has_warning(r, "paired clients skipped"));
  EXPECT_FALSE(fs::exists(dst / "sunshine_state.json"));

  options_t all;
  auto r2 = import_from(src, dst, all);
  ASSERT_TRUE(r2.ok) << r2.error;
  EXPECT_TRUE(has_warning(r2, "paired clients skipped"));
  EXPECT_TRUE(fs::exists(dst / "apps.json"));
  EXPECT_FALSE(fs::exists(dst / "sunshine_state.json"));
}

TEST_F(ConfigImportTest, WebCredentialsAreOptIn) {
  populate_source();
  write(dst / "sunshine_state.json", nlohmann::json({{"keep", true}}).dump(4));
  options_t o;
  o.settings = o.apps = o.clients = false;
  o.web_credentials = true;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  auto state = read_json(dst / "sunshine_state.json");
  EXPECT_EQ(state["username"], "admin");
  EXPECT_EQ(state["salt"], "SALT");
  EXPECT_EQ(state["password"], "HASH");
  EXPECT_EQ(state["keep"], true);
  EXPECT_FALSE(state.contains("root"));

  // Clients and credentials share the state file and must both survive.
  fs::remove(dst / "sunshine_state.json");
  o.clients = true;
  r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;
  state = read_json(dst / "sunshine_state.json");
  EXPECT_EQ(state["username"], "admin");
  EXPECT_EQ(state["root"]["uniqueid"], "0123456789ABCDEF");
}

TEST_F(ConfigImportTest, ExistingTargetFilesAreBackedUp) {
  populate_source();
  write(dst / "sunshine.conf", "sunshine_name = Old\n");
  write(dst / "apps.json", "{\"apps\": []}");
  auto r = import_from(src, dst);
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_FALSE(r.backup_dir.empty());
  EXPECT_EQ(read(r.backup_dir / "sunshine.conf"), "sunshine_name = Old\n");
  EXPECT_EQ(read(r.backup_dir / "apps.json"), "{\"apps\": []}");
  EXPECT_EQ(r.backup_dir.parent_path(), dst);
  EXPECT_NE(read(dst / "sunshine.conf").find("Gaming PC"), std::string::npos);
  EXPECT_FALSE(r.imported.empty());
}

TEST_F(ConfigImportTest, SourceIsNeverModified) {
  populate_source();
  auto before = read(src / "sunshine_state.json");
  auto r = import_from(src, dst);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(read(src / "sunshine_state.json"), before);
  EXPECT_FALSE(fs::exists(src / "import-backup"));
  size_t files = 0;
  for (auto &e : fs::recursive_directory_iterator(src)) {
    files += e.is_regular_file();
  }
  EXPECT_EQ(files, 7u);
}

TEST_F(ConfigImportTest, SameDirectoryIsRefused) {
  populate_source();
  auto r = import_from(src, src / ".." / "config");
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("same"), std::string::npos);
  r = import_from(src, src);
  EXPECT_FALSE(r.ok);
}

TEST_F(ConfigImportTest, MissingOrEmptySourceIsRefused) {
  auto r = import_from(scratch / "nope", dst);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.error.empty());
  r = import_from(src, dst);  // exists but empty
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(fs::exists(dst / "sunshine.conf"));
}

TEST_F(ConfigImportTest, InvalidAppsFileFailsWithoutWriting) {
  populate_source();
  write(src / "apps.json", "{ not json");
  auto r = import_from(src, dst);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("apps.json"), std::string::npos);
  EXPECT_FALSE(fs::exists(dst / "sunshine.conf"));
}

TEST_F(ConfigImportTest, CustomFileLocations) {
  write(src / "sunshine.conf", "file_apps = custom/my_apps.json\nfile_state = st/state.json\nsunshine_name = X\n");
  write(src / "custom" / "my_apps.json", "{\"apps\": [{\"name\": \"A\"}]}");
  write(src / "st" / "state.json", nlohmann::json({{"root", {{"uniqueid", "UID"}}}}).dump());
  write(src / "credentials" / "cakey.pem", key_bytes);
  write(src / "credentials" / "cacert.pem", cert_bytes);

  auto info = inspect_source(src);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->has_apps);
  EXPECT_TRUE(info->has_state);

  auto r = import_from(src, dst);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(read_json(dst / "apps.json")["apps"][0]["name"], "A");
  EXPECT_EQ(read_json(dst / "sunshine_state.json")["root"]["uniqueid"], "UID");
  EXPECT_EQ(read(dst / "credentials" / "cakey.pem"), key_bytes);
}

TEST_F(ConfigImportTest, InspectSourceFlags) {
  EXPECT_FALSE(inspect_source(scratch / "missing"));
  EXPECT_FALSE(inspect_source(src));  // empty directory

  write(src / "sunshine.conf", "port = 1\n");
  auto info = inspect_source(src);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->product, "Apollo");
  EXPECT_TRUE(info->has_settings);
  EXPECT_FALSE(info->has_apps);
  EXPECT_FALSE(info->has_state);
  EXPECT_FALSE(info->has_identity);

  populate_source();
  info = inspect_source(src);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->has_apps && info->has_state && info->has_identity);

  auto custom = scratch / "elsewhere";
  write(custom / "sunshine.conf", "port = 1\n");
  ASSERT_TRUE(inspect_source(custom));
  EXPECT_EQ(inspect_source(custom)->product, "Custom");
  write(scratch / "Sunshine" / "config" / "apps.json", "{}");
  EXPECT_EQ(inspect_source(scratch / "Sunshine" / "config")->product, "Sunshine");
}

TEST_F(ConfigImportTest, NonAsciiDirectoryNames) {
  auto base = scratch / from_u8("\xC3\x87\x61\xC4\x9F\x72\xC4\xB1 \xC4\x9F\xC3\xBC\xC5\x9F\x69\xC3\xB6\xC3\xA7");
  auto source = base / "Apollo" / "config";
  auto target = base / "hedef" / "config";
  write(source / "sunshine.conf", "sunshine_name = T\xC3\xBCrk\xC3\xA7\x65\n");
  write(source / "apps.json", nlohmann::json({{"apps", nlohmann::json::array({{{"name", "\xC3\x96yun"}, {"image-path", to_u8(source / "covers" / "k.png")}}})}}).dump(4));
  write(source / "covers" / "k.png", "PNG");

  ASSERT_TRUE(inspect_source(source));
  auto r = import_from(source, target);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NE(read(target / "sunshine.conf").find("T\xC3\xBCrk\xC3\xA7\x65"), std::string::npos);
  auto apps = read_json(target / "apps.json");
  EXPECT_TRUE(fs::equivalent(from_u8(apps["apps"][0]["image-path"].get<std::string>()), target / "covers" / "k.png"));
}

TEST_F(ConfigImportTest, DetectSourcesExcludesAndIgnoresMissing) {
  // Machine specific; only the contract is checked: results exist and never include the excluded path.
  auto sources = detect_sources(scratch);
  for (auto &s : sources) {
    EXPECT_TRUE(fs::exists(s.config_dir));
    EXPECT_FALSE(fs::equivalent(s.config_dir, scratch));
  }
  for (auto &s : detect_sources()) {
    auto again = detect_sources(s.config_dir);
    for (auto &a : again) {
      EXPECT_FALSE(fs::equivalent(a.config_dir, s.config_dir));
    }
  }
}

TEST_F(ConfigImportTest, ImportedClientListIsMadeSafeToLoad) {
  populate_source();
  const auto elevated = static_cast<uint32_t>(crypto::PERM::server_cmd) | static_cast<uint32_t>(crypto::PERM::_all_actions);
  nlohmann::json devices = nlohmann::json::array({
    {{"name", "A"}, {"cert", "CA"}, {"uuid", "dup"}, {"perm", 4294967295u}},
    {{"name", "B"}, {"cert", "CB"}, {"uuid", "dup"}, {"perm", "abc"}},
    {{"name", "C"}, {"cert", "CC"}, {"uuid", ""}, {"perm", -1}},
    {{"name", "D"}, {"cert", "CA"}, {"uuid", "d"}, {"perm", 1}},
    {{"name", "E"}, {"uuid", "e"}, {"perm", 1}},
    {{"name", "F"}, {"cert", 5}, {"uuid", "f"}},
    7,
    {{"name", "G"}, {"cert", "CG"}, {"uuid", "g"}, {"perm", 1.5}},
    {{"name", "H"}, {"cert", "CH"}, {"uuid", "h"}, {"perm", elevated}},
    {{"name", "I"}, {"cert", "CI"}, {"uuid", "i"}, {"perm", static_cast<uint32_t>(crypto::PERM::_default)}},
  });
  write(src / "sunshine_state.json", nlohmann::json({{"root", {{"uniqueid", "ID"}, {"named_devices", devices}}}}).dump(4));
  options_t o;
  o.settings = o.apps = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;

  auto named = read_json(dst / "sunshine_state.json")["root"]["named_devices"];
  // E (no certificate), F (bad certificate), 7 (not an object) and D (repeated certificate) are dropped
  ASSERT_EQ(named.size(), 6u);
  std::set<std::string> uuids;
  for (auto &entry : named) {
    EXPECT_FALSE(entry["uuid"].get<std::string>().empty());
    uuids.insert(entry["uuid"].get<std::string>());
    EXPECT_TRUE(entry["perm"].is_number_unsigned());
    EXPECT_EQ(entry["perm"].get<uint32_t>() & ~static_cast<uint32_t>(crypto::PERM::_all), 0u);
  }
  EXPECT_EQ(uuids.size(), 6u);

  EXPECT_EQ(named[0]["perm"].get<uint32_t>(), static_cast<uint32_t>(crypto::PERM::_all));  // unknown bits masked
  EXPECT_EQ(named[1]["perm"].get<uint32_t>(), static_cast<uint32_t>(crypto::PERM::_default));  // not a number
  EXPECT_EQ(named[2]["perm"].get<uint32_t>(), static_cast<uint32_t>(crypto::PERM::_default));  // negative
  EXPECT_EQ(named[3]["perm"].get<uint32_t>(), static_cast<uint32_t>(crypto::PERM::_default));  // not an integer
  EXPECT_EQ(named[4]["perm"].get<uint32_t>(), elevated);
  EXPECT_EQ(named[5]["perm"].get<uint32_t>(), static_cast<uint32_t>(crypto::PERM::_default));
  EXPECT_NE(named[0]["uuid"], named[1]["uuid"]);

  EXPECT_TRUE(has_warning(r, "default permission"));
  EXPECT_TRUE(has_warning(r, "skipped"));
  EXPECT_TRUE(has_warning(r, "new one"));
  EXPECT_TRUE(has_warning(r, "review them"));
}

TEST_F(ConfigImportTest, ConfFileWithByteOrderMarkKeepsItsFirstKey) {
  populate_source();
  write(src / "sunshine.conf", "\xEF\xBB\xBF" "sunshine_name = Bom PC\nlog_path = sunshine.log\n");
  options_t o;
  o.apps = o.clients = false;
  auto r = import_from(src, dst, o);
  ASSERT_TRUE(r.ok) << r.error;

  const auto conf = read(dst / "sunshine.conf");
  EXPECT_EQ(conf.find("\xEF\xBB\xBF"), std::string::npos);
  EXPECT_NE(conf.find("sunshine_name = Bom PC"), std::string::npos);
}
