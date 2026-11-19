/**
 * @file src/update_check.h
 * @brief Declarations for the background check for new NyxPlay Host releases.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Looks for new releases on GitHub (GitLab mirror as fallback) and announces them once in the system tray.
 */
namespace update_check {
  /// Upper bound for a downloaded release list.
  constexpr std::size_t max_body_bytes = 1024 * 1024;

  /// Upper bound for the number of releases that are inspected.
  constexpr std::size_t max_releases = 50;

  constexpr std::string_view github_releases_url = "https://api.github.com/repos/siberanka/NyxPlay/releases?per_page=20";
  constexpr std::string_view gitlab_releases_url = "https://gitlab.com/api/v4/projects/siberanka%2FNyxPlay/releases?per_page=20";
  constexpr std::string_view releases_page = "https://github.com/siberanka/NyxPlay/releases";

  /// The same version is announced at most once in this time span.
  constexpr std::chrono::hours reannounce_interval {7 * 24};

  /**
   * @brief A calendar version: `YYYY.MM.DD.BUILD` with an optional pre-release rank.
   */
  struct version_t {
    std::array<long, 4> parts {};
    long pre_rank = pre_rank_final;  ///< Lower than #pre_rank_final for alpha/beta/rc builds.

    static constexpr long pre_rank_final = 0x7fffffffL;
  };

  /**
   * @brief Parse a version or release tag (`[host-][v]YYYY.MM.DD.BUILD[-beta.N]`).
   * @param text The text to parse.
   * @param strict When true the text must be exactly a release tag. When false (the running
   *               version) a trailing `.<commit>` / `.dirty` suffix of a local build is ignored.
   * @return The version, or nothing when the text is not a version.
   */
  std::optional<version_t> parse_version(std::string_view text, bool strict);

  /**
   * @brief Compare two versions.
   * @return -1, 0 or 1.
   */
  int compare(const version_t &a, const version_t &b);

  /**
   * @brief A usable (non-draft, host) release.
   */
  struct release_t {
    std::string tag;
    std::string url;  ///< Always a link into the project repository.
    version_t version;
    bool prerelease = false;
  };

  /**
   * @brief Return the link when it points into the project repository on GitHub or GitLab, otherwise the releases page.
   */
  std::string safe_release_url(std::string_view url);

  /**
   * @brief Parse a GitHub or GitLab release list.
   *
   * Drafts, malformed entries and tags that are not host release tags are dropped.
   * @param body The response body.
   * @return The releases, or nothing when the body is not a JSON array.
   */
  std::optional<std::vector<release_t>> parse_releases(std::string_view body);

  /**
   * @brief Pick the release that should be announced.
   * @param releases The usable releases.
   * @param current The running version.
   * @param include_prereleases Whether pre-releases may be announced (`notify_pre_releases`).
   * @return The newest release that is newer than #current, if any.
   */
  std::optional<release_t> select_update(const std::vector<release_t> &releases, const version_t &current, bool include_prereleases);

  enum class status_t {
    up_to_date,
    update_available,
    failed,  ///< No source could be reached or understood.
  };

  struct result_t {
    status_t status = status_t::failed;
    release_t release;  ///< Valid when the status is #status_t::update_available.
  };

  /// Downloads a URL. Returns nothing on any failure.
  using fetch_fn = std::function<std::optional<std::string>(std::string_view url)>;

  /**
   * @brief Run one check. GitHub is asked first; the GitLab mirror is only asked when GitHub failed or
   *        did not return a usable release.
   * @param fetch The download function.
   * @param current_version The running version text.
   * @param include_prereleases Whether pre-releases may be announced.
   */
  result_t check(const fetch_fn &fetch, std::string_view current_version, bool include_prereleases);

  /**
   * @brief Whether a release should be announced now.
   * @param last_announced_tag The tag announced last, empty if none.
   * @param last_announced_at When it was announced.
   * @param tag The candidate.
   * @param now The current time.
   */
  bool should_announce(
    const std::string &last_announced_tag,
    std::chrono::system_clock::time_point last_announced_at,
    const std::string &tag,
    std::chrono::system_clock::time_point now
  );

  /**
   * @brief Download a URL over HTTPS (certificate verification on, at most 3 redirects, bounded size).
   * @param url The URL.
   * @param connect_timeout The connect timeout.
   * @param total_timeout The timeout of the whole transfer.
   * @param cancel Optional flag that aborts the transfer as soon as it is set.
   * @return The body, or nothing on any failure.
   */
  std::optional<std::string> fetch(
    std::string_view url,
    std::chrono::seconds connect_timeout = std::chrono::seconds {10},
    std::chrono::seconds total_timeout = std::chrono::seconds {20},
    const std::atomic<bool> *cancel = nullptr
  );

  /**
   * @brief Start the background checker (first check after about two minutes, then every 24 hours).
   *        Does nothing when `update_check` is disabled.
   */
  void start();

  /**
   * @brief Stop the background checker and wait for it. Safe to call when it was never started.
   */
  void stop();
}  // namespace update_check
