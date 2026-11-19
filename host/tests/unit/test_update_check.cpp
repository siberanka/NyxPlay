/**
 * @file tests/unit/test_update_check.cpp
 * @brief Test src/update_check.*.
 */
// test imports
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <map>

// local imports
#include <src/update_check.h>

using namespace std::literals;
using namespace update_check;

namespace {
  // Trimmed from the real GitHub response of the release list
  constexpr auto github_sample = R"json([
    {
      "url": "https://api.github.com/repos/siberanka/NyxPlay/releases/451169359",
      "html_url": "https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.19.1",
      "id": 451169359,
      "tag_name": "v2026.11.19.1",
      "name": "NyxPlay 2026.11.19.1",
      "draft": false,
      "prerelease": false,
      "published_at": "2026-11-19T10:16:59Z",
      "body": "notes"
    },
    {
      "html_url": "https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.16.1",
      "tag_name": "v2026.11.16.1",
      "name": "NyxPlay 2026.11.16.1",
      "draft": false,
      "prerelease": false,
      "body": "notes"
    }
  ])json"sv;

  // Shape of the GitLab releases API
  constexpr auto gitlab_sample = R"json([
    {
      "tag_name": "v2026.12.1.1",
      "name": "NyxPlay 2026.12.1.1",
      "description": "notes",
      "upcoming_release": false,
      "_links": {"self": "https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.12.1.1"}
    }
  ])json"sv;

  std::string release_json(std::string_view tag, bool prerelease = false, bool draft = false, std::string_view url = "https://github.com/siberanka/NyxPlay/releases/tag/x") {
    return R"({"tag_name":")" + std::string {tag} + R"(","draft":)" + (draft ? "true" : "false") + R"(,"prerelease":)" + (prerelease ? "true" : "false") + R"(,"html_url":")" + std::string {url} + R"("})";
  }

  std::string list_json(std::initializer_list<std::string> entries) {
    std::string out = "[";
    for (const auto &entry : entries) {
      if (out.size() > 1) {
        out += ',';
      }
      out += entry;
    }
    return out + ']';
  }

  version_t version(std::string_view text) {
    const auto parsed = parse_version(text, false);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(version_t {});
  }

  /// Fake network: maps a URL to a body; unknown URLs fail. Records the requested URLs.
  struct fake_net_t {
    std::map<std::string, std::string, std::less<>> bodies;
    std::vector<std::string> requested;

    fetch_fn fn() {
      return [this](std::string_view url) -> std::optional<std::string> {
        requested.emplace_back(url);
        if (const auto it = bodies.find(url); it != bodies.end()) {
          return it->second;
        }
        return std::nullopt;
      };
    }
  };
}  // namespace

struct ParseVersionTest: testing::TestWithParam<std::tuple<std::string, bool, bool, std::array<long, 4>>> {};

TEST_P(ParseVersionTest, Run) {
  const auto &[text, strict, valid, parts] = GetParam();
  const auto parsed = parse_version(text, strict);
  ASSERT_EQ(parsed.has_value(), valid) << text;
  if (valid) {
    EXPECT_EQ(parsed->parts, parts);
  }
}

