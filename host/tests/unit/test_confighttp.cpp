/**
 * @file tests/unit/test_confighttp.cpp
 * @brief Test the request validation of the Web UI server in src/confighttp.cpp.
 */
#include <climits>
#include <string>

#include <nlohmann/json.hpp>

#include <src/confighttp.h>

#include "../tests_common.h"

using nlohmann::json;

namespace {
  struct origin_case_t {
    const char *name;
    std::string method;
    std::string host;
    std::string origin;
    std::string referer;
    std::string fetch_site;
    bool allowed;
  };

  std::string long_origin() {
    return "https://" + std::string(5000, 'a');
  }
}  // namespace

struct OriginCheckTest: testing::TestWithParam<origin_case_t> {};

TEST_P(OriginCheckTest, Run) {
  const auto &c = GetParam();
  EXPECT_EQ(confighttp::is_same_origin(c.method, c.host, c.origin, c.referer, c.fetch_site), c.allowed) << c.name;
}

INSTANTIATE_TEST_SUITE_P(
  OriginCheckTests,
  OriginCheckTest,
  testing::Values(
    origin_case_t {"ip", "POST", "192.168.1.5:47990", "https://192.168.1.5:47990", "", "", true},
    origin_case_t {"mixed case name", "POST", "PC.local:47990", "https://pc.LOCAL:47990", "", "", true},
    origin_case_t {"localhost", "POST", "localhost:47990", "https://localhost:47990", "", "", true},
    origin_case_t {"ipv6", "POST", "[::1]:47990", "https://[::1]:47990", "", "", true},
    origin_case_t {"default port", "POST", "pc.local", "https://pc.local:443", "", "", true},
    origin_case_t {"foreign origin", "POST", "host:47990", "https://evil.example", "", "", false},
    origin_case_t {"other port", "POST", "host:47990", "https://host:8080", "", "", false},
    origin_case_t {"http scheme", "POST", "host:47990", "http://host:47990", "", "", false},
    origin_case_t {"null origin", "POST", "host:47990", "null", "", "", false},
    origin_case_t {"missing host", "POST", "", "https://host:47990", "", "", false},
    origin_case_t {"fetch same-origin", "POST", "host:47990", "", "", "same-origin", true},
    origin_case_t {"fetch none", "POST", "host:47990", "", "", "none", true},
    origin_case_t {"fetch same-site", "POST", "host:47990", "", "", "same-site", false},
    origin_case_t {"fetch cross-site beats origin", "POST", "host:47990", "https://host:47990", "", "cross-site", false},
    origin_case_t {"no browser headers", "POST", "host:47990", "", "", "", true},
    origin_case_t {"referer matching", "POST", "host:47990", "", "https://host:47990/config", "", true},
    origin_case_t {"referer foreign", "POST", "host:47990", "", "https://evil.example/x", "", false},
    origin_case_t {"get is never checked", "GET", "host:47990", "https://evil.example", "", "cross-site", true},
    origin_case_t {"delete is checked", "DELETE", "host:47990", "https://evil.example", "", "", false},
    origin_case_t {"overlong origin", "POST", "host:47990", long_origin(), "", "", false},
    origin_case_t {"userinfo in origin", "POST", "host:47990", "https://host:47990@evil.example", "", "", false}
  )
);

namespace {
  json minimal_app() {
    return {{"name", "App"}};
  }

  // The structure the Web UI sends for a new application
  json ui_app() {
    return {
      {"name", "Desktop"},
      {"output", ""},
      {"cmd", ""},
      {"working-dir", ""},
      {"exclude-global-prep-cmd", false},
      {"elevated", false},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5},
      {"prep-cmd", json::array()},
      {"detached", json::array()},
      {"image-path", ""},
      {"scale-factor", 100},
      {"virtual-display", false},
      {"use-app-identity", false},
      {"per-client-app-identity", false},
      {"allow-client-commands", true},
      {"terminate-on-pause", false},
      {"gamepad", ""},
      {"uuid", ""},
    };
  }

  json with(json app, const std::string &key, json value) {
    app[key] = std::move(value);
    return app;
  }
}  // namespace

TEST(ValidateAppTest, AcceptsValidApps) {
  EXPECT_FALSE(confighttp::validate_app(minimal_app()));
  EXPECT_FALSE(confighttp::validate_app(ui_app()));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "unknown-key", json::object())));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "image-path", "C:/x/y.PNG")));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "exit-timeout", 0)));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "uuid", "")));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "uuid", "0123abcd-ABCD-4000-8000-000000000001")));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "prep-cmd", json::array({{{"do", "a"}, {"undo", "b"}, {"elevated", true}}}))));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "detached", json::array({"cmd"}))));
}

TEST(ValidateAppTest, RejectsMissingOrBadName) {
  EXPECT_TRUE(confighttp::validate_app(json::object()));
  EXPECT_TRUE(confighttp::validate_app(with(json::object(), "a", "AAAA")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", "")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", "   ")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", 5)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", nullptr)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", "a\nb")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", std::string("a\0b", 3))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "name", std::string(257, 'x'))));
  EXPECT_FALSE(confighttp::validate_app(with(minimal_app(), "name", std::string(256, 'x'))));
}

