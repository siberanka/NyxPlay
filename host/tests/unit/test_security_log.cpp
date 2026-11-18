/**
 * @file tests/unit/test_security_log.cpp
 * @brief Test the security event log and the automatic blocking in src/security_log.*
 */
#include <src/confighttp.h>
#include <src/security_log.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "../tests_common.h"

using namespace std::literals;
namespace sl = security_log;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {
  fs::path make_temp_dir(const std::string &name) {
#ifdef SUNSHINE_TEST_BIN_DIR
    fs::path base = fs::path {SUNSHINE_TEST_BIN_DIR} / "security_log_test";
#else
    fs::path base = fs::temp_directory_path() / "security_log_test";
#endif
    auto dir = base / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
  }

  sl::event_t make_event(std::int64_t ts, const std::string &type, const std::string &ip, const std::string &result = "ok") {
    sl::event_t event;
    event.ts_ms = ts;
    event.type = type;
    event.ip = ip;
    event.service = "https";
    event.result = result;
    return event;
  }

  /// A clock the tests move by hand.
  struct fake_clock_t {
    std::int64_t now = 1'800'000'000'000;
    sl::ban_manager_t::clock_t fn() {
      return [this] {
        return now;
      };
    }
  };

  sl::ban_settings_t ban_settings(int threshold = 10, int window = 600, int duration = 1800) {
    sl::ban_settings_t settings;
    settings.enabled = true;
    settings.threshold = threshold;
    settings.window_seconds = window;
    settings.duration_seconds = duration;
    return settings;
  }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Sanitising and formatting
// ---------------------------------------------------------------------------------------------

TEST(SecurityLogText, ControlCharactersBecomeSpaces) {
  EXPECT_EQ(sl::sanitize_text("a\r\nb\tc"s, 64), "a  b c");
  EXPECT_EQ(sl::sanitize_text("x\0y"s, 64), "x y");
  EXPECT_EQ(sl::sanitize_text("\x1b[31mred"s, 64), "[31mred");
  EXPECT_EQ(sl::sanitize_text("  padded  "s, 64), "padded");
}

TEST(SecurityLogText, LengthIsLimitedOnACharacterBoundary) {
  EXPECT_EQ(sl::sanitize_text("abcdef"s, 3), "abc");
  // "ş" is two bytes: a limit in the middle of it drops the whole character
  EXPECT_EQ(sl::sanitize_text("aş"s, 2), "a");
  EXPECT_EQ(sl::sanitize_text("aş"s, 3), "aş");
}

TEST(SecurityLogText, InvalidAndInvisibleCharactersAreRemoved) {
  EXPECT_EQ(sl::sanitize_text("ab\xff" "cd"s, 64), "ab?cd");
  // Overlong encoding of '/' and a lone continuation byte
  EXPECT_EQ(sl::sanitize_text("\xc0\xaf"s, 64), "??");
  // Right-to-left override and line separator
  EXPECT_EQ(sl::sanitize_text("a\xe2\x80\xae" "b\xe2\x80\xa8" "c"s, 64), "abc");
  // Turkish text is kept
  EXPECT_EQ(sl::sanitize_text("Çağrı İstemci"s, 64), "Çağrı İstemci");
}

TEST(SecurityLogText, AddressesAreNormalised) {
  EXPECT_EQ(sl::normalize_ip("::ffff:192.0.2.7"), "192.0.2.7");
  EXPECT_EQ(sl::normalize_ip(" 192.0.2.7 "), "192.0.2.7");
  EXPECT_EQ(sl::normalize_ip("2001:DB8::1"), "2001:db8::1");
  EXPECT_EQ(sl::normalize_ip("fe80::1%12"), "fe80::1");
  EXPECT_EQ(sl::normalize_ip("not an address"), "");
  EXPECT_EQ(sl::normalize_ip(""), "");
}

TEST(SecurityLogText, TimeRoundTrip) {
  EXPECT_EQ(sl::format_time(0), "1970-01-01T00:00:00.000Z");
  EXPECT_EQ(sl::format_time(1'700'000'000'123), "2023-11-14T22:13:20.123Z");
  EXPECT_EQ(sl::parse_time("2023-11-14T22:13:20.123Z"), 1'700'000'000'123);
  EXPECT_EQ(sl::parse_time("2023-11-14T22:13:20Z"), 1'700'000'000'000);
  EXPECT_EQ(sl::parse_time("2023-11-14T22:13Z"), 1'699'999'980'000);
  EXPECT_EQ(sl::parse_time("2023-11-14"), 1'699'920'000'000);
  EXPECT_EQ(sl::parse_time("2023-11-15T01:13:20+03:00"), 1'700'000'000'000);
  for (std::int64_t ms : {1'234'567'890'123LL, 1'800'000'000'999LL, 951'782'400'000LL}) {
    EXPECT_EQ(sl::parse_time(sl::format_time(ms)), ms);
  }
}

TEST(SecurityLogText, InvalidTimesAreRejected) {
  for (const char *text : {"", "yesterday", "2023-13-01", "2023-02-30", "2023-11-14T25:00:00Z", "2023-11-14T22:13:20.Z", "2023-11-14T22:13:20Zjunk", "99-11-14"}) {
    EXPECT_FALSE(sl::parse_time(text)) << text;
  }
}

TEST(SecurityLogText, EventJsonRoundTripAndSanitising) {
  sl::event_t event = make_event(1'700'000'000'123, "launch", "192.0.2.7");
  event.client_name = "Phone";
  event.client_uuid = "abc";
  event.app_name = "Desktop";
  event.detail = "ok";
  auto doc = sl::to_json(event);
  EXPECT_EQ(doc["ts"], "2023-11-14T22:13:20.123Z");
  EXPECT_EQ(doc["client_name"], "Phone");

  auto back = sl::from_json(doc);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->ts_ms, event.ts_ms);
  EXPECT_EQ(back->app_name, "Desktop");

  // A tampered file cannot smuggle line breaks into the log
  doc["client_name"] = "evil\nINJECT";
  back = sl::from_json(doc);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->client_name, "evil INJECT");