INSTANTIATE_TEST_SUITE_P(
  UpdateCheck,
  ParseVersionTest,
  testing::Values(
    std::make_tuple("v2026.11.19.1", true, true, std::array<long, 4> {2026, 11, 19, 1}),
    std::make_tuple("2026.11.19.1", true, true, std::array<long, 4> {2026, 11, 19, 1}),
    std::make_tuple("host-v2026.12.1.10", true, true, std::array<long, 4> {2026, 12, 1, 10}),
    std::make_tuple("v2026.1.2.3-beta.4", true, true, std::array<long, 4> {2026, 1, 2, 3}),
    std::make_tuple("v2026.1.2.3-rc", true, true, std::array<long, 4> {2026, 1, 2, 3}),
    std::make_tuple("2026.11.19.1.dirty", false, true, std::array<long, 4> {2026, 11, 19, 1}),
    std::make_tuple("2026.11.19.1.abc1234.dirty", false, true, std::array<long, 4> {2026, 11, 19, 1}),
    std::make_tuple("2026.11.19.1-beta.2.dirty", false, true, std::array<long, 4> {2026, 11, 19, 1}),
    std::make_tuple("2026.11.19", false, true, std::array<long, 4> {2026, 11, 19, 0}),
    std::make_tuple("2026.11.19.1.dirty", true, false, std::array<long, 4> {}),
    std::make_tuple("2026.11.19", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026.11.19.1x", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026.11.19.1-gamma.1", true, false, std::array<long, 4> {}),
    std::make_tuple("v26.11.19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("v20261.11.19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026.111.19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026.11.19.12345678901", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026..19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("v2026.-1.19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("nightly", true, false, std::array<long, 4> {}),
    std::make_tuple("android-v2026.11.19.1", true, false, std::array<long, 4> {}),
    std::make_tuple("", true, false, std::array<long, 4> {}),
    std::make_tuple("dev", false, false, std::array<long, 4> {})
  )
);

TEST(UpdateCheckCompare, CalendarVersions) {
  EXPECT_GT(compare(version("2026.12.1.1"), version("2026.11.30.9")), 0);
  EXPECT_LT(compare(version("2026.11.30.9"), version("2026.12.1.1")), 0);
  EXPECT_GT(compare(version("2026.11.19.10"), version("2026.11.19.2")), 0);
  EXPECT_EQ(compare(version("2026.11.19.1"), version("2026.11.19.1")), 0);
  EXPECT_EQ(compare(version("2026.11.19"), version("2026.11.19.0")), 0);
}

TEST(UpdateCheckCompare, LocalBuildSuffixIsIgnored) {
  EXPECT_EQ(compare(version("2026.11.19.1.dirty"), version("2026.11.19.1")), 0);
  EXPECT_LT(compare(version("2026.11.19.1.abc1234.dirty"), version("2026.11.19.2")), 0);
}

TEST(UpdateCheckCompare, PreReleaseRanksBelowFinal) {
  EXPECT_LT(compare(version("2026.11.19.1-beta.3"), version("2026.11.19.1")), 0);
  EXPECT_GT(compare(version("2026.11.19.1-beta.2"), version("2026.11.19.1-beta.1")), 0);
  EXPECT_GT(compare(version("2026.11.19.1-rc.1"), version("2026.11.19.1-beta.9")), 0);
  EXPECT_GT(compare(version("2026.11.20.1-beta.1"), version("2026.11.19.1")), 0);
}

struct SafeUrlTest: testing::TestWithParam<std::tuple<std::string, bool>> {};

TEST_P(SafeUrlTest, Run) {
  const auto &[url, trusted] = GetParam();
  EXPECT_EQ(safe_release_url(url), trusted ? url : std::string {releases_page}) << url;
}

INSTANTIATE_TEST_SUITE_P(
  UpdateCheck,
  SafeUrlTest,
  testing::Values(
    std::make_tuple("https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.19.1", true),
    std::make_tuple("https://github.com/siberanka/NyxPlay", true),
    std::make_tuple("https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.12.1.1", true),
    std::make_tuple("http://github.com/siberanka/NyxPlay/releases", false),
    std::make_tuple("https://evil.example/siberanka/NyxPlay/releases", false),
    std::make_tuple("https://github.com.evil.example/siberanka/NyxPlay/x", false),
    std::make_tuple("https://github.com@evil.example/siberanka/NyxPlay/x", false),
    std::make_tuple("https://github.com:8443/siberanka/NyxPlay/x", false),
    std::make_tuple("https://github.com/siberanka/NyxPlayEvil/x", false),
    std::make_tuple("https://github.com/someone/else/x", false),
    std::make_tuple("https://github.com/siberanka/NyxPlay/../../evil/x", false),
    std::make_tuple("https://github.com/siberanka/NyxPlay/%2e%2e/evil", false),
    std::make_tuple("https://github.com/siberanka/NyxPlay/x\\y", false),
    std::make_tuple("https://github.com/siberanka/NyxPlay/x y", false),
    std::make_tuple("https://github.com/siberanka/NyxPlay//x", false),
    std::make_tuple("javascript:alert(1)", false),
    std::make_tuple("", false)
  )
);

TEST(UpdateCheckParse, GitHubSample) {
  const auto releases = parse_releases(github_sample);
  ASSERT_TRUE(releases.has_value());
  ASSERT_EQ(releases->size(), 2U);
  EXPECT_EQ((*releases)[0].tag, "v2026.11.19.1");
  EXPECT_FALSE((*releases)[0].prerelease);
  EXPECT_EQ((*releases)[0].url, "https://github.com/siberanka/NyxPlay/releases/tag/v2026.11.19.1");
}

TEST(UpdateCheckParse, GitLabSample) {
  const auto releases = parse_releases(gitlab_sample);
  ASSERT_TRUE(releases.has_value());
  ASSERT_EQ(releases->size(), 1U);
  EXPECT_EQ((*releases)[0].tag, "v2026.12.1.1");
  EXPECT_EQ((*releases)[0].url, "https://gitlab.com/siberanka/NyxPlay/-/releases/v2026.12.1.1");
}

TEST(UpdateCheckParse, DropsDraftsAndJunkTags) {
  const auto body = list_json({
    release_json("v2027.1.1.1", false, true),
    release_json("nightly"),
    release_json("android-v2026.11.19.1"),
    release_json("v2026.11.19"),
    release_json("v2026.12.5.1-beta.1", true),
    release_json("v2026.12.1.1"),
    "null",
    "5",
    "\"text\"",
    R"({"tag_name":5})",
    "{}",
  });
  const auto releases = parse_releases(body);
  ASSERT_TRUE(releases.has_value());
  ASSERT_EQ(releases->size(), 2U);
  EXPECT_EQ((*releases)[0].tag, "v2026.12.5.1-beta.1");
  EXPECT_TRUE((*releases)[0].prerelease);
  EXPECT_EQ((*releases)[1].tag, "v2026.12.1.1");
  EXPECT_FALSE((*releases)[1].prerelease);
}

TEST(UpdateCheckParse, PreReleaseTagWithoutFlagIsPreRelease) {
  const auto releases = parse_releases(list_json({release_json("v2026.12.5.1-rc.1")}));
  ASSERT_TRUE(releases.has_value());
  ASSERT_EQ(releases->size(), 1U);
  EXPECT_TRUE((*releases)[0].prerelease);
}

TEST(UpdateCheckParse, UntrustedLinkIsReplaced) {
  const auto releases = parse_releases(list_json({release_json("v2026.12.1.1", false, false, "https://evil.example/x")}));
  ASSERT_TRUE(releases.has_value());
  ASSERT_EQ(releases->size(), 1U);
  EXPECT_EQ((*releases)[0].url, releases_page);
}

TEST(UpdateCheckParse, MalformedBodies) {
  EXPECT_FALSE(parse_releases(""));
  EXPECT_FALSE(parse_releases("<html>rate limited</html>"));
  EXPECT_FALSE(parse_releases(R"({"message":"API rate limit exceeded"})"));
  EXPECT_FALSE(parse_releases(R"([{"tag_name":"v2026.11.19.1")"));
  EXPECT_FALSE(parse_releases("\"text\""));
  EXPECT_FALSE(parse_releases(std::string("\x00\x01\xff\xfe", 4)));
  const auto empty = parse_releases("[]");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

TEST(UpdateCheckParse, OversizedBody) {
  std::string body = "[";
  while (body.size() <= max_body_bytes) {
    body += R"({"tag_name":"nightly","body":"filler filler filler"},)";
  }
  body += release_json("v2099.1.1.1") + "]";
  EXPECT_FALSE(parse_releases(body));
}

TEST(UpdateCheckParse, NumberOfInspectedEntriesIsBounded) {
  std::string body = "[";
  for (std::size_t i = 0; i < max_releases; ++i) {
    body += "{},";
  }
  body += release_json("v2099.1.1.1") + "]";
  const auto releases = parse_releases(body);
  ASSERT_TRUE(releases.has_value());
  EXPECT_TRUE(releases->empty());
}

TEST(UpdateCheckParse, DeeplyNestedJsonDoesNotCrash) {
  std::string body(100000, '[');
  EXPECT_NO_THROW(parse_releases(body));
}

TEST(UpdateCheckSelect, OlderEqualAndNewer) {
  const auto releases = *parse_releases(list_json({release_json("v2026.11.16.1"), release_json("v2026.11.19.1")}));
  EXPECT_FALSE(select_update(releases, version("2026.11.19.1"), false));
  EXPECT_FALSE(select_update(releases, version("2026.11.20.1"), false));
  EXPECT_FALSE(select_update(releases, version("2026.11.19.1.dirty"), false));

  const auto update = select_update(releases, version("2026.11.16.1"), false);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->tag, "v2026.11.19.1");
}

TEST(UpdateCheckSelect, ListOrderDoesNotMatter) {
  const auto releases = *parse_releases(list_json({release_json("v2026.11.16.1"), release_json("v2026.12.1.1"), release_json("v2026.11.30.9")}));
  const auto update = select_update(releases, version("2026.11.19.1"), false);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->tag, "v2026.12.1.1");
}