TEST(ValidateAppTest, RejectsNonObjects) {
  EXPECT_TRUE(confighttp::validate_app(json::array()));
  EXPECT_TRUE(confighttp::validate_app(json("x")));
  EXPECT_TRUE(confighttp::validate_app(json(5)));
  EXPECT_TRUE(confighttp::validate_app(json(nullptr)));
}

TEST(ValidateAppTest, RejectsWrongTypes) {
  for (const char *key : {"cmd", "output", "working-dir", "image-path", "gamepad"}) {
    EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), key, 5))) << key;
    EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), key, json::array()))) << key;
  }
  for (const char *key : {"elevated", "auto-detach", "wait-all", "virtual-display", "terminate-on-pause", "allow-client-commands"}) {
    EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), key, "true"))) << key;
  }
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "exit-timeout", -1)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "exit-timeout", 3601)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "exit-timeout", "5")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "exit-timeout", 1.5)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "scale-factor", 0)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "scale-factor", 10000)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "prep-cmd", json::object())));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "prep-cmd", json::array({5}))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "prep-cmd", json::array({{{"do", 5}}}))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "state-cmd", json::array({{{"elevated", "yes"}}}))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "detached", json::array({1}))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "uuid", 5)));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "uuid", "has space")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "uuid", std::string(65, 'a'))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "image-path", "a.jpg")));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "cmd", std::string(8193, 'x'))));
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "cmd", std::string("a\0b", 3))));
}

TEST(ValidateAppTest, BoundsListsAndNesting) {
  json many = json::array();
  for (int i = 0; i < 65; ++i) {
    many.push_back("cmd");
  }
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "detached", many)));

  json deep = "leaf";
  for (int i = 0; i < 20; ++i) {
    deep = json::array({deep});
  }
  EXPECT_TRUE(confighttp::validate_app(with(minimal_app(), "extra", deep)));
}

TEST(ValidateConfigTest, RejectsNonObjects) {
  EXPECT_FALSE(confighttp::validate_config_object(json::array()).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object(json(5)).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object(json("x")).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object(json(nullptr)).error.empty());
  EXPECT_TRUE(confighttp::validate_config_object(json::object()).error.empty());
}

TEST(ValidateConfigTest, RejectsUnsafeKeysAndValues) {
  EXPECT_FALSE(confighttp::validate_config_object({{"a\nb", "1"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"a=b", "1"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"a#b", "1"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"", "1"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"sunshine_name", "a\nb = c"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"sunshine_name", json::object({{"a", 1}})}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"sunshine_name", std::string(9000, 'x')}}).error.empty());
  // A list may span lines
  EXPECT_TRUE(confighttp::validate_config_object({{"global_prep_cmd", "[\n{\"do\":\"a\"}\n]"}}).error.empty());
  EXPECT_TRUE(confighttp::validate_config_object({{"global_prep_cmd", json::array({{{"do", "a"}}})}}).error.empty());
}

TEST(ValidateConfigTest, ChecksThePort) {
  EXPECT_FALSE(confighttp::validate_config_object({{"port", 0}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"port", 1023}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"port", 65535}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"port", "abc"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"port", json::object({{"a", 1}})}}).error.empty());
  EXPECT_TRUE(confighttp::validate_config_object({{"port", 47989}}).error.empty());
  EXPECT_TRUE(confighttp::validate_config_object({{"port", "47989"}}).error.empty());
}

TEST(ValidateConfigTest, ReportsUnknownOptionsAndKeepsThem) {
  auto result = confighttp::validate_config_object({{"sunshine_name", "Host"}, {"nosuchkey", "1"}, {"qp", 28}});
  EXPECT_TRUE(result.error.empty());
  ASSERT_EQ(result.unknown_keys.size(), 1u);
  EXPECT_EQ(result.unknown_keys[0], "nosuchkey");
}

TEST(ValidateConfigTest, RendersOneOptionPerLine) {
  const json input = {{"sunshine_name", "Host"}, {"empty", ""}, {"unset", nullptr}, {"qp", 28}, {"upnp", true}};
  const auto text = confighttp::render_config_file(input);
  EXPECT_NE(text.find("sunshine_name = Host\n"), std::string::npos);
  EXPECT_NE(text.find("qp = 28\n"), std::string::npos);
  EXPECT_NE(text.find("upnp = true\n"), std::string::npos);
  EXPECT_EQ(text.find("empty"), std::string::npos);
  EXPECT_EQ(text.find("unset"), std::string::npos);
}