  EXPECT_FALSE(sl::from_json(json::object()));
  EXPECT_FALSE(sl::from_json(json("text")));
  doc.erase("ts");
  EXPECT_FALSE(sl::from_json(doc));
}

// ---------------------------------------------------------------------------------------------
// Event store
// ---------------------------------------------------------------------------------------------

namespace {
  void fill_store(sl::event_store_t &store) {
    auto add = [&](std::int64_t ts, const char *type, const char *ip, const char *result, const char *name, const char *uuid, const char *app) {
      auto event = make_event(ts, type, ip, result);
      event.client_name = name;
      event.client_uuid = uuid;
      event.app_name = app;
      store.add(event);
    };
    add(1000, "pair_failure", "192.0.2.7", "fail", "", "", "");
    add(2000, "launch", "192.0.2.8", "ok", "Living Room TV", "uuid-1", "Desktop");
    add(3000, "resume", "192.0.2.8", "ok", "Living Room TV", "uuid-1", "Desktop");
    add(4000, "launch", "198.51.100.9", "ok", "Phone", "uuid-2", "Steam Big Picture");
    add(5000, "cert_rejected", "198.51.100.9", "fail", "", "", "");
  }
}  // namespace

TEST(SecurityEventStore, NewestFirstAndTotal) {
  sl::event_store_t store;
  fill_store(store);
  auto [total, events] = store.query({});
  EXPECT_EQ(total, 5u);
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events.front().type, "cert_rejected");
  EXPECT_EQ(events.back().type, "pair_failure");
}

TEST(SecurityEventStore, Filters) {
  sl::event_store_t store;
  fill_store(store);

  sl::filter_t filter;
  filter.types = {"launch", "resume"};
  EXPECT_EQ(store.query(filter).first, 3u);

  filter = {};
  filter.result = "fail";
  EXPECT_EQ(store.query(filter).first, 2u);

  filter = {};
  filter.ip = "198.51";
  EXPECT_EQ(store.query(filter).first, 2u);

  filter = {};
  filter.client = "LIVING";
  EXPECT_EQ(store.query(filter).first, 2u);
  filter.client = "uuid-2";
  EXPECT_EQ(store.query(filter).first, 1u);

  filter = {};
  filter.app = "big pic";
  EXPECT_EQ(store.query(filter).first, 1u);

  filter = {};
  filter.since = 2000;
  filter.until = 4000;
  EXPECT_EQ(store.query(filter).first, 3u);

  // Type and result are exact, not substrings
  filter = {};
  filter.types = {"launc"};
  EXPECT_EQ(store.query(filter).first, 0u);
  filter = {};
  filter.result = "f";
  EXPECT_EQ(store.query(filter).first, 0u);
}

TEST(SecurityEventStore, Paging) {
  sl::event_store_t store;
  fill_store(store);
  sl::filter_t filter;
  filter.limit = 2;
  auto [total, page1] = store.query(filter);
  EXPECT_EQ(total, 5u);
  ASSERT_EQ(page1.size(), 2u);
  EXPECT_EQ(page1[0].ts_ms, 5000);
  EXPECT_EQ(page1[1].ts_ms, 4000);

  filter.offset = 2;
  auto page2 = store.query(filter).second;
  ASSERT_EQ(page2.size(), 2u);
  EXPECT_EQ(page2[0].ts_ms, 3000);

  filter.offset = 4;
  EXPECT_EQ(store.query(filter).second.size(), 1u);
  filter.offset = 10;
  EXPECT_TRUE(store.query(filter).second.empty());
}

TEST(SecurityEventStore, CapacityDropsTheOldest) {
  sl::event_store_t store {3};
  for (int i = 1; i <= 5; ++i) {
    store.add(make_event(i, "launch", "192.0.2.1"));
  }
  EXPECT_EQ(store.size(), 3u);
  auto events = store.query({}).second;
  EXPECT_EQ(events.front().ts_ms, 5);
  EXPECT_EQ(events.back().ts_ms, 3);

  store.clear();
  EXPECT_EQ(store.size(), 0u);
}

TEST(SecurityEventStore, Statistics) {
  sl::event_store_t store;
  fill_store(store);
  auto stats = store.stats(0);
  EXPECT_EQ(stats["by_type"]["launch"], 2);
  EXPECT_EQ(stats["by_type"]["pair_failure"], 1);
  ASSERT_EQ(stats["top_ips"].size(), 3u);
  // Two events each: the address with the failure comes first
  EXPECT_EQ(stats["top_ips"][0]["ip"], "198.51.100.9");
  EXPECT_EQ(stats["top_ips"][0]["failures"], 1);
  ASSERT_EQ(stats["top_clients"].size(), 2u);
  EXPECT_EQ(stats["top_clients"][0]["client_uuid"], "uuid-1");
  EXPECT_EQ(stats["top_clients"][0]["launches"], 2);
  EXPECT_EQ(stats["top_clients"][0]["client_name"], "Living Room TV");

  // Only events inside the period are counted
  stats = store.stats(4000);
  EXPECT_EQ(stats["by_type"].size(), 2u);
}

