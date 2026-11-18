/**
 * @file src/security_log.cpp
 * @brief Definitions for the security event log and the automatic blocking of abusive addresses.
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>

// local includes
#include "config.h"
#include "file_handler.h"
#include "logging.h"
#include "security_log.h"

using namespace std::literals;

namespace security_log {
  namespace fs = std::filesystem;

  // ---------------------------------------------------------------------------------------------
  // Text, time and address helpers
  // ---------------------------------------------------------------------------------------------

  namespace {
    /// Code points that must not reach a log: C1 controls, zero width and direction controls, line separators.
    bool is_hidden_code_point(std::uint32_t cp) {
      return (cp >= 0x80 && cp <= 0x9F) || cp == 0xAD || cp == 0x61C || (cp >= 0x200B && cp <= 0x200F) ||
             (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069) || cp == 0xFEFF;
    }

    boost::asio::ip::address normalize(boost::asio::ip::address address) {
      if (address.is_v6()) {
        auto v6 = address.to_v6();
        if (v6.is_v4_mapped()) {
          return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
        }
        v6.scope_id(0);
        return v6;
      }
      return address;
    }

    /// Dotted IPv4 text must have four decimal parts: the platform parsers also accept short forms such as "10.1" or "0.0.0".
    bool strict_address_text(std::string_view text) {
      if (text.find(':') != std::string_view::npos) {
        return true;
      }
      int parts = 0;
      std::size_t digits = 0;
      int value = 0;
      for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
          value = value * 10 + (ch - '0');
          if (++digits > 3 || value > 255) {
            return false;
          }
        } else if (ch == '.' && digits > 0) {
          ++parts;
          digits = 0;
          value = 0;
        } else {
          return false;
        }
      }
      return digits > 0 && parts == 3;
    }

    std::string trim(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
      }
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
      }
      return std::string {text};
    }

    std::string lower(std::string_view text) {
      std::string result {text};
      for (auto &ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
          ch = static_cast<char>(ch - 'A' + 'a');
        }
      }
      return result;
    }

    bool icontains(const std::string &haystack, const std::string &lowered_needle) {
      if (lowered_needle.empty()) {
        return true;
      }
      return lower(haystack).find(lowered_needle) != std::string::npos;
    }

    // Days since 1970-01-01 of a civil date (proleptic Gregorian calendar).
    std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
      y -= m <= 2;
      const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
      const unsigned yoe = static_cast<unsigned>(y - era * 400);
      const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
      const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    }

    void civil_from_days(std::int64_t z, std::int64_t &y, unsigned &m, unsigned &d) {
      z += 719468;
      const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const unsigned doe = static_cast<unsigned>(z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      y = static_cast<std::int64_t>(yoe) + era * 400;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const unsigned mp = (5 * doy + 2) / 153;
      d = doy - (153 * mp + 2) / 5 + 1;
      m = mp < 10 ? mp + 3 : mp - 9;
      y += m <= 2;
    }

    bool read_digits(std::string_view text, std::size_t &pos, std::size_t count, int &value) {
      if (pos + count > text.size()) {
        return false;
      }
      value = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const char ch = text[pos + i];
        if (ch < '0' || ch > '9') {
          return false;
        }
        value = value * 10 + (ch - '0');
      }
      pos += count;
      return true;
    }

    bool read_char(std::string_view text, std::size_t &pos, char expected) {
      if (pos < text.size() && text[pos] == expected) {
        ++pos;
        return true;
      }
      return false;
    }
  }  // namespace

  std::string sanitize_text(std::string_view text, std::size_t max_bytes) {
    std::string out;
    out.reserve(std::min(text.size(), max_bytes));

    std::size_t i = 0;
    while (i < text.size() && out.size() < max_bytes) {
      const auto byte = static_cast<unsigned char>(text[i]);
      if (byte < 0x20 || byte == 0x7F) {
        out += ' ';
        ++i;
        continue;
      }
      if (byte < 0x80) {
        out += static_cast<char>(byte);
        ++i;
        continue;
      }

      std::size_t length = 0;
      std::uint32_t cp = 0;
      if (byte >= 0xC2 && byte <= 0xDF) {
        length = 2;
        cp = byte & 0x1F;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        length = 3;
        cp = byte & 0x0F;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        length = 4;
        cp = byte & 0x07;
      }

      bool valid = length != 0 && i + length <= text.size();
      for (std::size_t k = 1; valid && k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80) {
          valid = false;
        } else {
          cp = (cp << 6) | (next & 0x3F);
        }
      }
      if (valid) {
        const bool overlong = (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000);
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        valid = !overlong && !surrogate && cp <= 0x10FFFF;
      }
      if (!valid) {
        out += '?';
        ++i;
        continue;
      }
      if (!is_hidden_code_point(cp)) {
        if (out.size() + length > max_bytes) {
          break;
        }
        out.append(text.substr(i, length));
      }
      i += length;
    }

    return trim(out);
  }

  std::string normalize_ip(std::string_view text) {
    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(trim(text), ec);
    if (ec) {
      return {};
    }
    return normalize(address).to_string();
  }

  std::int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  std::string format_time(std::int64_t ms) {
    std::int64_t days = ms / 86400000;
    std::int64_t rest = ms % 86400000;
    if (rest < 0) {
      rest += 86400000;
      --days;
    }
    std::int64_t year;
    unsigned month, day;
    civil_from_days(days, year, month, day);

    const int hour = static_cast<int>(rest / 3600000);
    const int minute = static_cast<int>(rest / 60000 % 60);
    const int second = static_cast<int>(rest / 1000 % 60);
    const int milli = static_cast<int>(rest % 1000);

    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month, day, hour, minute, second, milli);
    return buffer;
  }

  std::optional<std::int64_t> parse_time(std::string_view text) {
    if (text.size() > 64) {
      return std::nullopt;
    }
    std::size_t pos = 0;
    int year, month, day, hour = 0, minute = 0, second = 0, milli = 0;
    if (!read_digits(text, pos, 4, year) || !read_char(text, pos, '-') || !read_digits(text, pos, 2, month) ||
        !read_char(text, pos, '-') || !read_digits(text, pos, 2, day)) {
      return std::nullopt;
    }
    if (pos < text.size() && (text[pos] == 'T' || text[pos] == 't' || text[pos] == ' ')) {
      ++pos;
      if (!read_digits(text, pos, 2, hour) || !read_char(text, pos, ':') || !read_digits(text, pos, 2, minute)) {
        return std::nullopt;
      }
      if (read_char(text, pos, ':')) {
        if (!read_digits(text, pos, 2, second)) {
          return std::nullopt;
        }
        if (read_char(text, pos, '.')) {
          std::size_t digits = 0;
          int scale = 100;
          while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) {
              milli += (text[pos] - '0') * scale;
              scale /= 10;
            }
            ++digits;
            ++pos;
          }
          if (digits == 0) {
            return std::nullopt;
          }
        }
      }
    }

    std::int64_t offset_ms = 0;
    if (pos < text.size()) {
      if (text[pos] == 'Z' || text[pos] == 'z') {
        ++pos;
      } else if (text[pos] == '+' || text[pos] == '-') {
        const int sign = text[pos] == '-' ? -1 : 1;
        ++pos;
        int off_hour, off_minute = 0;
        if (!read_digits(text, pos, 2, off_hour)) {
          return std::nullopt;
        }
        read_char(text, pos, ':');
        if (pos < text.size() && !read_digits(text, pos, 2, off_minute)) {
          return std::nullopt;
        }
        offset_ms = sign * (off_hour * 3600000LL + off_minute * 60000LL);
      }
    }
    if (pos != text.size()) {
      return std::nullopt;
    }

    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 60) {
      return std::nullopt;
    }
    const auto days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    // Reject days that do not exist in the month (31 February, ...)
    std::int64_t check_year;
    unsigned check_month, check_day;
    civil_from_days(days, check_year, check_month, check_day);
    if (check_year != year || check_month != static_cast<unsigned>(month) || check_day != static_cast<unsigned>(day)) {
      return std::nullopt;
    }

    return days * 86400000LL + hour * 3600000LL + minute * 60000LL + second * 1000LL + milli - offset_ms;
  }

  nlohmann::json to_json(const event_t &event) {
    return {
      {"ts", format_time(event.ts_ms)},
      {"type", event.type},
      {"ip", event.ip},
      {"service", event.service},
      {"client_uuid", event.client_uuid},
      {"client_name", event.client_name},
      {"app_id", event.app_id},
      {"app_name", event.app_name},
      {"result", event.result},
      {"detail", event.detail},
    };
  }

  std::optional<event_t> from_json(const nlohmann::json &json) {
    if (!json.is_object()) {
      return std::nullopt;
    }
    const auto text = [&](const char *key, std::size_t max_bytes) {
      auto it = json.find(key);
      return it != json.end() && it->is_string() ? sanitize_text(it->get_ref<const std::string &>(), max_bytes) : std::string {};
    };

    event_t event;
    auto ts = json.find("ts");
    if (ts == json.end() || !ts->is_string()) {
      return std::nullopt;
    }
    auto parsed = parse_time(ts->get_ref<const std::string &>());
    if (!parsed) {
      return std::nullopt;
    }
    event.ts_ms = *parsed;
    event.type = text("type", 32);
    if (event.type.empty()) {
      return std::nullopt;
    }
    event.ip = text("ip", 64);
    event.service = text("service", 16);
    event.client_uuid = text("client_uuid", max_id_bytes);
    event.client_name = text("client_name", max_name_bytes);
    event.app_id = text("app_id", max_id_bytes);
    event.app_name = text("app_name", max_name_bytes);
    event.result = text("result", 16);
    event.detail = text("detail", max_detail_bytes);
    return event;
  }

  // ---------------------------------------------------------------------------------------------
  // Filters
  // ---------------------------------------------------------------------------------------------

  namespace {
    std::string query_value(const std::map<std::string, std::string> &query, const char *name) {
      auto it = query.find(name);
      return it == query.end() ? std::string {} : it->second;
    }

    bool parse_number(const std::string &text, long long &value) {
      const auto trimmed = trim(text);
      auto [end, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
      return !trimmed.empty() && ec == std::errc {} && end == trimmed.data() + trimmed.size();
    }
  }  // namespace

  std::optional<filter_t> parse_filter(const std::map<std::string, std::string> &query, std::string &error) {
    filter_t filter;

    if (const auto list = query_value(query, "type"); !list.empty()) {
      std::size_t start = 0;
      while (start <= list.size()) {
        const auto end = list.find(',', start);
        auto token = lower(trim(std::string_view {list}.substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (!token.empty()) {
          if (token.size() > 32 || !std::ranges::all_of(token, [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
              })) {
            error = "type contains an invalid event type";
            return std::nullopt;
          }
          if (filter.types.size() >= 32) {
            error = "type lists too many event types";
            return std::nullopt;
          }
          filter.types.push_back(std::move(token));
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
    }

    filter.ip = lower(sanitize_text(query_value(query, "ip"), 64));
    filter.client = lower(sanitize_text(query_value(query, "client"), max_name_bytes));
    filter.app = lower(sanitize_text(query_value(query, "app"), max_name_bytes));
    filter.result = lower(sanitize_text(query_value(query, "result"), 16));

    for (auto [name, target] : {std::pair {"since", &filter.since}, std::pair {"until", &filter.until}}) {
      const auto text = trim(query_value(query, name));
      if (text.empty()) {
        continue;
      }
      auto parsed = parse_time(text);
      if (!parsed) {
        error = std::string(name) + " must be an ISO-8601 time such as 2026-01-31T12:00:00Z";
        return std::nullopt;
      }
      *target = parsed;
    }

    if (const auto text = query_value(query, "limit"); !trim(text).empty()) {
      long long value = 0;
      if (!parse_number(text, value)) {
        error = "limit must be a number between 1 and 1000";
        return std::nullopt;
      }
      filter.limit = static_cast<std::size_t>(std::clamp<long long>(value, 1, 1000));
    }
    if (const auto text = query_value(query, "offset"); !trim(text).empty()) {
      long long value = 0;
      if (!parse_number(text, value) || value < 0) {
        error = "offset must be a number of 0 or more";
        return std::nullopt;
      }
      filter.offset = static_cast<std::size_t>(std::min<long long>(value, 1'000'000));
    }
    return filter;
  }

  std::optional<int> parse_hours(const std::map<std::string, std::string> &query, std::string &error) {
    const auto text = query_value(query, "hours");
    if (trim(text).empty()) {
      return 24;
    }
    long long value = 0;
    if (!parse_number(text, value)) {
      error = "hours must be a number between 1 and 720";
      return std::nullopt;
    }
    return static_cast<int>(std::clamp<long long>(value, 1, 720));
  }

  // ---------------------------------------------------------------------------------------------
  // Event store
  // ---------------------------------------------------------------------------------------------

  event_store_t::event_store_t(std::size_t capacity):
      capacity_(std::max<std::size_t>(capacity, 1)) {
  }

  void event_store_t::add(event_t event) {
    std::lock_guard lock {mutex_};
    events_.push_back(std::move(event));
    while (events_.size() > capacity_) {
      events_.pop_front();
    }
  }

  void event_store_t::load(std::vector<event_t> events) {
    std::lock_guard lock {mutex_};
    events_.clear();
    const auto skip = events.size() > capacity_ ? events.size() - capacity_ : 0;
    for (std::size_t i = skip; i < events.size(); ++i) {
      events_.push_back(std::move(events[i]));
    }
  }

  void event_store_t::clear() {
    std::lock_guard lock {mutex_};
    events_.clear();
  }

  std::size_t event_store_t::size() const {
    std::lock_guard lock {mutex_};
    return events_.size();
  }

  std::pair<std::size_t, std::vector<event_t>> event_store_t::query(const filter_t &filter) const {
    const auto ip = lower(filter.ip);
    const auto client = lower(filter.client);
    const auto app = lower(filter.app);

    const auto matches = [&](const event_t &event) {
      if (filter.since && event.ts_ms < *filter.since) {
        return false;
      }
      if (filter.until && event.ts_ms > *filter.until) {
        return false;
      }
      if (!filter.types.empty() && std::ranges::find(filter.types, event.type) == filter.types.end()) {
        return false;
      }
      if (!filter.result.empty() && event.result != filter.result) {
        return false;
      }
      if (!icontains(event.ip, ip)) {
        return false;
      }
      if (!client.empty() && !icontains(event.client_name, client) && !icontains(event.client_uuid, client)) {
        return false;
      }
      if (!app.empty() && !icontains(event.app_name, app) && !icontains(event.app_id, app)) {
        return false;
      }
      return true;
    };

    std::pair<std::size_t, std::vector<event_t>> result;
    std::lock_guard lock {mutex_};
    for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
      if (!matches(*it)) {
        continue;
      }
      if (result.first >= filter.offset && result.second.size() < filter.limit) {
        result.second.push_back(*it);
      }
      ++result.first;
    }
    return result;
  }

  nlohmann::json event_store_t::stats(std::int64_t since_ms) const {
    struct ip_stat_t {
      std::uint64_t events = 0;
      std::uint64_t failures = 0;
      std::int64_t last_ms = 0;
    };
    struct client_stat_t {
      std::string name;
      std::uint64_t launches = 0;
      std::int64_t last_ms = 0;
    };
    std::map<std::string, std::uint64_t> by_type;
    std::map<std::string, ip_stat_t> by_ip;
    std::map<std::string, client_stat_t> by_client;

    {
      std::lock_guard lock {mutex_};
      for (const auto &event : events_) {
        if (event.ts_ms < since_ms) {
          continue;
        }
        ++by_type[event.type];
        if (!event.ip.empty()) {
          auto &stat = by_ip[event.ip];
          ++stat.events;
          if (event.result == result::fail || event.result == result::denied) {
            ++stat.failures;
          }
          stat.last_ms = std::max(stat.last_ms, event.ts_ms);
        }
        if ((event.type == type::launch || event.type == type::resume) && event.result == result::ok && !event.client_uuid.empty()) {
          auto &stat = by_client[event.client_uuid];
          ++stat.launches;
          if (event.ts_ms >= stat.last_ms) {
            stat.last_ms = event.ts_ms;
            stat.name = event.client_name;
          }
        }
      }
    }

    nlohmann::json json;
    json["by_type"] = nlohmann::json::object();
    for (const auto &[name, count] : by_type) {
      json["by_type"][name] = count;
    }

    std::vector<std::pair<std::string, ip_stat_t>> ips(by_ip.begin(), by_ip.end());
    std::ranges::sort(ips, [](const auto &a, const auto &b) {
      return std::tie(b.second.events, b.second.failures, a.first) < std::tie(a.second.events, a.second.failures, b.first);
    });
    json["top_ips"] = nlohmann::json::array();
    for (std::size_t i = 0; i < ips.size() && i < 10; ++i) {
      json["top_ips"].push_back({
        {"ip", ips[i].first},
        {"events", ips[i].second.events},
        {"failures", ips[i].second.failures},
        {"last_seen", format_time(ips[i].second.last_ms)},
      });
    }

    std::vector<std::pair<std::string, client_stat_t>> clients(by_client.begin(), by_client.end());
    std::ranges::sort(clients, [](const auto &a, const auto &b) {
      return std::tie(b.second.launches, a.first) < std::tie(a.second.launches, b.first);
    });
    json["top_clients"] = nlohmann::json::array();
    for (std::size_t i = 0; i < clients.size() && i < 10; ++i) {
      json["top_clients"].push_back({
        {"client_uuid", clients[i].first},
        {"client_name", clients[i].second.name},
        {"launches", clients[i].second.launches},
        {"last_seen", format_time(clients[i].second.last_ms)},
      });
    }
    return json;
  }

  // ---------------------------------------------------------------------------------------------
  // Event file
  // ---------------------------------------------------------------------------------------------

  namespace {
    /// Read the lines of the last `max_bytes` of a file (a partial first line is dropped).
    std::vector<std::string> read_tail_lines(const fs::path &path, std::uint64_t max_bytes) {
      std::vector<std::string> lines;
      std::ifstream in(path, std::ios::binary | std::ios::ate);
      if (!in) {
        return lines;
      }
      const auto size = static_cast<std::uint64_t>(in.tellg());
      const bool truncated = size > max_bytes;
      const auto start = truncated ? size - max_bytes : 0;
      in.seekg(static_cast<std::streamoff>(start));
      std::string content(static_cast<std::size_t>(size - start), '\0');
      in.read(content.data(), static_cast<std::streamsize>(content.size()));
      content.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));

      std::size_t pos = 0;
      if (truncated) {
        const auto newline = content.find('\n');
        pos = newline == std::string::npos ? content.size() : newline + 1;
      }
      while (pos < content.size()) {
        auto end = content.find('\n', pos);
        if (end == std::string::npos) {
          end = content.size();
        }
        if (end > pos) {
          auto line = content.substr(pos, end - pos);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (!line.empty()) {
            lines.push_back(std::move(line));
          }
        }
        pos = end + 1;
      }
      return lines;
    }
  }  // namespace

  event_file_t::event_file_t(fs::path path, std::uint64_t max_bytes):
      path_(std::move(path)),
      max_bytes_(max_bytes) {
  }

  bool event_file_t::append(const std::vector<std::string> &lines) {
    if (lines.empty()) {
      return true;
    }
    std::uint64_t added = 0;
    for (const auto &line : lines) {
      added += line.size() + 1;
    }

    std::error_code ec;
    const auto size = fs::exists(path_, ec) ? fs::file_size(path_, ec) : 0;
    if (!ec && size > 0 && size + added > max_bytes_) {
      auto rotated = path_;
      rotated += ".1";
      fs::remove(rotated, ec);
      fs::rename(path_, rotated, ec);
      if (ec) {
        // Never grow without a bound: drop the old content if it cannot be rotated
        fs::remove(path_, ec);
      }
    }

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
      return false;
    }
    for (const auto &line : lines) {
      out << line << '\n';
    }
    out.flush();
    return out.good();
  }

  std::vector<event_t> event_file_t::load_tail(std::size_t max_events) const {
    auto rotated = path_;
    rotated += ".1";
    constexpr std::uint64_t tail_bytes = 4ULL * 1024 * 1024;

    std::vector<event_t> events;
    for (const auto &path : {rotated, path_}) {
      for (const auto &line : read_tail_lines(path, tail_bytes)) {
        auto json = nlohmann::json::parse(line, nullptr, false);
        if (auto event = from_json(json)) {
          events.push_back(std::move(*event));
        }
      }
    }
    if (events.size() > max_events) {
      events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(max_events));
    }
    return events;
  }

  void event_file_t::clear() {
    std::error_code ec;
    fs::remove(path_, ec);
    auto rotated = path_;
    rotated += ".1";
    fs::remove(rotated, ec);
  }

  // ---------------------------------------------------------------------------------------------
  // Addresses
  // ---------------------------------------------------------------------------------------------

  std::optional<address_group_t> group_of(const boost::asio::ip::address &input) {
    const auto address = normalize(input);
    if (address.is_unspecified() || address.is_multicast()) {
      return std::nullopt;
    }

    address_group_t group;
    group.address = address;
    group.loopback = address.is_loopback();
    if (address.is_v4()) {
      group.key = address.to_string();
      group.display = group.key;
      return group;
    }

    auto bytes = address.to_v6().to_bytes();
    constexpr char digits[] = "0123456789abcdef";
    group.key = "v6/64:";
    for (int i = 0; i < 8; ++i) {
      group.key += digits[bytes[i] >> 4];
      group.key += digits[bytes[i] & 0xf];
    }
    for (int i = 8; i < 16; ++i) {
      bytes[i] = 0;
    }
    group.display = boost::asio::ip::make_address_v6(bytes).to_string() + "/64";
    return group;
  }

  std::optional<address_group_t> group_of(std::string_view text) {
    auto value = trim(text);
    if (const auto slash = value.find('/'); slash != std::string::npos) {
      const auto suffix = value.substr(slash + 1);
      value.resize(slash);
      boost::system::error_code ec;
      if (!strict_address_text(value)) {
        return std::nullopt;
      }
      const auto address = boost::asio::ip::make_address(value, ec);
      if (ec) {
        return std::nullopt;
      }
      const auto normalized = normalize(address);
      if (suffix != (normalized.is_v6() ? "64" : "32")) {
        return std::nullopt;
      }
    }
    boost::system::error_code ec;
    if (!strict_address_text(value)) {
      return std::nullopt;
    }
    const auto address = boost::asio::ip::make_address(value, ec);
    if (ec) {
      return std::nullopt;
    }
    return group_of(address);
  }

  bool cidr_t::contains(const boost::asio::ip::address &input) const {
    const auto address = normalize(input);
    if (address.is_v4() != base.is_v4()) {
      return false;
    }
    const auto compare = [&](const auto &a, const auto &b) {
      int bits = prefix;
      for (std::size_t i = 0; i < a.size() && bits > 0; ++i, bits -= 8) {
        const unsigned char mask = bits >= 8 ? 0xFF : static_cast<unsigned char>(0xFF << (8 - bits));
        if ((a[i] & mask) != (b[i] & mask)) {
          return false;
        }
      }
      return true;
    };
    return address.is_v4() ? compare(address.to_v4().to_bytes(), base.to_v4().to_bytes()) : compare(address.to_v6().to_bytes(), base.to_v6().to_bytes());
  }

  std::optional<cidr_t> parse_cidr(std::string_view text) {
    auto value = trim(text);
    std::optional<int> prefix;
    if (const auto slash = value.find('/'); slash != std::string::npos) {
      long long number = 0;
      if (!parse_number(value.substr(slash + 1), number) || number < 0 || number > 128) {
        return std::nullopt;
      }
      prefix = static_cast<int>(number);
      value.resize(slash);
    }
    boost::system::error_code ec;
    if (!strict_address_text(value)) {
      return std::nullopt;
    }
    const auto address = boost::asio::ip::make_address(value, ec);
    if (ec) {
      return std::nullopt;
    }

    cidr_t cidr;
    cidr.base = normalize(address);
    const bool mapped = address.is_v6() && cidr.base.is_v4();
    if (prefix && mapped) {
      if (*prefix < 96) {
        return std::nullopt;
      }
      prefix = *prefix - 96;
    }
    cidr.prefix = prefix.value_or(cidr.base.is_v4() ? 32 : 128);
    // A range of /0 would exempt every address from blocking
    if (cidr.prefix <= 0 || cidr.prefix > (cidr.base.is_v4() ? 32 : 128)) {
      return std::nullopt;
    }
    return cidr;
  }

  bool parse_whitelist(std::string_view text, std::vector<cidr_t> &out, std::string &error) {
    // Besides the plain list a JSON-style list (["a", "b"]) is understood
    const auto separator = [](char ch) {
      return ch == ',' || ch == ';' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '[' || ch == ']' || ch == '"' || ch == '\'';
    };

    bool ok = true;
    std::size_t pos = 0;
    while (pos < text.size()) {
      while (pos < text.size() && separator(text[pos])) {
        ++pos;
      }
      auto end = pos;
      while (end < text.size() && !separator(text[end])) {
        ++end;
      }
      if (end > pos) {
        const auto entry = text.substr(pos, end - pos);
        if (auto cidr = parse_cidr(entry)) {
          out.push_back(*cidr);
        } else if (ok) {
          ok = false;
          error = sanitize_text(entry, 64);
        }
      }
      pos = end;
    }
    return ok;
  }

  // ---------------------------------------------------------------------------------------------
  // Ban manager
  // ---------------------------------------------------------------------------------------------

  nlohmann::json to_json(const ban_t &ban) {
    return {
      {"ip", ban.display},
      {"reason", ban.reason},
      {"since", format_time(ban.since_ms)},
      {"until", ban.until_ms == 0 ? nlohmann::json(nullptr) : nlohmann::json(format_time(ban.until_ms))},
      {"failures", ban.failures},
      {"manual", ban.manual},
    };
  }

  ban_manager_t::ban_manager_t(clock_t clock):
      clock_(std::move(clock)) {
  }

  void ban_manager_t::configure(ban_settings_t settings) {
    std::lock_guard lock {mutex_};
    settings_ = std::move(settings);
    windows_.clear();
    publish_unlocked();
  }

  ban_settings_t ban_manager_t::settings() const {
    std::lock_guard lock {mutex_};
    return settings_;
  }

  bool ban_manager_t::whitelisted_unlocked(const boost::asio::ip::address &address) const {
    return std::ranges::any_of(settings_.whitelist, [&](const cidr_t &cidr) {
      return cidr.contains(address);
    });
  }

  void ban_manager_t::publish_unlocked() {
    active_.store(bans_.size(), std::memory_order_release);
    revision_.fetch_add(1, std::memory_order_acq_rel);
  }

  void ban_manager_t::expire_unlocked(std::int64_t now) {
    bool changed = false;
    for (auto it = bans_.begin(); it != bans_.end();) {
      if (it->second.until_ms != 0 && now >= it->second.until_ms) {
        expired_.push_back(std::move(it->second));
        it = bans_.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    if (changed) {
      publish_unlocked();
    }
  }

  bool ban_manager_t::is_blocked(const boost::asio::ip::address &address) {
    // Nothing is blocked in the common case: do not touch the lock
    if (active_.load(std::memory_order_acquire) == 0) {
      return false;
    }
    auto group = group_of(address);
    if (!group || group->loopback) {
      return false;
    }

    std::lock_guard lock {mutex_};
    if (!settings_.enabled) {
      return false;
    }
    auto it = bans_.find(group->key);
    if (it == bans_.end()) {
      return false;
    }
    if (it->second.until_ms != 0 && clock_() >= it->second.until_ms) {
      expire_unlocked(clock_());
      return false;
    }
    return !whitelisted_unlocked(group->address);
  }

  bool ban_manager_t::is_blocked(std::string_view address) {
    boost::system::error_code ec;
    const auto parsed = boost::asio::ip::make_address(trim(address), ec);
    return !ec && is_blocked(parsed);
  }

  std::optional<ban_t> ban_manager_t::record_failure(std::string_view address, int weight, std::string_view reason) {
    auto group = group_of(address);
    if (!group || group->loopback || weight <= 0) {
      return std::nullopt;
    }

    std::lock_guard lock {mutex_};
    if (!settings_.enabled || whitelisted_unlocked(group->address)) {
      return std::nullopt;
    }
    const auto now = clock_();
    expire_unlocked(now);
    if (bans_.contains(group->key)) {
      return std::nullopt;
    }

    // Bound the memory an address scan can use
    if (windows_.size() >= 4096 && !windows_.contains(group->key)) {
      const auto horizon = now - static_cast<std::int64_t>(settings_.window_seconds) * 1000;
      std::erase_if(windows_, [&](const auto &entry) {
        return entry.second.entries.empty() || entry.second.entries.back().first < horizon;
      });
      if (windows_.size() >= 4096) {
        auto oldest = std::ranges::min_element(windows_, {}, [](const auto &entry) {
          return entry.second.entries.back().first;
        });
        windows_.erase(oldest);
      }
    }

    auto &window = windows_[group->key];
    const auto horizon = now - static_cast<std::int64_t>(settings_.window_seconds) * 1000;
    while (!window.entries.empty() && window.entries.front().first <= horizon) {
      window.entries.pop_front();
    }
    window.entries.emplace_back(now, weight);
    long long total = 0;
    for (const auto &entry : window.entries) {
      total += entry.second;
    }
    if (total < settings_.threshold) {
      return std::nullopt;
    }

    ban_t ban;
    ban.key = group->key;
    ban.display = group->display;
    ban.reason = sanitize_text(reason, 64);
    ban.since_ms = now;
    ban.until_ms = settings_.duration_seconds == 0 ? 0 : now + static_cast<std::int64_t>(settings_.duration_seconds) * 1000;
    ban.failures = static_cast<int>(std::min<std::size_t>(window.entries.size(), 1'000'000));
    ban.manual = false;
    windows_.erase(group->key);
    bans_[ban.key] = ban;
    publish_unlocked();
    return ban;
  }

  std::string ban_manager_t::add_manual(std::string_view address, long long duration_seconds, std::string_view reason, ban_t *added) {
    auto group = group_of(address);
    if (!group) {
      return "Not a valid IP address";
    }
    if (group->loopback) {
      return "The host PC itself (loopback) cannot be blocked";
    }
    if (duration_seconds > 2592000) {
      return "duration_seconds must be between 0 and 2592000";
    }

    std::lock_guard lock {mutex_};
    if (!settings_.enabled) {
      return "Blocking is disabled (ban_enabled)";
    }
    if (whitelisted_unlocked(group->address)) {
      return "This address is on the whitelist (ban_whitelist) and cannot be blocked";
    }
    const auto now = clock_();
    expire_unlocked(now);

    const long long seconds = duration_seconds < 0 ? settings_.duration_seconds : duration_seconds;
    ban_t ban;
    ban.key = group->key;
    ban.display = group->display;
    ban.reason = sanitize_text(reason, 120);
    ban.since_ms = now;
    ban.until_ms = seconds == 0 ? 0 : now + seconds * 1000;
    ban.failures = 0;
    ban.manual = true;
    if (auto existing = bans_.find(ban.key); existing != bans_.end()) {
      ban.failures = existing->second.failures;
    }
    bans_[ban.key] = ban;
    windows_.erase(ban.key);
    publish_unlocked();
    if (added) {
      *added = ban;
    }
    return {};
  }

  bool ban_manager_t::remove(std::string_view address, ban_t *removed) {
    auto group = group_of(address);
    if (!group) {
      return false;
    }
    std::lock_guard lock {mutex_};
    expire_unlocked(clock_());
    auto it = bans_.find(group->key);
    if (it == bans_.end()) {
      return false;
    }
    if (removed) {
      *removed = it->second;
    }
    bans_.erase(it);
    windows_.erase(group->key);
    publish_unlocked();
    return true;
  }

  std::vector<ban_t> ban_manager_t::list() {
    std::lock_guard lock {mutex_};
    expire_unlocked(clock_());
    std::vector<ban_t> result;
    for (const auto &[key, ban] : bans_) {
      result.push_back(ban);
    }
    std::ranges::sort(result, [](const ban_t &a, const ban_t &b) {
      return a.since_ms > b.since_ms;
    });
    return result;
  }

  std::size_t ban_manager_t::active_count() {
    std::lock_guard lock {mutex_};
    expire_unlocked(clock_());
    return bans_.size();
  }

  std::vector<ban_t> ban_manager_t::take_expired() {
    std::lock_guard lock {mutex_};
    expire_unlocked(clock_());
    return std::exchange(expired_, {});
  }

  nlohmann::json ban_manager_t::to_json() {
    nlohmann::json json;
    json["version"] = 1;
    json["bans"] = nlohmann::json::array();
    for (const auto &ban : list()) {
      json["bans"].push_back(security_log::to_json(ban));
    }
    return json;
  }

  void ban_manager_t::load_json(const nlohmann::json &json) {
    std::map<std::string, ban_t> loaded;
    const auto now = clock_();
    auto list = json.is_object() ? json.find("bans") : json.end();
    if (list != json.end() && list->is_array()) {
      for (const auto &item : *list) {
        if (!item.is_object() || !item.contains("ip") || !item["ip"].is_string() || !item.contains("since") || !item["since"].is_string()) {
          continue;
        }
        auto group = group_of(item["ip"].get_ref<const std::string &>());
        auto since = parse_time(item["since"].get_ref<const std::string &>());
        if (!group || group->loopback || !since) {
          continue;
        }
        ban_t ban;
        ban.key = group->key;
        ban.display = group->display;
        ban.since_ms = *since;
        if (auto until = item.find("until"); until != item.end() && until->is_string()) {
          auto parsed = parse_time(until->get_ref<const std::string &>());
          if (!parsed) {
            continue;
          }
          ban.until_ms = *parsed;
          if (ban.until_ms <= now) {
            continue;
          }
        }
        if (auto reason = item.find("reason"); reason != item.end() && reason->is_string()) {
          ban.reason = sanitize_text(reason->get_ref<const std::string &>(), 120);
        }
        if (auto failures = item.find("failures"); failures != item.end() && failures->is_number_integer()) {
          ban.failures = static_cast<int>(std::clamp<long long>(failures->get<long long>(), 0, 1'000'000));
        }
        if (auto manual = item.find("manual"); manual != item.end() && manual->is_boolean()) {
          ban.manual = manual->get<bool>();
        }
        loaded[ban.key] = std::move(ban);
      }
    }

    std::lock_guard lock {mutex_};
    bans_ = std::move(loaded);
    publish_unlocked();
  }

  // ---------------------------------------------------------------------------------------------
  // Aggregation of blocked connection attempts
  // ---------------------------------------------------------------------------------------------

  void blocked_aggregator_t::note(std::string_view ip, std::string_view service, std::int64_t now_ms) {
    std::lock_guard lock {mutex_};
    const auto key = std::string {ip} + '|' + std::string {service};
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      if (entries_.size() >= 1024) {
        return;
      }
      it = entries_.emplace(key, entry_t {std::string {ip}, std::string {service}, 0, now_ms}).first;
    }
    ++it->second.count;
  }

  std::vector<blocked_aggregator_t::entry_t> blocked_aggregator_t::take_due(std::int64_t now_ms, bool force) {
    std::vector<entry_t> due;
    std::lock_guard lock {mutex_};
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (force || now_ms - it->second.first_ms >= window_ms) {
        due.push_back(std::move(it->second));
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    return due;
  }

  // ---------------------------------------------------------------------------------------------
  // Process wide log
  // ---------------------------------------------------------------------------------------------

  int failure_weight(std::string_view event_type) {
    if (event_type == type::pair_failure || event_type == type::webui_login_failure) {
      return 2;
    }
    // cert_rejected does not count: clients routinely try HTTPS with their own certificate before they are paired
    // (and keep doing so after an unpair), so counting it would lock a new client out before it can pair.
    // permission_denied does not count either: it comes from an authenticated, paired client (e.g. one without
    // the permission to list app images asks for every app).
    if (event_type == type::rtsp_rejected) {
      return 1;
    }
    return 0;
  }

  namespace {
    constexpr auto events_file_name = "security_events.log"sv;
    constexpr auto bans_file_name = "security_bans.json"sv;

    /// Event types that a single peer can produce in bulk: one entry per address and interval is stored.
    bool is_rate_limited_type(std::string_view event_type) {
      return event_type == type::webui_denied || event_type == type::control_rejected || event_type == type::rtsp_rejected ||
             event_type == type::cert_rejected || event_type == type::permission_denied;
    }

    constexpr std::int64_t rate_limit_interval_ms = 5000;
    constexpr std::size_t max_rate_entries = 16384;

    struct global_t {
      std::mutex setup_mutex;
      std::atomic<bool> log_enabled {true};
      std::atomic<bool> ban_enabled {true};

      event_store_t store;
      ban_manager_t bans;
      blocked_aggregator_t blocked;

      std::unique_ptr<event_file_t> file;
      fs::path bans_path;
      std::mutex bans_file_mutex;
      std::uint64_t saved_revision = 0;

      // Writer / maintenance thread
      std::mutex queue_mutex;
      std::condition_variable queue_cv;
      std::condition_variable idle_cv;
      std::deque<std::string> queue;
      std::size_t in_flight = 0;
      bool clear_requested = false;
      bool stop = false;
      bool running = false;
      std::thread worker;

      std::mutex rate_mutex;
      struct rate_t {
        std::int64_t last_ms = 0;
        std::uint64_t suppressed = 0;
      };
      std::unordered_map<std::string, rate_t> rates;

      std::mutex handler_mutex;
      std::function<void(const ban_t &)> ban_handler;
    };

    global_t &global() {
      // Intentionally never destroyed: the log must stay usable until the process exits
      static global_t *instance = new global_t();
      return *instance;
    }

    void enqueue_line(global_t &g, std::string line) {
      std::lock_guard lock {g.queue_mutex};
      if (!g.running || !g.file) {
        return;
      }
      // A stuck disk must not grow the queue without a bound
      if (g.queue.size() >= 20000) {
        g.queue.pop_front();
        if (g.in_flight > 0) {
          --g.in_flight;
        }
      }
      g.queue.push_back(std::move(line));
      ++g.in_flight;
      g.queue_cv.notify_one();
    }

    void save_bans(global_t &g) {
      std::lock_guard lock {g.bans_file_mutex};
      if (g.bans_path.empty()) {
        return;
      }
      const auto revision = g.bans.revision();
      const auto text = g.bans.to_json().dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
      if (file_handler::write_file_atomic(g.bans_path.string().c_str(), text) != 0) {
        BOOST_LOG(warning) << "Couldn't write ["sv << g.bans_path.string() << ']';
        return;
      }
      g.saved_revision = revision;
    }

    std::string describe_time(std::int64_t until_ms) {
      return until_ms == 0 ? "until lifted"s : "until "s + format_time(until_ms);
    }

    void announce_ban(global_t &g, const ban_t &ban, std::string_view actor_ip) {
      save_bans(g);
      std::string detail = ban.manual ? "manual=true"s : "automatic"s;
      detail += " "s + describe_time(ban.until_ms);
      if (!ban.reason.empty()) {
        detail += " reason="s + ban.reason;
      }
      if (!ban.manual) {
        detail += " failures="s + std::to_string(ban.failures);
      }
      if (!actor_ip.empty()) {
        detail += " by="s + std::string {actor_ip};
      }
      record(type::ban_added, result::ok, ban.display, service::host, {.detail = detail});

      std::function<void(const ban_t &)> handler;
      {
        std::lock_guard lock {g.handler_mutex};
        handler = g.ban_handler;
      }
      if (handler) {
        try {
          handler(ban);
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Stopping the sessions of a blocked address failed: "sv << e.what();
        }
      }
    }

    void maintain(global_t &g, bool force) {
      const auto now = system_now_ms();
      for (const auto &entry : g.blocked.take_due(now, force)) {
        record(type::connection_blocked, result::blocked, entry.ip, entry.service, {.detail = "count="s + std::to_string(entry.count) + " in 60s"s});
      }
      for (const auto &ban : g.bans.take_expired()) {
        record(type::ban_removed, result::ok, ban.display, service::host, {.detail = "expired"});
      }
      if (g.bans.revision() != [&] {
            std::lock_guard lock {g.bans_file_mutex};
            return g.saved_revision;
          }()) {
        save_bans(g);
      }

      std::lock_guard lock {g.rate_mutex};
      if (g.rates.size() > 1024) {
        std::erase_if(g.rates, [&](const auto &entry) {
          return now - entry.second.last_ms > 60000;
        });
      }
    }

    void worker_main(global_t &g) {
      std::unique_lock lock {g.queue_mutex};
      while (true) {
        g.queue_cv.wait_for(lock, 1s, [&] {
          return g.stop || g.clear_requested || !g.queue.empty();
        });

        std::vector<std::string> batch;
        batch.reserve(g.queue.size());
        for (auto &line : g.queue) {
          batch.push_back(std::move(line));
        }
        g.queue.clear();
        const bool do_clear = std::exchange(g.clear_requested, false);
        const bool stopping = g.stop;
        lock.unlock();

        if (g.file) {
          if (do_clear) {
            g.file->clear();
          }
          if (!batch.empty() && !g.file->append(batch)) {
            BOOST_LOG(warning) << "Couldn't write ["sv << g.file->path().string() << ']';
          }
        }

        if (!stopping) {
          maintain(g, false);
        }

        lock.lock();
        g.in_flight = g.in_flight >= batch.size() ? g.in_flight - batch.size() : 0;
        if (g.in_flight == 0) {
          g.idle_cv.notify_all();
        }
        if (stopping && g.queue.empty()) {
          return;
        }
      }
    }

    void stop_worker(global_t &g) {
      std::thread worker;
      {
        std::lock_guard lock {g.queue_mutex};
        if (!g.running) {
          return;
        }
      }
      maintain(g, true);
      {
        std::lock_guard lock {g.queue_mutex};
        g.stop = true;
        g.queue_cv.notify_all();
        worker = std::move(g.worker);
      }
      if (worker.joinable()) {
        worker.join();
      }
      std::lock_guard lock {g.queue_mutex};
      g.running = false;
      g.stop = false;
      g.in_flight = 0;
    }
  }  // namespace

  void init(const settings_t &settings, const fs::path &directory) {
    auto &g = global();
    std::lock_guard setup {g.setup_mutex};
    stop_worker(g);

    g.log_enabled.store(settings.log_enabled);
    g.ban_enabled.store(settings.ban.enabled);
    g.bans.configure(settings.ban);
    g.store.clear();
    {
      std::lock_guard lock {g.rate_mutex};
      g.rates.clear();
    }

    std::error_code ec;
    fs::create_directories(directory, ec);
    g.file = std::make_unique<event_file_t>(directory / std::string {events_file_name}, static_cast<std::uint64_t>(std::clamp(settings.max_mb, 1, 100)) * 1024ULL * 1024ULL);
    g.bans_path = directory / std::string {bans_file_name};

    if (settings.log_enabled) {
      g.store.load(g.file->load_tail(ring_capacity));
    }

    g.bans.load_json(nlohmann::json::object());
    {
      std::ifstream in(g.bans_path, std::ios::binary);
      if (in) {
        auto json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded()) {
          BOOST_LOG(warning) << "Ignoring unreadable ["sv << g.bans_path.string() << ']';
        } else {
          g.bans.load_json(json);
        }
      }
    }
    {
      std::lock_guard lock {g.bans_file_mutex};
      g.saved_revision = g.bans.revision();
    }

    std::lock_guard lock {g.queue_mutex};
    g.queue.clear();
    g.in_flight = 0;
    g.running = true;
    g.stop = false;
    g.worker = std::thread {worker_main, std::ref(g)};
  }

  void init() {
    settings_t settings;
    settings.log_enabled = config::security.log;
    settings.max_mb = config::security.log_max_mb;
    settings.ban.enabled = config::security.ban_enabled;
    settings.ban.threshold = config::security.ban_threshold;
    settings.ban.window_seconds = config::security.ban_window_seconds;
    settings.ban.duration_seconds = config::security.ban_duration_seconds;
    std::string bad_entry;
    if (!parse_whitelist(config::security.ban_whitelist, settings.ban.whitelist, bad_entry)) {
      BOOST_LOG(warning) << "Ignoring the invalid ban_whitelist entry ["sv << bad_entry << ']';
    }
    init(settings, fs::path {config::nvhttp.file_state}.parent_path());
  }

  void shutdown() {
    auto &g = global();
    std::lock_guard setup {g.setup_mutex};
    stop_worker(g);
    save_bans(g);
  }

  void flush() {
    auto &g = global();
    std::unique_lock lock {g.queue_mutex};
    if (!g.running) {
      return;
    }
    g.queue_cv.notify_all();
    g.idle_cv.wait_for(lock, 5s, [&] {
      return g.in_flight == 0;
    });
  }

  void record(std::string_view event_type, std::string_view event_result, std::string_view ip, std::string_view event_service, const extra_t &extra) {
    auto &g = global();

    event_t event;
    event.ts_ms = system_now_ms();
    event.type = sanitize_text(event_type, 32);
    event.result = sanitize_text(event_result, 16);
    event.service = sanitize_text(event_service, 16);
    event.ip = normalize_ip(ip);
    if (event.ip.empty() && !ip.empty()) {
      event.ip = sanitize_text(ip, 64);
    }
    event.client_uuid = sanitize_text(extra.client_uuid, max_id_bytes);
    event.client_name = sanitize_text(extra.client_name, max_name_bytes);
    event.app_id = sanitize_text(extra.app_id, max_id_bytes);
    event.app_name = sanitize_text(extra.app_name, max_name_bytes);
    event.detail = sanitize_text(extra.detail, max_detail_bytes);

    // Failures count toward a ban even when they are not stored
    std::optional<ban_t> new_ban;
    if (g.ban_enabled.load(std::memory_order_relaxed) && !event.ip.empty() && (event.result == result::fail || event.result == result::denied)) {
      if (const auto weight = failure_weight(event.type); weight > 0) {
        new_ban = g.bans.record_failure(event.ip, weight, event.type);
      }
    }

    if (g.log_enabled.load(std::memory_order_relaxed)) {
      bool store_it = true;
      if (is_rate_limited_type(event.type) || (event.type == type::pair_request && event.result != result::ok)) {
        std::lock_guard lock {g.rate_mutex};
        const auto rate_key = event.type + '|' + event.ip;
        // Address rotation must not grow the table without a bound: drop settled entries, then stop storing
        if (g.rates.size() >= max_rate_entries && !g.rates.contains(rate_key)) {
          std::erase_if(g.rates, [&](const auto &entry) {
            return entry.second.suppressed == 0 && event.ts_ms - entry.second.last_ms >= rate_limit_interval_ms;
          });
        }
        if (g.rates.size() >= max_rate_entries && !g.rates.contains(rate_key)) {
          store_it = false;
        } else {
          auto &rate = g.rates[rate_key];
          if (rate.last_ms != 0 && event.ts_ms - rate.last_ms < rate_limit_interval_ms) {
            ++rate.suppressed;
            store_it = false;
          } else {
            rate.last_ms = event.ts_ms;
            if (rate.suppressed > 0) {
              event.detail = sanitize_text(event.detail + " (+"s + std::to_string(rate.suppressed) + " similar suppressed)"s, max_detail_bytes);
              rate.suppressed = 0;
            }
          }
        }
      }

      if (store_it) {
        BOOST_LOG(info) << "Security event: "sv << event.type << " result="sv << event.result << " ip="sv << event.ip << " service="sv << event.service
                        << " client=\""sv << event.client_name << "\" ("sv << event.client_uuid << ") app=\""sv << event.app_name << "\" detail=\""sv << event.detail << '"';
        enqueue_line(g, to_json(event).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        g.store.add(std::move(event));
      }
    }

    if (new_ban) {
      announce_ban(g, *new_ban, {});
    }
  }

  bool any_blocked() {
    auto &g = global();
    return g.ban_enabled.load(std::memory_order_relaxed) && g.bans.any();
  }

  bool is_blocked(const boost::asio::ip::address &address) {
    auto &g = global();
    return g.ban_enabled.load(std::memory_order_relaxed) && g.bans.is_blocked(address);
  }

  bool is_blocked(std::string_view address) {
    auto &g = global();
    return g.ban_enabled.load(std::memory_order_relaxed) && g.bans.is_blocked(address);
  }

  void note_blocked(const boost::asio::ip::address &address, std::string_view event_service) {
    global().blocked.note(normalize(address).to_string(), event_service, system_now_ms());
  }

  bool refuse_if_blocked(boost::asio::ip::tcp::socket::lowest_layer_type &socket, std::string_view event_service) {
    boost::system::error_code ec;
    const auto endpoint = socket.remote_endpoint(ec);
    if (ec || !is_blocked(endpoint.address())) {
      return false;
    }
    note_blocked(endpoint.address(), event_service);
    socket.close(ec);
    return true;
  }

  void set_ban_handler(std::function<void(const ban_t &)> handler) {
    auto &g = global();
    std::lock_guard lock {g.handler_mutex};
    g.ban_handler = std::move(handler);
  }

  std::string group_key(std::string_view address) {
    auto group = group_of(address);
    return group ? group->key : std::string {};
  }

  nlohmann::json events_json(const filter_t &filter) {
    auto [total, events] = global().store.query(filter);
    nlohmann::json json;
    json["total"] = total;
    json["events"] = nlohmann::json::array();
    for (const auto &event : events) {
      json["events"].push_back(to_json(event));
    }
    return json;
  }

  nlohmann::json stats_json(int hours) {
    auto &g = global();
    auto json = g.store.stats(system_now_ms() - static_cast<std::int64_t>(std::clamp(hours, 1, 720)) * 3600000LL);
    json["active_bans"] = g.bans.active_count();
    return json;
  }

  nlohmann::json bans_json() {
    nlohmann::json json;
    json["bans"] = nlohmann::json::array();
    for (const auto &ban : global().bans.list()) {
      json["bans"].push_back(to_json(ban));
    }
    return json;
  }

  std::string unban(std::string_view address, std::string_view actor_ip) {
    auto &g = global();
    ban_t removed;
    if (!g.bans.remove(address, &removed)) {
      return group_of(address) ? "This address is not blocked" : "Not a valid IP address";
    }
    save_bans(g);
    std::string detail = "lifted by admin";
    if (!actor_ip.empty()) {
      detail += " by=" + std::string {actor_ip};
    }
    record(type::ban_removed, result::ok, removed.display, service::webui, {.detail = detail});
    return {};
  }

  std::string ban(std::string_view address, long long duration_seconds, std::string_view reason, std::string_view actor_ip) {
    auto &g = global();
    ban_t added;
    // An administrator must not lock themselves out of the Web UI
    if (!actor_ip.empty()) {
      const auto actor = group_key(actor_ip);
      if (!actor.empty() && actor == group_key(address)) {
        return "The address you are using cannot be blocked";
      }
    }
    if (auto error = g.bans.add_manual(address, duration_seconds, reason, &added); !error.empty()) {
      return error;
    }
    announce_ban(g, added, actor_ip);
    return {};
  }

  void clear_events(std::string_view actor_ip) {
    auto &g = global();
    g.store.clear();
    {
      std::lock_guard lock {g.queue_mutex};
      if (g.running) {
        g.in_flight = g.in_flight >= g.queue.size() ? g.in_flight - g.queue.size() : 0;
        g.queue.clear();
        g.clear_requested = true;
        g.queue_cv.notify_all();
      }
    }
    flush();
    record(type::events_cleared, result::ok, actor_ip, service::webui, {.detail = "event log cleared"});
  }
}  // namespace security_log