TEST(UpdateCheckSelect, PreReleasesFollowTheSetting) {
  const auto releases = *parse_releases(list_json({release_json("v2026.12.5.1-beta.2", true), release_json("v2026.12.1.1")}));
  const auto current = version("2026.11.19.1");

  const auto without = select_update(releases, current, false);
  ASSERT_TRUE(without.has_value());
  EXPECT_EQ(without->tag, "v2026.12.1.1");

  const auto with = select_update(releases, current, true);
  ASSERT_TRUE(with.has_value());
  EXPECT_EQ(with->tag, "v2026.12.5.1-beta.2");
}

TEST(UpdateCheckSelect, OnlyPreReleaseIsIgnoredByDefault) {
  const auto releases = *parse_releases(list_json({release_json("v2026.12.5.1-beta.1", true)}));
  EXPECT_FALSE(select_update(releases, version("2026.11.19.1"), false));
  EXPECT_TRUE(select_update(releases, version("2026.11.19.1"), true));
}

TEST(UpdateCheckSelect, PreReleaseOlderThanStableIsNotAnnounced) {
  const auto releases = *parse_releases(list_json({release_json("v2026.12.1.1"), release_json("v2026.11.25.1-beta.1", true)}));
  const auto update = select_update(releases, version("2026.11.19.1"), true);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->tag, "v2026.12.1.1");
}