TEST(SecurityEventStore, QueryParameters) {
  std::string error;
  auto filter = sl::parse_filter({{"type", "Launch, resume"}, {"ip", "192.0"}, {"limit", "5000"}, {"offset", "3"}, {"since", "2023-11-14T22:13:20Z"}}, error);
  ASSERT_TRUE(filter) << error;
  EXPECT_EQ(filter->types, (std::vector<std::string> {"launch", "resume"}));
  EXPECT_EQ(filter->limit, 1000u);
  EXPECT_EQ(filter->offset, 3u);
  EXPECT_EQ(filter->since, 1'700'000'000'000);

  filter = sl::parse_filter({}, error);
  ASSERT_TRUE(filter);
  EXPECT_EQ(filter->limit, 200u);

  EXPECT_FALSE(sl::parse_filter({{"limit", "many"}}, error));
  EXPECT_FALSE(sl::parse_filter({{"offset", "-1"}}, error));
  EXPECT_FALSE(sl::parse_filter({{"since", "tomorrow"}}, error));
  EXPECT_FALSE(sl::parse_filter({{"type", "a;b"}}, error));
  EXPECT_EQ(sl::parse_filter({{"limit", "0"}}, error)->limit, 1u);

  EXPECT_EQ(sl::parse_hours({}, error), 24);
  EXPECT_EQ(sl::parse_hours({{"hours", "9999"}}, error), 720);
  EXPECT_EQ(sl::parse_hours({{"hours", "0"}}, error), 1);
  EXPECT_FALSE(sl::parse_hours({{"hours", "x"}}, error));
}

// ---------------------------------------------------------------------------------------------
// Event file
// ---------------------------------------------------------------------------------------------

TEST(SecurityEventFile, AppendAndLoadTail) {
  auto dir = make_temp_dir("file_append");
  sl::event_file_t file {dir / "security_events.log", 1024 * 1024};

  std::vector<std::string> lines;
  for (int i = 1; i <= 10; ++i) {
    lines.push_back(sl::to_json(make_event(1'700'000'000'000 + i, "launch", "192.0.2.1")).dump());
  }
  ASSERT_TRUE(file.append(lines));

  auto events = file.load_tail(100);
  ASSERT_EQ(events.size(), 10u);
  EXPECT_EQ(events.front().ts_ms, 1'700'000'000'001);
  EXPECT_EQ(events.back().ts_ms, 1'700'000'000'010);

  // The tail is bounded and keeps the newest events
  events = file.load_tail(3);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events.front().ts_ms, 1'700'000'000'008);
}

TEST(SecurityEventFile, DamagedLinesAreSkipped) {
  auto dir = make_temp_dir("file_damaged");
  auto path = dir / "security_events.log";
  {
    std::ofstream out(path, std::ios::binary);
    out << "this is not json\n";
    out << sl::to_json(make_event(1'700'000'000'000, "launch", "192.0.2.1")).dump() << "\r\n";
    out << "{\"ts\":\"nonsense\",\"type\":\"launch\"}\n";
    out << "{\"half\":";
  }
  sl::event_file_t file {path, 1024 * 1024};
  auto events = file.load_tail(10);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, "launch");
}

TEST(SecurityEventFile, RotationKeepsOnePreviousFile) {
  auto dir = make_temp_dir("file_rotate");
  auto path = dir / "security_events.log";
  sl::event_file_t file {path, 2000};

  std::int64_t ts = 1'700'000'000'000;
  for (int i = 0; i < 60; ++i) {
    ASSERT_TRUE(file.append({sl::to_json(make_event(ts + i, "launch", "192.0.2.1")).dump()}));
  }

  auto rotated = path;
  rotated += ".1";
  EXPECT_TRUE(fs::exists(path));
  EXPECT_TRUE(fs::exists(rotated));
  EXPECT_LE(fs::file_size(path), 2000u);
  // Never more than the two generations
  EXPECT_FALSE(fs::exists(dir / "security_events.log.2"));

  // The newest event survives, the loaded list stays in order across both files
  auto events = file.load_tail(1000);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().ts_ms, ts + 59);
  for (std::size_t i = 1; i < events.size(); ++i) {
    EXPECT_LT(events[i - 1].ts_ms, events[i].ts_ms);
  }

  file.clear();
  EXPECT_FALSE(fs::exists(path));
  EXPECT_FALSE(fs::exists(rotated));
}

// ---------------------------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------------------------

TEST(SecurityAddress, GroupsAreIpv4HostsAndIpv6Prefixes) {
  auto v4 = sl::group_of("192.0.2.7");
  ASSERT_TRUE(v4);
  EXPECT_EQ(v4->key, "192.0.2.7");
  EXPECT_EQ(v4->display, "192.0.2.7");

  EXPECT_EQ(sl::group_of("::ffff:192.0.2.7")->key, v4->key);

  auto a = sl::group_of("2001:db8:1:2:aaaa:bbbb:cccc:dddd");
  auto b = sl::group_of("2001:db8:1:2::1");
  auto c = sl::group_of("2001:db8:1:3::1");
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(a->key, b->key);
  EXPECT_NE(a->key, c->key);
  EXPECT_EQ(a->display, "2001:db8:1:2::/64");
  // The displayed form is accepted again
  EXPECT_EQ(sl::group_of(a->display)->key, a->key);
  EXPECT_EQ(sl::group_key("2001:db8:1:2::77"), a->key);

  EXPECT_TRUE(sl::group_of("127.0.0.1")->loopback);
  EXPECT_TRUE(sl::group_of("::1")->loopback);
  EXPECT_TRUE(sl::group_of("::ffff:127.0.0.1")->loopback);
  EXPECT_FALSE(sl::group_of("0.0.0.0"));
  EXPECT_FALSE(sl::group_of("224.0.0.1"));
  EXPECT_FALSE(sl::group_of("garbage"));
  EXPECT_FALSE(sl::group_of("192.0.2.7/24"));
  EXPECT_EQ(sl::group_key("garbage"), "");
}

