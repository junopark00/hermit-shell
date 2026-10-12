/**
 * @file src/webhook_core.h
 * @brief Shell: the pure parts of the webhook notifications (docs/features.md "Webhooks"): URL
 * rules, the event list, the event JSON, the HMAC signature and the bounded queue. No logging, no
 * network and no Shell globals, so shell/tests/webhook_test.cpp can build it alone.
 */
#pragma once

#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

namespace webhook::core {
  /// Event groups a user can turn on with `webhook_events`; `test` is always sent.
  inline const std::set<std::string> known_groups {"pairing", "stream", "app"};

  /// Events the queue holds at most; the oldest is dropped when a new one arrives on a full queue.
  constexpr std::size_t queue_capacity = 64;

  enum class url_error_e {
    ok,
    empty,  ///< no URL: webhooks are off (not a failure)
    scheme,  ///< neither `https://` nor an allowed `http://`
    userinfo,  ///< `user:password@` in the URL
    host,  ///< empty host or a character that does not belong in one
    http_not_private,  ///< `http://` with a host that is not a private or Tailscale IP or `localhost`
    bad_char,  ///< space or control character
  };

  inline const char *url_error_text(url_error_e error) {
    switch (error) {
      case url_error_e::ok:
        return "ok";
      case url_error_e::empty:
        return "no URL";
      case url_error_e::scheme:
        return "the URL must start with https://";
      case url_error_e::userinfo:
        return "the URL must not contain a user name or password";
      case url_error_e::host:
        return "the URL has no valid host";
      case url_error_e::http_not_private:
        return "http:// is only allowed with a private or Tailscale IP address or localhost as the host";
      case url_error_e::bad_char:
        return "the URL contains a space or control character";
    }
    return "invalid URL";
  }

  struct url_parts_t {
    bool https = true;
    std::string host;  ///< without brackets and port
    std::string port;  ///< empty when the URL has none
  };

  /// Whether a host string is a literal private, link-local or Tailscale IP address (the caller's
  /// address classes). `localhost` is accepted without asking it.
  using host_is_private_f = std::function<bool(std::string_view host)>;

  /**
   * @brief Check a webhook URL: `https://` is required; `http://` is accepted only when
   * `allow_http_private` is set and the host is `localhost` or `host_is_private` says yes. No
   * userinfo, no spaces or control characters.
   */
  inline url_error_e validate_url(std::string_view url, bool allow_http_private, const host_is_private_f &host_is_private, url_parts_t *parts = nullptr) {
    if (url.empty()) {
      return url_error_e::empty;
    }
    for (unsigned char c : url) {
      if (c <= ' ' || c == 0x7F) {
        return url_error_e::bad_char;
      }
    }

    auto starts_with_ci = [](std::string_view text, std::string_view prefix) {
      if (text.size() < prefix.size()) {
        return false;
      }
      for (std::size_t i = 0; i < prefix.size(); ++i) {
        const auto a = static_cast<unsigned char>(text[i]);
        const auto b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) {
          return false;
        }
      }
      return true;
    };

    url_parts_t result;
    std::string_view rest;
    if (starts_with_ci(url, "https://")) {
      result.https = true;
      rest = url.substr(8);
    } else if (starts_with_ci(url, "http://")) {
      if (!allow_http_private) {
        return url_error_e::scheme;
      }
      result.https = false;
      rest = url.substr(7);
    } else {
      return url_error_e::scheme;
    }

    // authority: up to the path, query or fragment
    const auto end = rest.find_first_of("/?#");
    const auto authority = end == std::string_view::npos ? rest : rest.substr(0, end);
    if (authority.find('@') != std::string_view::npos) {
      return url_error_e::userinfo;
    }

    std::string_view host_part = authority;
    std::string_view port;
    if (!host_part.empty() && host_part.front() == '[') {
      const auto close = host_part.find(']');
      if (close == std::string_view::npos) {
        return url_error_e::host;
      }
      auto after = host_part.substr(close + 1);
      host_part = host_part.substr(1, close - 1);
      if (!after.empty()) {
        if (after.front() != ':') {
          return url_error_e::host;
        }
        port = after.substr(1);
      }
    } else if (const auto colon = host_part.find(':'); colon != std::string_view::npos) {
      // one colon at most: an IPv6 address needs brackets
      port = host_part.substr(colon + 1);
      host_part = host_part.substr(0, colon);
      if (port.empty() || port.find(':') != std::string_view::npos) {
        return url_error_e::host;
      }
    }