TEST(UpdateCheckSelect, InstalledBetaOfSameVersionSeesFinalRelease) {
  const auto releases = *parse_releases(list_json({release_json("v2026.12.1.1")}));
  EXPECT_TRUE(select_update(releases, version("2026.12.1.1-beta.2"), false));
  EXPECT_FALSE(select_update(releases, version("2026.12.1.1"), false));
}

TEST(UpdateCheckCheck, GitHubAnswersAndMirrorIsNotAsked) {
  fake_net_t net;
  net.bodies[std::string {github_releases_url}] = std::string {github_sample};
  const auto result = check(net.fn(), "2026.11.16.1", false);
  EXPECT_EQ(result.status, status_t::update_available);
  EXPECT_EQ(result.release.tag, "v2026.11.19.1");
  ASSERT_EQ(net.requested.size(), 1U);
  EXPECT_EQ(net.requested[0], github_releases_url);
}

TEST(UpdateCheckCheck, UpToDateDoesNotAskTheMirror) {
  fake_net_t net;
  net.bodies[std::string {github_releases_url}] = std::string {github_sample};
  net.bodies[std::string {gitlab_releases_url}] = std::string {gitlab_sample};
  const auto result = check(net.fn(), "2026.11.19.1", false);
  EXPECT_EQ(result.status, status_t::up_to_date);
  EXPECT_EQ(net.requested.size(), 1U);
}

TEST(UpdateCheckCheck, FallsBackToGitLabWhenGitHubFails) {
  fake_net_t net;
  net.bodies[std::string {gitlab_releases_url}] = std::string {gitlab_sample};
  const auto result = check(net.fn(), "2026.11.19.1", false);
  EXPECT_EQ(result.status, status_t::update_available);
  EXPECT_EQ(result.release.tag, "v2026.12.1.1");
  ASSERT_EQ(net.requested.size(), 2U);
  EXPECT_EQ(net.requested[1], gitlab_releases_url);
}