TEST(SecurityAddress, Whitelist) {
  std::vector<sl::cidr_t> list;
  std::string bad;
  ASSERT_TRUE(sl::parse_whitelist("192.168.1.20, 10.0.0.0/24;2001:db8::/32 [\"172.16.0.5\"]", list, bad)) << bad;
  ASSERT_EQ(list.size(), 4u);

  auto contains = [&](const char *text) {
    return std::ranges::any_of(list, [&](const sl::cidr_t &cidr) {
      return cidr.contains(boost::asio::ip::make_address(text));
    });
  };
  EXPECT_TRUE(contains("192.168.1.20"));
  EXPECT_FALSE(contains("192.168.1.21"));
  EXPECT_TRUE(contains("10.0.0.200"));
  EXPECT_FALSE(contains("10.0.1.1"));
  EXPECT_TRUE(contains("2001:db8:ffff::1"));
  EXPECT_FALSE(contains("2001:db9::1"));
  EXPECT_TRUE(contains("172.16.0.5"));
  EXPECT_TRUE(contains("::ffff:192.168.1.20"));

  list.clear();
  EXPECT_FALSE(sl::parse_whitelist("192.168.1.20, nonsense, 10.0.0.0/8", list, bad));
  EXPECT_EQ(bad, "nonsense");
  EXPECT_FALSE(sl::parse_whitelist("10.0.0.0/33", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("10.0.0.0/-1", list, bad));
  list.clear();
  EXPECT_TRUE(sl::parse_whitelist("", list, bad));
  EXPECT_TRUE(list.empty());
}

// ---------------------------------------------------------------------------------------------
// Ban manager
// ---------------------------------------------------------------------------------------------

TEST(SecurityBans, ThresholdWithinTheWindow) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(5, 60, 1800));

  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "pair_failure"));
    clock.now += 1000;
  }
  EXPECT_FALSE(bans.is_blocked("203.0.113.5"));

  auto ban = bans.record_failure("203.0.113.5", 1, "pair_failure");
  ASSERT_TRUE(ban);
  EXPECT_EQ(ban->display, "203.0.113.5");
  EXPECT_EQ(ban->failures, 5);
  EXPECT_FALSE(ban->manual);
  EXPECT_EQ(ban->until_ms, clock.now + 1'800'000);
  EXPECT_TRUE(bans.is_blocked("203.0.113.5"));
  EXPECT_TRUE(bans.is_blocked(boost::asio::ip::make_address("::ffff:203.0.113.5")));
  EXPECT_FALSE(bans.is_blocked("203.0.113.6"));
  EXPECT_EQ(bans.active_count(), 1u);

  // Further failures of a blocked address do not create another block
  EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "pair_failure"));
}

TEST(SecurityBans, FailuresOutsideTheWindowDoNotCount) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 10, 60));

  EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "x"));
  EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "x"));
  clock.now += 11'000;
  EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "x"));
  EXPECT_FALSE(bans.is_blocked("203.0.113.5"));
  EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "x"));
  EXPECT_TRUE(bans.record_failure("203.0.113.5", 1, "x"));
}

TEST(SecurityBans, WeightsAddUp) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(10, 600, 60));
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(bans.record_failure("203.0.113.5", 2, "pair_failure"));
  }
  EXPECT_TRUE(bans.record_failure("203.0.113.5", 2, "pair_failure"));

  EXPECT_EQ(sl::failure_weight("pair_failure"), 2);
  EXPECT_EQ(sl::failure_weight("webui_login_failure"), 2);
  // Logged, but never a reason to block: unpaired clients try HTTPS with their own certificate before pairing,
  // and a paired client without a permission is not an attacker
  EXPECT_EQ(sl::failure_weight("cert_rejected"), 0);
  EXPECT_EQ(sl::failure_weight("permission_denied"), 0);
  EXPECT_EQ(sl::failure_weight("rtsp_rejected"), 1);
  EXPECT_EQ(sl::failure_weight("launch"), 0);
  EXPECT_EQ(sl::failure_weight("webui_denied"), 0);
}

TEST(SecurityBans, ExpiryIsLazyAndReported) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 60, 100));
  for (int i = 0; i < 3; ++i) {
    bans.record_failure("203.0.113.5", 1, "x");
  }
  ASSERT_TRUE(bans.is_blocked("203.0.113.5"));

  clock.now += 99'000;
  EXPECT_TRUE(bans.is_blocked("203.0.113.5"));
  clock.now += 2'000;
  EXPECT_FALSE(bans.is_blocked("203.0.113.5"));
  EXPECT_EQ(bans.active_count(), 0u);

  auto expired = bans.take_expired();
  ASSERT_EQ(expired.size(), 1u);
  EXPECT_EQ(expired[0].display, "203.0.113.5");
  EXPECT_TRUE(bans.take_expired().empty());
  EXPECT_TRUE(bans.list().empty());
}

TEST(SecurityBans, ZeroDurationBlocksUntilLifted) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 60, 0));
  for (int i = 0; i < 3; ++i) {
    bans.record_failure("203.0.113.5", 1, "x");
  }
  clock.now += 365LL * 24 * 3600 * 1000;
  EXPECT_TRUE(bans.is_blocked("203.0.113.5"));
  auto list = bans.list();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].until_ms, 0);
  EXPECT_TRUE(sl::to_json(list[0])["until"].is_null());

  sl::ban_t removed;
  EXPECT_TRUE(bans.remove("203.0.113.5", &removed));
  EXPECT_FALSE(bans.is_blocked("203.0.113.5"));
  EXPECT_FALSE(bans.remove("203.0.113.5", nullptr));
}