    if (host_part.empty()) {
      return url_error_e::host;
    }
    for (unsigned char c : host_part) {
      if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':')) {
        return url_error_e::host;
      }
    }
    if (!port.empty()) {
      if (port.size() > 5) {
        return url_error_e::host;
      }
      unsigned long number = 0;
      for (unsigned char c : port) {
        if (!std::isdigit(c)) {
          return url_error_e::host;
        }
        number = number * 10 + (c - '0');
      }
      if (number < 1 || number > 65535) {
        return url_error_e::host;
      }
    }

    result.host = std::string(host_part);
    result.port = std::string(port);

    if (!result.https) {
      std::string lower;
      for (unsigned char c : result.host) {
        lower += static_cast<char>(std::tolower(c));
      }
      if (lower != "localhost" && !(host_is_private && host_is_private(result.host))) {
        return url_error_e::http_not_private;
      }
    }

    if (parts) {
      *parts = std::move(result);
    }
    return url_error_e::ok;
  }

  struct event_filter_t {
    std::set<std::string> groups;  ///< known groups that are on
    std::vector<std::string> unknown;  ///< names in the list that are not groups (ignored)
  };

  /// `webhook_events = none`: no group on (an empty value would not be saved by the web UI).
  constexpr std::string_view events_none = "none";

  /**
   * @brief Parse `webhook_events` (comma separated, spaces and case ignored); `none` is skipped,
   * so `none` alone turns every group off without being reported as unknown.
   */
  inline event_filter_t parse_events(std::string_view list) {
    event_filter_t filter;
    std::string current;
    auto flush = [&]() {
      if (current.empty()) {
        return;
      }
      if (known_groups.count(current)) {
        filter.groups.insert(current);
      } else if (current != events_none) {
        filter.unknown.push_back(current);
      }
      current.clear();
    };
    for (unsigned char c : list) {
      if (c == ',') {
        flush();
      } else if (c > ' ') {
        current += static_cast<char>(std::tolower(c));
      }
    }
    flush();
    return filter;
  }

  /// `pairing.succeeded` -> `pairing`; `test` -> `test`.
  inline std::string event_group(std::string_view event) {
    const auto dot = event.find('.');
    return std::string(dot == std::string_view::npos ? event : event.substr(0, dot));
  }

  /// Whether an event passes the filter; `test` always does.
  inline bool event_allowed(const event_filter_t &filter, std::string_view event) {
    const auto group = event_group(event);
    return group == "test" || filter.groups.count(group) > 0;
  }

  /// ISO 8601 in UTC with a `Z` suffix, e.g. `2026-10-12T09:30:00Z`.
  inline std::string iso8601_utc(std::chrono::system_clock::time_point time_point) {
    const auto time = std::chrono::system_clock::to_time_t(time_point);
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    const auto length = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buffer, length);
  }

  /**
   * @brief The event object: `event`, `time` and `host` first, then the event's own fields, then
   * `client_ip` when the caller passes one (only with `webhook_include_client_ip`).
   */
  inline nlohmann::ordered_json make_event(std::string_view event, std::chrono::system_clock::time_point time, std::string_view host, nlohmann::ordered_json fields, std::string_view client_ip = {}) {
    nlohmann::ordered_json body;
    body["event"] = std::string(event);
    body["time"] = iso8601_utc(time);
    body["host"] = std::string(host);
    if (fields.is_object()) {
      for (auto &[key, value] : fields.items()) {
        body[key] = std::move(value);
      }
    }
    if (!client_ip.empty()) {
      body["client_ip"] = std::string(client_ip);
    }
    return body;
  }

  /// Serialize an event body as one line (invalid UTF-8 is replaced, never thrown).
  inline std::string dump_event(const nlohmann::ordered_json &body) {
    return body.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
  }

  /// Lowercase hex of HMAC-SHA256(key, body); the `X-Shell-Signature` header is `sha256=` + this.
  inline std::string hmac_sha256_hex(std::string_view key, std::string_view body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(body.data()), body.size(), digest, &length)) {
      return {};
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
      out += hex[digest[i] >> 4];
      out += hex[digest[i] & 0x0F];
    }
    return out;
  }

  /**
   * @brief A FIFO that holds at most `capacity` items; a push on a full queue drops the oldest.
   * Not thread safe: the owner locks.
   */
  template<class T>
  class bounded_queue_t {
  public:
    explicit bounded_queue_t(std::size_t capacity = queue_capacity):
        _capacity(capacity) {}

    /// @return true when an older item was dropped to make room.
    bool push(T item) {
      bool dropped = false;
      while (_items.size() >= _capacity && !_items.empty()) {
        _items.pop_front();
        dropped = true;
      }
      _items.push_back(std::move(item));
      return dropped;
    }

    bool empty() const {
      return _items.empty();
    }

    std::size_t size() const {
      return _items.size();
    }

    std::size_t capacity() const {
      return _capacity;
    }

    T pop() {
      T item = std::move(_items.front());
      _items.pop_front();
      return item;
    }

    void clear() {
      _items.clear();
    }

  private:
    std::size_t _capacity;
    std::deque<T> _items;
  };

  /// Retry waits after the first and the second failed attempt (connection errors and 5xx only).
  constexpr std::chrono::seconds retry_backoff[] = {std::chrono::seconds(2), std::chrono::seconds(8)};
  constexpr int max_retries = 2;
  constexpr long connect_timeout_s = 5;
  constexpr long total_timeout_s = 10;
}  // namespace webhook::core