TEST(ValidateConfigTest, RoundTripOfTheUiDocumentIsAccepted) {
  // GET /api/config returns every value as text; the UI posts the changed options back
  const json document = {
    {"sunshine_name", "Gaming PC"},
    {"port", "47989"},
    {"origin_web_ui_allowed", "lan"},
    {"min_log_level", "info"},
    {"hevc_mode", "2"},
    {"global_prep_cmd", "[{\"do\":\"a\",\"undo\":\"b\",\"elevated\":false}]"},
    {"upnp", "enabled"},
  };
  auto result = confighttp::validate_config_object(document);
  EXPECT_TRUE(result.error.empty());
  EXPECT_TRUE(result.unknown_keys.empty());
}

TEST(BodyLimitTest, LimitsPerEndpoint) {
  EXPECT_LE(confighttp::body_limit_for("POST", "/api/login"), 4096u);
  EXPECT_LE(confighttp::body_limit_for("POST", "/api/password"), 4096u);
  EXPECT_LE(confighttp::body_limit_for("POST", "/api/pin"), 4096u);
  EXPECT_LE(confighttp::body_limit_for("DELETE", "/api/pin"), 4096u);
  EXPECT_EQ(confighttp::body_limit_for("POST", "/api/apps"), 256u * 1024);
  EXPECT_EQ(confighttp::body_limit_for("POST", "/api/config"), 256u * 1024);
  EXPECT_EQ(confighttp::body_limit_for("POST", "/api/covers/upload"), confighttp::MAX_COVER_REQUEST_SIZE);
  EXPECT_EQ(confighttp::body_limit_for("POST", "/api/unknown"), 4096u);
  for (const char *path : {"/api/login", "/api/apps", "/api/config", "/api/covers/upload", "/x"}) {
    EXPECT_LE(confighttp::body_limit_for("POST", path), confighttp::MAX_REQUEST_SIZE) << path;
  }
}

TEST(LoginDelayTest, GrowsAndSaturates) {
  const int expected[] = {1, 2, 4, 8, 16, 32, 60, 60, 60, 60};
  for (int failures = 1; failures <= 10; ++failures) {
    EXPECT_EQ(confighttp::login_delay_seconds(failures), expected[failures - 1]) << failures;
  }
  EXPECT_EQ(confighttp::login_delay_seconds(0), 0);
  EXPECT_EQ(confighttp::login_delay_seconds(-3), 0);
  EXPECT_EQ(confighttp::login_delay_seconds(INT_MAX), 60);
}

TEST(CookieValueTest, MatchesWholeNamesOnly) {
  EXPECT_EQ(confighttp::getCookieValue("auth=abc", "auth"), "abc");
  EXPECT_EQ(confighttp::getCookieValue("a=1; auth=abc; b=2", "auth"), "abc");
  EXPECT_EQ(confighttp::getCookieValue("xauth=zzz", "auth"), "");
  EXPECT_EQ(confighttp::getCookieValue("auth=", "auth"), "");
  EXPECT_EQ(confighttp::getCookieValue("auth=abc  ", "auth"), "abc");
  EXPECT_EQ(confighttp::getCookieValue("auth", "auth"), "");
  EXPECT_EQ(confighttp::getCookieValue("auth=first; auth=second", "auth"), "first");
  EXPECT_EQ(confighttp::getCookieValue(std::string(100 * 1024, 'x'), "auth"), "");
  EXPECT_EQ(confighttp::getCookieValue("", "auth"), "");
}

TEST(FirstSetupTest, OnlyTheHostPcMayCreateTheFirstCredentials) {
  EXPECT_TRUE(confighttp::first_setup_allowed(net::PC));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::LAN));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::WAN));
}

TEST(FirstSetupTest, ClassifiesLoopbackAndLanAddresses) {
  EXPECT_TRUE(confighttp::first_setup_allowed(net::from_address("127.0.0.1")));
  EXPECT_TRUE(confighttp::first_setup_allowed(net::from_address("::1")));
  EXPECT_TRUE(confighttp::first_setup_allowed(net::from_address("::ffff:127.0.0.1")));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::from_address("192.168.1.20")));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::from_address("10.0.0.5")));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::from_address("fe80::1")));
  EXPECT_FALSE(confighttp::first_setup_allowed(net::from_address("203.0.113.9")));
}

TEST(FirstSetupTest, RefusalMessageNamesTheHostPc) {
  EXPECT_NE(confighttp::FIRST_SETUP_LOCAL_ONLY_MESSAGE.find("host PC"), std::string_view::npos);
  EXPECT_NE(confighttp::FIRST_SETUP_LOCAL_ONLY_MESSAGE.find("localhost"), std::string_view::npos);
}

TEST(ValidateConfigTest, RejectsLineBreaksAfterAListPrefix) {
  EXPECT_FALSE(confighttp::validate_config_object({{"global_prep_cmd", "[1]\nfile_apps = x"}}).error.empty());
  EXPECT_FALSE(confighttp::validate_config_object({{"global_prep_cmd", "[\nbroken"}}).error.empty());
  EXPECT_TRUE(confighttp::validate_config_object({{"global_prep_cmd", "[\n{\"do\":\"a\"}\n]"}}).error.empty());
}