TEST(SecurityBans, LoopbackAndWhitelistAreNeverBlocked) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  auto settings = ban_settings(3, 60, 60);
  std::string bad;
  ASSERT_TRUE(sl::parse_whitelist("192.168.1.0/24, 2001:db8:5::/48", settings.whitelist, bad));
  bans.configure(settings);

  for (const char *address : {"127.0.0.1", "::1", "::ffff:127.0.0.1", "192.168.1.77", "2001:db8:5:9::1"}) {
    for (int i = 0; i < 20; ++i) {
      EXPECT_FALSE(bans.record_failure(address, 1, "x")) << address;
    }
    EXPECT_FALSE(bans.is_blocked(address)) << address;
  }
  EXPECT_EQ(bans.active_count(), 0u);

  // Manual blocks follow the same rules, with a clear reason
  EXPECT_NE(bans.add_manual("127.0.0.1", 60, "x", nullptr).find("loopback"), std::string::npos);
  EXPECT_NE(bans.add_manual("192.168.1.77", 60, "x", nullptr).find("whitelist"), std::string::npos);
  EXPECT_NE(bans.add_manual("nope", 60, "x", nullptr), "");
  EXPECT_NE(bans.add_manual("203.0.113.5", 99999999, "x", nullptr), "");
  EXPECT_EQ(bans.active_count(), 0u);
}

TEST(SecurityBans, Ipv6IsCountedPerPrefix) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 60, 60));

  EXPECT_FALSE(bans.record_failure("2001:db8:1:2::1", 1, "x"));
  EXPECT_FALSE(bans.record_failure("2001:db8:1:2:aaaa::2", 1, "x"));
  auto ban = bans.record_failure("2001:db8:1:2:bbbb::3", 1, "x");
  ASSERT_TRUE(ban);
  EXPECT_EQ(ban->display, "2001:db8:1:2::/64");

  EXPECT_TRUE(bans.is_blocked("2001:db8:1:2:ffff:ffff:ffff:ffff"));
  EXPECT_FALSE(bans.is_blocked("2001:db8:1:3::1"));

  // Another address of the same /64 can be lifted by either spelling
  EXPECT_TRUE(bans.remove("2001:db8:1:2::/64", nullptr));
  EXPECT_FALSE(bans.is_blocked("2001:db8:1:2::1"));
}

TEST(SecurityBans, ManualBlock) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(10, 60, 1800));

  sl::ban_t added;
  EXPECT_EQ(bans.add_manual("203.0.113.9", -1, "scanner\nline", &added), "");
  EXPECT_TRUE(added.manual);
  EXPECT_EQ(added.reason, "scanner line");
  EXPECT_EQ(added.until_ms, clock.now + 1'800'000);
  EXPECT_TRUE(bans.is_blocked("203.0.113.9"));

  EXPECT_EQ(bans.add_manual("203.0.113.10", 0, "", &added), "");
  EXPECT_EQ(added.until_ms, 0);
  EXPECT_EQ(bans.add_manual("203.0.113.11", 120, "", &added), "");
  EXPECT_EQ(added.until_ms, clock.now + 120'000);
  EXPECT_EQ(bans.active_count(), 3u);

  auto settings = ban_settings();
  settings.enabled = false;
  bans.configure(settings);
  EXPECT_NE(bans.add_manual("203.0.113.12", 60, "", nullptr), "");
  EXPECT_FALSE(bans.is_blocked("203.0.113.9"));
}

TEST(SecurityBans, DisabledNeverBlocks) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  auto settings = ban_settings(3);
  settings.enabled = false;
  bans.configure(settings);
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(bans.record_failure("203.0.113.5", 1, "x"));
  }
  EXPECT_FALSE(bans.is_blocked("203.0.113.5"));
}

TEST(SecurityBans, PersistenceRoundTrip) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 60, 600));
  for (int i = 0; i < 3; ++i) {
    bans.record_failure("203.0.113.5", 1, "pair_failure");
  }
  bans.add_manual("2001:db8:1:2::77", 0, "manual block", nullptr);
  bans.add_manual("203.0.113.30", 30, "short", nullptr);

  auto dir = make_temp_dir("bans_round_trip");
  {
    std::ofstream out(dir / "security_bans.json");
    out << bans.to_json().dump();
  }
  json stored;
  {
    std::ifstream in(dir / "security_bans.json");
    stored = json::parse(in);
  }

  // Half a minute later the short block has run out
  clock.now += 31'000;
  sl::ban_manager_t restored {clock.fn()};
  restored.configure(ban_settings(3, 60, 600));
  restored.load_json(stored);

  EXPECT_TRUE(restored.is_blocked("203.0.113.5"));
  EXPECT_TRUE(restored.is_blocked("2001:db8:1:2::1"));
  EXPECT_FALSE(restored.is_blocked("203.0.113.30"));
  auto list = restored.list();
  ASSERT_EQ(list.size(), 2u);
  for (const auto &ban : list) {
    if (ban.manual) {
      EXPECT_EQ(ban.reason, "manual block");
      EXPECT_EQ(ban.until_ms, 0);
      EXPECT_EQ(ban.display, "2001:db8:1:2::/64");
    } else {
      EXPECT_EQ(ban.reason, "pair_failure");
      EXPECT_EQ(ban.failures, 3);
    }
  }
}

TEST(SecurityBans, StoredEntriesAreValidated) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings());
  auto future = sl::format_time(clock.now + 60'000);
  auto past = sl::format_time(clock.now - 60'000);
  bans.load_json(json::parse(R"({"bans":[
    {"ip":"203.0.113.1","since":")" + past + R"(","until":")" + future + R"(","reason":"a\nb"},
    {"ip":"203.0.113.2","since":")" + past + R"(","until":")" + past + R"("},
    {"ip":"127.0.0.1","since":")" + past + R"(","until":null},
    {"ip":"not an ip","since":")" + past + R"("},
    {"ip":"203.0.113.3"},
    "junk"
  ]})"));
  auto list = bans.list();
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0].display, "203.0.113.1");
  EXPECT_EQ(list[0].reason, "a b");

  bans.load_json(json("not an object"));
  EXPECT_TRUE(bans.list().empty());
}

