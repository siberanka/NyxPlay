/**
 * @file src/security_log.h
 * @brief Declarations for the security event log and the automatic blocking of abusive addresses.
 *
 * The event log records who did what from which address (pairing, Web UI logins, app launches, streams,
 * rejected connections). The ban manager counts authentication failures per address and blocks an address
 * for a while when it keeps failing. Both are fed through record().
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>

namespace security_log {
  /// Number of events kept in memory.
  inline constexpr std::size_t ring_capacity = 5000;

  /// Limits applied to peer-controlled text before it is stored or logged.
  inline constexpr std::size_t max_name_bytes = 128;
  inline constexpr std::size_t max_detail_bytes = 240;
  inline constexpr std::size_t max_id_bytes = 64;

  namespace type {
    inline constexpr std::string_view pair_request = "pair_request";
    inline constexpr std::string_view pair_success = "pair_success";
    inline constexpr std::string_view pair_failure = "pair_failure";
    inline constexpr std::string_view pair_cancelled = "pair_cancelled";
    inline constexpr std::string_view cert_rejected = "cert_rejected";
    inline constexpr std::string_view webui_login_success = "webui_login_success";
    inline constexpr std::string_view webui_login_failure = "webui_login_failure";
    inline constexpr std::string_view webui_denied = "webui_denied";
    inline constexpr std::string_view permission_denied = "permission_denied";
    inline constexpr std::string_view launch = "launch";
    inline constexpr std::string_view resume = "resume";
    inline constexpr std::string_view cancel = "cancel";
    inline constexpr std::string_view app_exit = "app_exit";
    inline constexpr std::string_view stream_start = "stream_start";
    inline constexpr std::string_view stream_end = "stream_end";
    inline constexpr std::string_view rtsp_rejected = "rtsp_rejected";
    inline constexpr std::string_view control_rejected = "control_rejected";
    inline constexpr std::string_view unpair = "unpair";
    inline constexpr std::string_view client_update = "client_update";
    inline constexpr std::string_view admin_action = "admin_action";
    inline constexpr std::string_view ban_added = "ban_added";
    inline constexpr std::string_view ban_removed = "ban_removed";
    inline constexpr std::string_view connection_blocked = "connection_blocked";
    inline constexpr std::string_view events_cleared = "events_cleared";
  }  // namespace type

  namespace result {
    inline constexpr std::string_view ok = "ok";
    inline constexpr std::string_view fail = "fail";
    inline constexpr std::string_view denied = "denied";
    inline constexpr std::string_view blocked = "blocked";
  }  // namespace result

  namespace service {
    inline constexpr std::string_view http = "http";
    inline constexpr std::string_view https = "https";
    inline constexpr std::string_view webui = "webui";
    inline constexpr std::string_view rtsp = "rtsp";
    inline constexpr std::string_view control = "control";
    inline constexpr std::string_view host = "host";
  }  // namespace service

  /**
   * @brief One recorded event. The member order is the order of the JSON fields.
   */
  struct event_t {
    std::int64_t ts_ms = 0;  ///< Milliseconds since the Unix epoch (UTC).
    std::string type;
    std::string ip;
    std::string service;
    std::string client_uuid;
    std::string client_name;
    std::string app_id;
    std::string app_name;
    std::string result;
    std::string detail;
  };

  /// Optional descriptive fields of an event (all text is sanitised when the event is recorded).
  struct extra_t {
    std::string_view client_uuid;
    std::string_view client_name;
    std::string_view app_id;
    std::string_view app_name;
    std::string_view detail;
  };

  // ---------------------------------------------------------------------------------------------
  // Text, time and address helpers
  // ---------------------------------------------------------------------------------------------

  /**
   * @brief Make peer-controlled text safe to store and log.
   * @details Control characters (CR, LF, NUL, ...) become spaces, invalid UTF-8 and invisible
   *          direction/line separator characters are dropped, the result is trimmed and cut at
   *          `max_bytes` on a character boundary.
   */
  std::string sanitize_text(std::string_view text, std::size_t max_bytes);

  /// @return The canonical text of an address (IPv4-mapped IPv6 as IPv4, no scope id), empty if it is not one.
  std::string normalize_ip(std::string_view text);

  /// @return `YYYY-MM-DDTHH:MM:SS.mmmZ` (UTC).
  std::string format_time(std::int64_t ms);

  /// @brief Parse `YYYY-MM-DD[THH:MM[:SS[.fff]]][Z|+HH:MM|-HH:MM]` (no zone means UTC) into epoch milliseconds.
  std::optional<std::int64_t> parse_time(std::string_view text);

  std::int64_t system_now_ms();

  nlohmann::json to_json(const event_t &event);

  /// @brief Rebuild an event from JSON, sanitising every field. Returns nothing if the essentials are missing.
  std::optional<event_t> from_json(const nlohmann::json &json);

  // ---------------------------------------------------------------------------------------------
  // Event store
  // ---------------------------------------------------------------------------------------------

  struct filter_t {
    std::vector<std::string> types;  ///< Exact match, empty means any.
    std::string ip;  ///< Case-insensitive substring.
    std::string client;  ///< Case-insensitive substring of the client name or uuid.
    std::string app;  ///< Case-insensitive substring of the app name or id.
    std::string result;  ///< Exact match.
    std::optional<std::int64_t> since;
    std::optional<std::int64_t> until;
    std::size_t limit = 200;
    std::size_t offset = 0;
  };

  /**
   * @brief Read the filter of GET /api/security/events.
   * @param query Decoded query parameters (lower-case names).
   * @param error Receives the reason when the query is invalid.
   */
  std::optional<filter_t> parse_filter(const std::map<std::string, std::string> &query, std::string &error);

  /// @brief Parse the `hours` parameter of GET /api/security/stats (1..720, default 24).
  std::optional<int> parse_hours(const std::map<std::string, std::string> &query, std::string &error);

  /**
   * @brief Bounded in-memory list of events with filtering and statistics. Thread safe.
   */
  class event_store_t {
  public:
    explicit event_store_t(std::size_t capacity = ring_capacity);

    void add(event_t event);
    /// Replace the content (used when the log file is loaded).
    void load(std::vector<event_t> events);
    void clear();
    std::size_t size() const;

    /// @return The number of matching events and the requested page, newest first.
    std::pair<std::size_t, std::vector<event_t>> query(const filter_t &filter) const;

    /// @return `by_type`, `top_ips` and `top_clients` of the events since `since_ms`.
    nlohmann::json stats(std::int64_t since_ms) const;

  private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<event_t> events_;
  };

  // ---------------------------------------------------------------------------------------------
  // Event file
  // ---------------------------------------------------------------------------------------------

  /**
   * @brief Append-only JSON Lines file with one rotated generation (`<path>.1`). Not thread safe.
   */
  class event_file_t {
  public:
    event_file_t(std::filesystem::path path, std::uint64_t max_bytes);

    /// Append lines (each without a line break). Rotates the file first when it would exceed the cap.
    bool append(const std::vector<std::string> &lines);
    /// Read the newest `max_events` events of the rotated and the current file, oldest first.
    std::vector<event_t> load_tail(std::size_t max_events) const;
    /// Remove the file and its rotated generation.
    void clear();
    const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
    std::uint64_t max_bytes_;
  };

  // ---------------------------------------------------------------------------------------------
  // Addresses
  // ---------------------------------------------------------------------------------------------

  /**
   * @brief The unit that is counted and blocked: an IPv4 address or an IPv6 /64.
   */
  struct address_group_t {
    std::string key;  ///< Internal key, stable across spellings of the address.
    std::string display;  ///< `192.0.2.7` or `2001:db8:1:2::/64`.
    bool loopback = false;
    boost::asio::ip::address address;  ///< The normalised address the group was made from.
  };

  /// @return The group of an address, nothing for unspecified/multicast addresses.
  std::optional<address_group_t> group_of(const boost::asio::ip::address &address);
  /// @brief Same for text; a trailing `/64` (as shown for IPv6 groups) is accepted.
  std::optional<address_group_t> group_of(std::string_view text);

  /// @brief An address range of the whitelist.
  struct cidr_t {
    boost::asio::ip::address base;
    int prefix = 0;
    bool contains(const boost::asio::ip::address &address) const;
  };

  std::optional<cidr_t> parse_cidr(std::string_view text);

  /**
   * @brief Parse the `ban_whitelist` option: addresses or CIDR ranges separated by comma, semicolon or whitespace.
   * @return false (and the offending entry in `error`) if an entry is invalid.
   */
  bool parse_whitelist(std::string_view text, std::vector<cidr_t> &out, std::string &error);

  // ---------------------------------------------------------------------------------------------
  // Ban manager
  // ---------------------------------------------------------------------------------------------

  struct ban_settings_t {
    bool enabled = true;
    int threshold = 10;  ///< Weighted failures that cause a ban.
    int window_seconds = 600;
    int duration_seconds = 1800;  ///< 0 keeps the ban until it is lifted.
    std::vector<cidr_t> whitelist;
  };

  struct ban_t {
    std::string key;
    std::string display;
    std::string reason;
    std::int64_t since_ms = 0;
    std::int64_t until_ms = 0;  ///< 0 = until lifted.
    int failures = 0;
    bool manual = false;
  };

  nlohmann::json to_json(const ban_t &ban);

  /**
   * @brief Counts weighted failures per address group within a sliding window and keeps the bans. Thread safe.
   */
  class ban_manager_t {
  public:
    using clock_t = std::function<std::int64_t()>;

    explicit ban_manager_t(clock_t clock = system_now_ms);

    void configure(ban_settings_t settings);
    ban_settings_t settings() const;

    /// Fast check used by the listeners.
    bool is_blocked(const boost::asio::ip::address &address);
    bool is_blocked(std::string_view address);

    /// @return true if at least one ban exists (lock free, used on hot paths).
    bool any() const {
      return active_.load(std::memory_order_acquire) != 0;
    }

    /// @return The new ban when this failure made the address reach the threshold.
    std::optional<ban_t> record_failure(std::string_view address, int weight, std::string_view reason);

    /// @param duration_seconds negative uses the configured duration, 0 blocks until lifted.
    /// @return empty on success, otherwise the reason why the address cannot be blocked.
    std::string add_manual(std::string_view address, long long duration_seconds, std::string_view reason, ban_t *added);

    bool remove(std::string_view address, ban_t *removed);

    /// Active bans (expired ones are dropped first).
    std::vector<ban_t> list();
    std::size_t active_count();
    /// Bans that expired since the last call.
    std::vector<ban_t> take_expired();

    nlohmann::json to_json();
    /// Replace the bans with the stored ones (expired and invalid entries are skipped).
    void load_json(const nlohmann::json &json);

    /// Bans may be changed from several threads; the persistence layer asks whether something changed.
    std::uint64_t revision() const {
      return revision_.load(std::memory_order_acquire);
    }

  private:
    struct window_t {
      std::deque<std::pair<std::int64_t, int>> entries;
    };

    bool whitelisted_unlocked(const boost::asio::ip::address &address) const;
    void expire_unlocked(std::int64_t now);
    void publish_unlocked();

    clock_t clock_;
    mutable std::mutex mutex_;
    ban_settings_t settings_;
    std::map<std::string, ban_t> bans_;
    std::map<std::string, window_t> windows_;
    std::vector<ban_t> expired_;
    std::atomic<std::size_t> active_ {0};
    std::atomic<std::uint64_t> revision_ {0};
  };

  // ---------------------------------------------------------------------------------------------
  // Aggregation of blocked connection attempts
  // ---------------------------------------------------------------------------------------------

  /**
   * @brief Counts refused connections per address and service so that a flood becomes one event per minute.
   */
  class blocked_aggregator_t {
  public:
    struct entry_t {
      std::string ip;
      std::string service;
      std::uint64_t count = 0;
      std::int64_t first_ms = 0;
    };

    static constexpr std::int64_t window_ms = 60 * 1000;

    void note(std::string_view ip, std::string_view service, std::int64_t now_ms);
    /// Entries whose window ended (all entries with `force`).
    std::vector<entry_t> take_due(std::int64_t now_ms, bool force);

  private:
    std::mutex mutex_;
    std::map<std::string, entry_t> entries_;
  };

  /// @return How much a failed event of this type counts toward a ban (0 = it does not count).
  int failure_weight(std::string_view event_type);

  // ---------------------------------------------------------------------------------------------
  // Process wide log
  // ---------------------------------------------------------------------------------------------

  /**
   * @brief Settings of the process wide log (taken from the configuration).
   */
  struct settings_t {
    bool log_enabled = true;
    int max_mb = 10;
    ban_settings_t ban;
  };

  /// @brief Read the configuration, load the stored events and bans and start the writer thread.
  void init();
  /// @brief Same with explicit settings and storage directory (the file names are fixed).
  void init(const settings_t &settings, const std::filesystem::path &directory);
  /// @brief Write out what is pending and stop the writer thread.
  void shutdown();
  /// @brief Wait until everything recorded so far is written.
  void flush();

  /**
   * @brief Record an event: store it, write it to the host log and count it toward a ban if it is a failure.
   * @param ip Address of the peer (any spelling), may be empty.
   */
  void record(std::string_view type, std::string_view result, std::string_view ip, std::string_view service, const extra_t &extra = {});

  /// @return true if any address is blocked at the moment (lock free).
  bool any_blocked();

  /// @return true if the address is blocked (always false when blocking is disabled or the address is exempt).
  bool is_blocked(const boost::asio::ip::address &address);
  bool is_blocked(std::string_view address);

  /// @brief Count a refused connection (aggregated into one `connection_blocked` event per address and minute).
  void note_blocked(const boost::asio::ip::address &address, std::string_view service);

  /**
   * @brief Close a freshly accepted socket whose peer is blocked.
   * @return true if the socket was closed and must not be served.
   */
  bool refuse_if_blocked(boost::asio::ip::tcp::socket::lowest_layer_type &socket, std::string_view service);

  /// @brief Called (without locks held) when an address was blocked, so live sessions of it can be stopped.
  void set_ban_handler(std::function<void(const ban_t &)> handler);

  /// @brief Group key of an address text, empty if invalid. Used to match sessions with bans.
  std::string group_key(std::string_view address);

  nlohmann::json events_json(const filter_t &filter);
  nlohmann::json stats_json(int hours);
  nlohmann::json bans_json();
  /// @return empty on success, otherwise the reason.
  std::string unban(std::string_view address, std::string_view actor_ip);
  std::string ban(std::string_view address, long long duration_seconds, std::string_view reason, std::string_view actor_ip);
  /// @brief Clear the in-memory events and the log files, then record that it happened.
  void clear_events(std::string_view actor_ip);
}  // namespace security_log