TEST(UpdateCheckCheck, FallsBackWhenGitHubBodyIsUnusable) {
  for (const char *junk : {"<html>", R"({"message":"rate limited"})", "", "[]", "[{\"tag_name\":\"nightly\"}]"}) {
    fake_net_t net;
    net.bodies[std::string {github_releases_url}] = junk;
    net.bodies[std::string {gitlab_releases_url}] = std::string {gitlab_sample};
    const auto result = check(net.fn(), "2026.11.19.1", false);
    EXPECT_EQ(result.status, status_t::update_available) << junk;
    EXPECT_EQ(net.requested.size(), 2U) << junk;
  }
}

TEST(UpdateCheckCheck, FailedWhenNothingIsReachable) {
  fake_net_t net;
  EXPECT_EQ(check(net.fn(), "2026.11.19.1", false).status, status_t::failed);
  EXPECT_EQ(net.requested.size(), 2U);

  net.bodies[std::string {github_releases_url}] = "<html>";
  net.bodies[std::string {gitlab_releases_url}] = "not json";
  EXPECT_EQ(check(net.fn(), "2026.11.19.1", false).status, status_t::failed);
}

TEST(UpdateCheckCheck, EmptyReleaseListsAreNotAFailure) {
  fake_net_t net;
  net.bodies[std::string {github_releases_url}] = "[]";
  net.bodies[std::string {gitlab_releases_url}] = "[]";
  EXPECT_EQ(check(net.fn(), "2026.11.19.1", false).status, status_t::up_to_date);
}

TEST(UpdateCheckCheck, UnparsableRunningVersionNeverAsksTheNetwork) {
  fake_net_t net;
  net.bodies[std::string {github_releases_url}] = std::string {github_sample};
  EXPECT_EQ(check(net.fn(), "dev", false).status, status_t::failed);
  EXPECT_TRUE(net.requested.empty());
}

TEST(UpdateCheckCheck, ThrowingFetchIsAFailure) {
  const auto result = check([](std::string_view) -> std::optional<std::string> {
    throw std::runtime_error("network");
  }, "2026.11.19.1", false);
  EXPECT_EQ(result.status, status_t::failed);
}

TEST(UpdateCheckAnnounce, OncePerVersionAndWeek) {
  const auto now = std::chrono::system_clock::now();
  EXPECT_TRUE(should_announce("", {}, "v2026.12.1.1", now));
  EXPECT_FALSE(should_announce("v2026.12.1.1", now - 24h, "v2026.12.1.1", now));
  EXPECT_FALSE(should_announce("v2026.12.1.1", now - 6 * 24h, "v2026.12.1.1", now));
  EXPECT_TRUE(should_announce("v2026.12.1.1", now - 7 * 24h, "v2026.12.1.1", now));
  EXPECT_TRUE(should_announce("v2026.12.1.1", now - 1h, "v2026.12.2.1", now));
  // A clock that moved backwards must not silence the notice
  EXPECT_TRUE(should_announce("v2026.12.1.1", now + 48h, "v2026.12.1.1", now));
}

TEST(UpdateCheckFetch, UnreachableUrlFailsQuicklyWithoutThrowing) {
  const auto start = std::chrono::steady_clock::now();
  std::optional<std::string> body;
  EXPECT_NO_THROW(body = fetch("https://127.0.0.1:9/", 1s, 1s));
  EXPECT_FALSE(body.has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

TEST(UpdateCheckFetch, RefusesOtherSchemes) {
  EXPECT_FALSE(fetch("http://127.0.0.1:9/", 1s, 1s).has_value());
  EXPECT_FALSE(fetch("file:///C:/Windows/win.ini", 1s, 1s).has_value());
  EXPECT_FALSE(fetch("", 1s, 1s).has_value());
}

TEST(UpdateCheckFetch, CancelFlagAbortsImmediately) {
  const std::atomic<bool> cancel = true;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(fetch("https://127.0.0.1:9/", 5s, 5s, &cancel).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(UpdateCheckLifecycle, StopWithoutStartIsSafe) {
  EXPECT_NO_THROW(update_check::stop());
}