TEST(SecurityBlocked, AttemptsAreAggregatedPerMinute) {
  sl::blocked_aggregator_t aggregator;
  for (int i = 0; i < 500; ++i) {
    aggregator.note("203.0.113.5", "https", 1000 + i);
  }
  aggregator.note("203.0.113.5", "webui", 1000);
  aggregator.note("203.0.113.6", "https", 30'000);

  EXPECT_TRUE(aggregator.take_due(40'000, false).empty());
  auto due = aggregator.take_due(61'500, false);
  ASSERT_EQ(due.size(), 2u);
  std::uint64_t total = 0;
  for (const auto &entry : due) {
    total += entry.count;
  }
  EXPECT_EQ(total, 501u);

  // The newer entry is still collecting and is handed out on shutdown
  auto rest = aggregator.take_due(61'500, true);
  ASSERT_EQ(rest.size(), 1u);
  EXPECT_EQ(rest[0].ip, "203.0.113.6");
  EXPECT_TRUE(aggregator.take_due(1'000'000, true).empty());
}

// ---------------------------------------------------------------------------------------------
// Process wide log
// ---------------------------------------------------------------------------------------------

class SecurityLogProcess: public ::testing::Test {
protected:
  void TearDown() override {
    sl::shutdown();
  }

  static sl::settings_t settings(int threshold = 10) {
    sl::settings_t result;
    result.ban = ban_settings(threshold, 600, 1800);
    return result;
  }

  static json events(const std::string &query_type = {}) {
    sl::filter_t filter;
    if (!query_type.empty()) {
      filter.types = {query_type};
    }
    return sl::events_json(filter);
  }
};

TEST_F(SecurityLogProcess, EventsAreSanitisedStoredAndWritten) {
  auto dir = make_temp_dir("process_events");
  sl::init(settings(), dir);

  sl::record(sl::type::launch, sl::result::ok, "::ffff:192.0.2.7", sl::service::https, {.client_uuid = "u-1", .client_name = "Phone\r\nFAKE 2026 pair_success", .app_id = "5", .app_name = "Desk\ttop", .detail = std::string_view {"d\0e", 3}});
  sl::flush();

  auto list = events("launch");
  ASSERT_EQ(list["total"], 1);
  const auto &event = list["events"][0];
  EXPECT_EQ(event["ip"], "192.0.2.7");
  EXPECT_EQ(event["service"], "https");
  EXPECT_EQ(event["client_name"], "Phone  FAKE 2026 pair_success");
  EXPECT_EQ(event["app_name"], "Desk top");
  EXPECT_EQ(event["detail"], "d e");
  EXPECT_EQ(event["result"], "ok");
  EXPECT_TRUE(sl::parse_time(event["ts"].get<std::string>()));

  // One JSON object per line in the file, nothing else
  std::ifstream in(dir / "security_events.log");
  std::string line;
  int lines = 0;
  while (std::getline(in, line)) {
    ++lines;
    EXPECT_TRUE(json::accept(line)) << line;
    EXPECT_EQ(line.find('\r'), std::string::npos);
  }
  EXPECT_EQ(lines, 1);
}

TEST_F(SecurityLogProcess, EventsSurviveARestart) {
  auto dir = make_temp_dir("process_restart");
  sl::init(settings(), dir);
  for (int i = 0; i < 25; ++i) {
    sl::record(sl::type::resume, sl::result::ok, "192.0.2.7", sl::service::https, {.client_name = "Phone"});
  }
  sl::flush();
  sl::shutdown();

  sl::init(settings(), dir);
  EXPECT_EQ(events("resume")["total"], 25);

  sl::clear_events("192.0.2.1");
  sl::flush();
  EXPECT_EQ(events("resume")["total"], 0);
  // Only the record that the log was cleared is left, and it is written again
  EXPECT_EQ(events()["total"], 1);
  EXPECT_EQ(events("events_cleared")["total"], 1);
  sl::shutdown();
  sl::init(settings(), dir);
  EXPECT_EQ(events("resume")["total"], 0);
  EXPECT_EQ(events("events_cleared")["total"], 1);
}

TEST_F(SecurityLogProcess, FailuresBlockTheAddressAndBlocksPersist) {
  auto dir = make_temp_dir("process_ban");
  sl::init(settings(10), dir);

  // Pair failures weigh 2: the fifth one reaches the threshold of 10
  for (int i = 0; i < 4; ++i) {
    sl::record(sl::type::pair_failure, sl::result::fail, "203.0.113.5", sl::service::http, {.detail = "wrong PIN"});
  }
  EXPECT_FALSE(sl::is_blocked("203.0.113.5"));
  EXPECT_FALSE(sl::any_blocked());
  sl::record(sl::type::pair_failure, sl::result::fail, "203.0.113.5", sl::service::http, {.detail = "wrong PIN"});
  EXPECT_TRUE(sl::is_blocked("203.0.113.5"));
  EXPECT_TRUE(sl::is_blocked(boost::asio::ip::make_address("::ffff:203.0.113.5")));
  EXPECT_TRUE(sl::any_blocked());
  EXPECT_FALSE(sl::is_blocked("203.0.113.6"));

  auto bans = sl::bans_json();
  ASSERT_EQ(bans["bans"].size(), 1u);
  EXPECT_EQ(bans["bans"][0]["ip"], "203.0.113.5");
  EXPECT_EQ(bans["bans"][0]["manual"], false);
  EXPECT_EQ(bans["bans"][0]["reason"], "pair_failure");
  EXPECT_TRUE(bans["bans"][0]["until"].is_string());
  EXPECT_EQ(events("ban_added")["total"], 1);
  EXPECT_EQ(sl::stats_json(24)["active_bans"], 1);

  // Successes and denied web requests do not count
  for (int i = 0; i < 50; ++i) {
    sl::record(sl::type::launch, sl::result::ok, "203.0.113.7", sl::service::https);
    sl::record(sl::type::webui_denied, sl::result::denied, "203.0.113.7", sl::service::webui);
  }
  EXPECT_FALSE(sl::is_blocked("203.0.113.7"));

  // The block is in the file and survives a restart
  sl::shutdown();
  EXPECT_TRUE(fs::exists(dir / "security_bans.json"));
  sl::init(settings(10), dir);
  EXPECT_TRUE(sl::is_blocked("203.0.113.5"));
  EXPECT_EQ(events("ban_added")["total"], 1);

  // An administrator lifts it
  EXPECT_EQ(sl::unban("203.0.113.5", "192.0.2.1"), "");
  EXPECT_FALSE(sl::is_blocked("203.0.113.5"));
  EXPECT_EQ(events("ban_removed")["total"], 1);
  EXPECT_NE(sl::unban("203.0.113.5", "192.0.2.1"), "");
  EXPECT_NE(sl::unban("nonsense", "192.0.2.1"), "");
}

TEST_F(SecurityLogProcess, ManualBlocksAndTheHandler) {
  auto dir = make_temp_dir("process_manual");
  auto config = settings();
  std::string bad;
  ASSERT_TRUE(sl::parse_whitelist("198.51.100.0/24", config.ban.whitelist, bad));
  sl::init(config, dir);

  std::vector<std::string> notified;
  sl::set_ban_handler([&](const sl::ban_t &ban) {
    notified.push_back(ban.display);
  });

  EXPECT_EQ(sl::ban("203.0.113.9", 3600, "scanner", "192.0.2.1"), "");
  EXPECT_TRUE(sl::is_blocked("203.0.113.9"));
  ASSERT_EQ(notified.size(), 1u);
  EXPECT_EQ(notified[0], "203.0.113.9");

  EXPECT_NE(sl::ban("127.0.0.1", 60, "x", "192.0.2.1").find("loopback"), std::string::npos);
  EXPECT_NE(sl::ban("198.51.100.4", 60, "x", "192.0.2.1").find("whitelist"), std::string::npos);
  EXPECT_NE(sl::ban("garbage", 60, "x", "192.0.2.1"), "");
  EXPECT_NE(sl::ban("::ffff:192.0.2.1", 60, "x", "192.0.2.1").find("you are using"), std::string::npos);
  EXPECT_FALSE(sl::is_blocked("192.0.2.1"));

  auto added = events("ban_added");
  ASSERT_EQ(added["total"], 1);
  EXPECT_NE(added["events"][0]["detail"].get<std::string>().find("manual=true"), std::string::npos);

  // The whitelisted address never gets blocked by failures either
  for (int i = 0; i < 100; ++i) {
    sl::record(sl::type::webui_login_failure, sl::result::fail, "198.51.100.4", sl::service::webui);
    sl::record(sl::type::webui_login_failure, sl::result::fail, "127.0.0.1", sl::service::webui);
  }
  EXPECT_FALSE(sl::is_blocked("198.51.100.4"));
  EXPECT_FALSE(sl::is_blocked("127.0.0.1"));
  sl::set_ban_handler(nullptr);
}

TEST_F(SecurityLogProcess, FloodingEventsAreRateLimitedButStillCounted) {
  auto dir = make_temp_dir("process_flood");
  sl::init(settings(1000), dir);

  for (int i = 0; i < 200; ++i) {
    sl::record(sl::type::webui_denied, sl::result::denied, "203.0.113.5", sl::service::webui, {.detail = "no valid session"});
    sl::record(sl::type::cert_rejected, sl::result::fail, "203.0.113.5", sl::service::https);
  }
  // One entry per type and address in the interval, not one per request
  EXPECT_EQ(events("webui_denied")["total"], 1);
  EXPECT_EQ(events("cert_rejected")["total"], 1);
}

TEST_F(SecurityLogProcess, NewClientsAreNotLockedOutBeforePairing) {
  auto dir = make_temp_dir("process_pairing");
  sl::init(settings(10), dir);

  // A client that is not paired yet (or was unpaired) keeps trying HTTPS with its own certificate
  for (int i = 0; i < 200; ++i) {
    sl::record(sl::type::cert_rejected, sl::result::fail, "203.0.113.5", sl::service::https, {.detail = "certificate verify failed"});
    sl::record(sl::type::permission_denied, sl::result::denied, "203.0.113.5", sl::service::https, {.client_name = "Phone", .detail = "app asset"});
  }
  EXPECT_FALSE(sl::is_blocked("203.0.113.5"));

  // The pairing itself still works, and a handful of mistyped PINs stay below the threshold
  sl::record(sl::type::pair_request, sl::result::ok, "203.0.113.5", sl::service::http, {.client_name = "Phone"});
  sl::record(sl::type::pair_failure, sl::result::fail, "203.0.113.5", sl::service::http, {.client_name = "Phone", .detail = "wrong PIN"});
  sl::record(sl::type::pair_request, sl::result::ok, "203.0.113.5", sl::service::http, {.client_name = "Phone"});
  sl::record(sl::type::pair_success, sl::result::ok, "203.0.113.5", sl::service::http, {.client_name = "Phone"});
  EXPECT_FALSE(sl::is_blocked("203.0.113.5"));

  // Real brute force is still stopped
  for (int i = 0; i < 5; ++i) {
    sl::record(sl::type::pair_failure, sl::result::fail, "203.0.113.6", sl::service::http, {.detail = "wrong PIN"});
  }
  EXPECT_TRUE(sl::is_blocked("203.0.113.6"));
}

TEST_F(SecurityLogProcess, RotatingAddressesStayBounded) {
  auto dir = make_temp_dir("process_rotation");
  sl::init(settings(3), dir);

  // Far more addresses than the counter and rate tables hold: nothing grows without a bound and counting keeps working
  for (int i = 0; i < 30000; ++i) {
    const auto address = "10." + std::to_string((i >> 16) & 255) + "." + std::to_string((i >> 8) & 255) + "." + std::to_string(i & 255);
    sl::record(sl::type::webui_denied, sl::result::denied, address, sl::service::webui);
    sl::record(sl::type::rtsp_rejected, sl::result::fail, address, sl::service::rtsp);
  }
  EXPECT_LE(events()["total"].get<std::size_t>(), sl::ring_capacity);
  for (int i = 0; i < 3; ++i) {
    sl::record(sl::type::rtsp_rejected, sl::result::fail, "198.51.100.77", sl::service::rtsp);
  }
  EXPECT_TRUE(sl::is_blocked("198.51.100.77"));
}

TEST(SecurityBans, LoopbackSpellingsAreNeverBanned) {
  fake_clock_t clock;
  sl::ban_manager_t bans {clock.fn()};
  bans.configure(ban_settings(3, 60, 60));
  for (const char *address : {"127.0.0.1", "127.255.255.254", "::1", "::ffff:127.0.0.1", "0:0:0:0:0:ffff:7f00:1"}) {
    EXPECT_NE(bans.add_manual(address, 60, "x", nullptr), "") << address;
    for (int i = 0; i < 10; ++i) {
      EXPECT_FALSE(bans.record_failure(address, 2, "x")) << address;
    }
    EXPECT_FALSE(bans.is_blocked(address)) << address;
  }
  EXPECT_EQ(bans.active_count(), 0u);

  // A v4-mapped spelling and the plain address are one and the same block
  ASSERT_EQ(bans.add_manual("::ffff:203.0.113.5", 60, "x", nullptr), "");
  EXPECT_TRUE(bans.is_blocked("203.0.113.5"));
  EXPECT_TRUE(bans.remove("203.0.113.5", nullptr));
  EXPECT_FALSE(bans.is_blocked("::ffff:203.0.113.5"));
}

TEST(SecurityAddress, MalformedWhitelistEntriesNeverMatchEverything) {
  std::vector<sl::cidr_t> list;
  std::string bad;
  EXPECT_FALSE(sl::parse_whitelist("0.0.0/0", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("*", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("any", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("10.0.0.1/abc", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("10.0.0.1/", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("0.0.0.0/0", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("::/0", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("10.1", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("1.2.3.4.5", list, bad));
  EXPECT_FALSE(sl::parse_whitelist("1.2.3.256", list, bad));
  EXPECT_TRUE(list.empty());

  // Short spellings are not addresses for the block list either
  EXPECT_FALSE(sl::group_of("10.1"));
  EXPECT_FALSE(sl::group_of("0.0.0"));
  EXPECT_TRUE(sl::group_of("10.1.2.3"));
}

TEST_F(SecurityLogProcess, DisabledLogStillBlocks) {
  auto dir = make_temp_dir("process_disabled");
  auto config = settings(3);
  config.log_enabled = false;
  sl::init(config, dir);
  for (int i = 0; i < 3; ++i) {
    sl::record(sl::type::rtsp_rejected, sl::result::fail, "203.0.113.5", sl::service::rtsp);
  }
  EXPECT_TRUE(sl::is_blocked("203.0.113.5"));
  EXPECT_EQ(events()["total"], 0);

  config = settings();
  config.ban.enabled = false;
  sl::init(config, make_temp_dir("process_disabled2"));
  for (int i = 0; i < 50; ++i) {
    sl::record(sl::type::pair_failure, sl::result::fail, "203.0.113.5", sl::service::https);
  }
  EXPECT_FALSE(sl::is_blocked("203.0.113.5"));
}

// ---------------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------------

TEST(SecurityConfig, RangesAreValidated) {
  auto error = [](const char *key, const json &value) {
    return confighttp::validate_config_object({{key, value}}).error;
  };

  EXPECT_TRUE(error("security_log_max_mb", 10).empty());
  EXPECT_TRUE(error("security_log_max_mb", "100").empty());
  EXPECT_FALSE(error("security_log_max_mb", 0).empty());
  EXPECT_FALSE(error("security_log_max_mb", 101).empty());

  EXPECT_TRUE(error("ban_threshold", 3).empty());
  EXPECT_TRUE(error("ban_threshold", 1000).empty());
  EXPECT_FALSE(error("ban_threshold", 2).empty());
  EXPECT_FALSE(error("ban_threshold", 1001).empty());
  EXPECT_FALSE(error("ban_threshold", "many").empty());

  EXPECT_TRUE(error("ban_window_seconds", 10).empty());
  EXPECT_TRUE(error("ban_window_seconds", 86400).empty());
  EXPECT_FALSE(error("ban_window_seconds", 9).empty());
  EXPECT_FALSE(error("ban_window_seconds", 86401).empty());

  EXPECT_TRUE(error("ban_duration_seconds", 0).empty());
  EXPECT_TRUE(error("ban_duration_seconds", 2592000).empty());
  EXPECT_FALSE(error("ban_duration_seconds", -1).empty());
  EXPECT_FALSE(error("ban_duration_seconds", 2592001).empty());

  EXPECT_TRUE(error("ban_whitelist", "192.168.1.5, 10.0.0.0/8").empty());
  EXPECT_TRUE(error("ban_whitelist", json::array({"192.168.1.5", "10.0.0.0/8"})).empty());
  EXPECT_TRUE(error("ban_whitelist", "").empty());
  EXPECT_FALSE(error("ban_whitelist", "192.168.1.5, nope").empty());
  EXPECT_FALSE(error("ban_whitelist", "10.0.0.0/99").empty());

  // The options are known to the validator
  auto result = confighttp::validate_config_object({{"ban_enabled", "enabled"}, {"security_log", "disabled"}, {"ban_threshold", 10}});
  EXPECT_TRUE(result.error.empty());
  EXPECT_TRUE(result.unknown_keys.empty());
}
