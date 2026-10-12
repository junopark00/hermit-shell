/**
 * @file src/webhook.cpp
 * @brief Shell: webhook notifications; see webhook.h and docs/features.md "Webhooks".
 *
 * One background thread takes events from a bounded queue (webhook::core::queue_capacity) and
 * POSTs each as JSON with libcurl: HTTPS with certificate verification against the Windows
 * certificate store (the linked libcurl uses Schannel), no proxy, no redirects, 5 s to connect and
 * 10 s in all, two retries (2 s, 8 s) on connection errors and 5xx only. Everything other threads
 * do is push into the queue.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <curl/curl.h>

#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"
#include "webhook.h"
#include "webhook_core.h"

using namespace std::literals;

namespace webhook {
  namespace fs = std::filesystem;

  namespace {
    struct event_t {
      std::string name;
      std::string body;  ///< the JSON line
      std::chrono::system_clock::time_point time;
    };

    struct result_t {
      std::string event;
      std::string time;  ///< when the last attempt ended (ISO 8601 UTC)
      bool ok = false;
      long http_code = 0;
      std::string error;  ///< empty on success
      int attempts = 0;
    };

    struct state_t {
      std::mutex mutex;
      std::condition_variable cv;
      bool stop = false;
      bool enabled = false;
      std::string url;
      std::string url_host;
      core::url_error_e url_error = core::url_error_e::empty;
      bool https = true;
      core::event_filter_t filter;
      bool include_client_ip = false;
      std::string secret;
      core::bounded_queue_t<event_t> queue;
      bool overflow_logged = false;  ///< one warning per time the queue fills up
      std::uint64_t sent = 0;
      std::uint64_t failed = 0;
      std::uint64_t dropped = 0;
      std::optional<result_t> last;
      std::string last_failure_logged;  ///< repeated identical failures are logged once
      std::thread worker;
    };

    state_t &state() {
      static state_t instance;
      return instance;
    }

    fs::path secret_path() {
      return fs::path(config::nvhttp.pkey).parent_path() / "webhook_secret";
    }

    std::string read_secret() {
      std::ifstream in(secret_path(), std::ios::binary);
      if (!in) {
        return {};
      }
      std::string secret((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) {
        secret.pop_back();
      }
      return secret;
    }

    /// A literal private, link-local or Tailscale IP address (the classes of net::from_address).
    bool host_is_private(std::string_view host) {
      boost::system::error_code ec;
      const auto address = boost::asio::ip::make_address(std::string(host), ec);
      if (ec) {
        return false;
      }
      try {
        const auto net = net::from_address(address.to_string());
        return net == net::PC || net == net::LAN;
      } catch (...) {
        return false;
      }
    }

    std::size_t discard_body(char *, std::size_t size, std::size_t count, void *) {
      return size * count;
    }

    /// Lets stop() cut a transfer short instead of waiting for its timeout.
    int abort_on_stop(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      auto &st = state();
      std::lock_guard lock {st.mutex};
      return st.stop ? 1 : 0;
    }

    enum class outcome_e {
      ok,
      retry,  ///< connection error or 5xx
      fail,  ///< everything else, including 3xx and 4xx
    };

    struct attempt_t {
      outcome_e outcome;
      long http_code;
      std::string error;
    };

    bool is_connection_error(CURLcode code) {
      switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
          return true;
        default:
          return false;
      }
    }

    attempt_t post(const std::string &url, bool https, const std::string &body, const std::string &secret, std::chrono::system_clock::time_point time) {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return {outcome_e::fail, 0, "couldn't create a CURL handle"};
      }
      auto cleanup = util::fail_guard([curl]() {
        curl_easy_cleanup(curl);
      });

      struct curl_slist *headers = nullptr;
      auto free_headers = util::fail_guard([&headers]() {
        curl_slist_free_all(headers);
      });
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Expect:");
      const auto unix_time = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
      headers = curl_slist_append(headers, ("X-Shell-Timestamp: " + std::to_string(unix_time)).c_str());
      if (!secret.empty()) {
        headers = curl_slist_append(headers, ("X-Shell-Signature: sha256=" + core::hmac_sha256_hex(secret, body)).c_str());
      }

      char error_buffer[CURL_ERROR_SIZE] = {};
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "Shell/" PROJECT_VERSION);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      // Never a proxy, whatever http_proxy/https_proxy/all_proxy say in the environment
      curl_easy_setopt(curl, CURLOPT_PROXY, "");
      // Redirects are never followed: a 3xx answer is a failure (see docs/features.md)
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, core::connect_timeout_s);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, core::total_timeout_s);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_on_stop);
      // Only the scheme the URL was checked with; libcurl must not switch protocols on its own
#if CURL_AT_LEAST_VERSION(7, 85, 0)
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
#else
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS, https ? CURLPROTO_HTTPS : CURLPROTO_HTTP);
#endif
      // TLS: verify the certificate chain and the host name (SNI is sent for host names), TLS 1.2+
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
#ifdef _WIN32
      // The libcurl Shell links uses Schannel, which verifies against the Windows certificate store
      // by itself (NATIVE_CA is for an OpenSSL build and does nothing here). REVOKE_BEST_EFFORT:
      // a revocation server that cannot be reached does not fail the delivery; a certificate known
      // to be revoked still does.
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA | CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif

      const CURLcode code = curl_easy_perform(curl);
      if (code != CURLE_OK) {
        std::string error = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
        if (code == CURLE_ABORTED_BY_CALLBACK) {
          return {outcome_e::fail, 0, "stopped"};
        }
        return {is_connection_error(code) ? outcome_e::retry : outcome_e::fail, 0, std::move(error)};
      }

      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      if (http_code >= 200 && http_code < 300) {
        return {outcome_e::ok, http_code, {}};
      }
      if (http_code >= 300 && http_code < 400) {
        return {outcome_e::fail, http_code, "HTTP " + std::to_string(http_code) + " (redirects are not followed)"};
      }
      if (http_code >= 500) {
        return {outcome_e::retry, http_code, "HTTP " + std::to_string(http_code)};
      }
      return {outcome_e::fail, http_code, "HTTP " + std::to_string(http_code)};
    }

    /// Sleeps `wait`, or less when stop() is called. Expects the lock not held.
    bool wait_unless_stopped(std::chrono::seconds wait) {
      auto &st = state();
      std::unique_lock lock {st.mutex};
      st.cv.wait_for(lock, wait, [&st]() {
        return st.stop;
      });
      return !st.stop;
    }

    void deliver(event_t event) {
      auto &st = state();
      std::string url, secret;
      bool https;
      {
        std::lock_guard lock {st.mutex};
        url = st.url;
        https = st.https;
        secret = st.secret;
      }

      attempt_t attempt {outcome_e::fail, 0, {}};
      int attempts = 0;
      for (;;) {
        ++attempts;
        attempt = post(url, https, event.body, secret, event.time);
        if (attempt.outcome != outcome_e::retry || attempts > core::max_retries) {
          break;
        }
        BOOST_LOG(debug) << "Webhook ["sv << event.name << "] attempt "sv << attempts << " failed ("sv << attempt.error << "); retrying in "sv
                         << core::retry_backoff[attempts - 1].count() << " s"sv;
        if (!wait_unless_stopped(core::retry_backoff[attempts - 1])) {
          attempt = {outcome_e::fail, 0, "stopped"};
          break;
        }
      }

      std::lock_guard lock {st.mutex};
      result_t result;
      result.event = event.name;
      result.time = core::iso8601_utc(std::chrono::system_clock::now());
      result.ok = attempt.outcome == outcome_e::ok;
      result.http_code = attempt.http_code;
      result.error = attempt.error;
      result.attempts = attempts;
      if (result.ok) {
        ++st.sent;
        if (!st.last_failure_logged.empty()) {
          BOOST_LOG(info) << "Webhook ["sv << event.name << "] delivered; the receiver answers again"sv;
          st.last_failure_logged.clear();
        }
        BOOST_LOG(debug) << "Webhook ["sv << event.name << "] delivered (HTTP "sv << result.http_code << ')';
      } else if (attempt.error != "stopped") {
        ++st.failed;
        if (st.last_failure_logged != attempt.error) {
          BOOST_LOG(warning) << "Webhook ["sv << event.name << "] not delivered after "sv << attempts << " attempt(s): "sv << attempt.error
                             << ". The same failure is not logged again until a delivery succeeds."sv;
          st.last_failure_logged = attempt.error;
        } else {
          BOOST_LOG(debug) << "Webhook ["sv << event.name << "] not delivered: "sv << attempt.error;
        }
      }
      st.last = std::move(result);
    }

    void worker_loop() {
      auto &st = state();
      for (;;) {
        event_t event;
        {
          std::unique_lock lock {st.mutex};
          st.cv.wait(lock, [&st]() {
            return st.stop || !st.queue.empty();
          });
          if (st.stop) {
            return;
          }
          event = st.queue.pop();
          if (st.queue.size() < st.queue.capacity() / 2) {
            st.overflow_logged = false;
          }
        }
        deliver(std::move(event));
      }
    }
  }  // namespace

  void start() {
    auto &st = state();
    std::lock_guard lock {st.mutex};
    st.stop = false;
    st.enabled = false;
    st.url = config::shell.webhook_url;
    st.include_client_ip = config::shell.webhook_include_client_ip;
    st.filter = core::parse_events(config::shell.webhook_events);
    st.secret = read_secret();

    core::url_parts_t parts;
    st.url_error = core::validate_url(st.url, config::shell.webhook_allow_http_private, host_is_private, &parts);
    if (st.url_error == core::url_error_e::empty) {
      BOOST_LOG(debug) << "Webhooks are off (no webhook_url)"sv;
      return;
    }
    if (st.url_error != core::url_error_e::ok) {
      BOOST_LOG(warning) << "Webhooks are off: webhook_url is not accepted ("sv << core::url_error_text(st.url_error) << ")"sv;
      return;
    }
    st.url_host = parts.host;
    st.https = parts.https;
    for (const auto &name : st.filter.unknown) {
      BOOST_LOG(warning) << "webhook_events: unknown event group ["sv << name << "] ignored (known: pairing, stream, app)"sv;
    }
    if (st.filter.groups.empty()) {
      BOOST_LOG(warning) << "webhook_events selects no event group; only test events will be sent"sv;
    }

    st.enabled = true;
    st.worker = std::thread(worker_loop);
    BOOST_LOG(info) << "Webhooks on: POST to ["sv << (st.https ? "https://"sv : "http://"sv) << st.url_host << "] for ["sv << config::shell.webhook_events
                    << "], signature "sv << (st.secret.empty() ? "off"sv : "on"sv) << ", client IP "sv << (st.include_client_ip ? "included"sv : "not included"sv);
  }

  void stop() {
    auto &st = state();
    {
      std::lock_guard lock {st.mutex};
      st.stop = true;
      st.enabled = false;
      st.queue.clear();
    }
    st.cv.notify_all();
    if (st.worker.joinable()) {
      st.worker.join();
    }
  }

  void enqueue(const std::string &event, nlohmann::ordered_json fields, const std::string &client_address) {
    auto &st = state();
    try {
      std::lock_guard lock {st.mutex};
      if (!st.enabled || !core::event_allowed(st.filter, event)) {
        return;
      }
      const auto now = std::chrono::system_clock::now();
      auto body = core::make_event(event, now, config::nvhttp.shell_name, std::move(fields), st.include_client_ip ? client_address : std::string {});
      if (st.queue.push(event_t {event, core::dump_event(body), now})) {
        ++st.dropped;
        if (!st.overflow_logged) {
          BOOST_LOG(warning) << "Webhook queue full ("sv << st.queue.capacity() << " events); the oldest events are dropped until the receiver catches up"sv;
          st.overflow_logged = true;
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Webhook ["sv << event << "] not queued: "sv << e.what();
      return;
    }
    st.cv.notify_one();
  }

  void pairing_succeeded(const std::string &client_name, const std::string &client_address) {
    enqueue("pairing.succeeded", {{"client", client_name}}, client_address);
  }

  void pairing_failed(const std::string &client_name, const std::string &reason, const std::string &client_address) {
    enqueue("pairing.failed", {{"client", client_name}, {"reason", reason}}, client_address);
  }

  void app_started(const std::string &app_name) {
    enqueue("app.started", {{"app", app_name}});
  }

  void app_ended(const std::string &app_name) {
    enqueue("app.ended", {{"app", app_name}});
  }

  std::string save_secret(const std::string &secret) {
    auto &st = state();
    const auto path = secret_path();
    std::error_code ec;
    if (secret.empty()) {
      fs::remove(path, ec);
      if (ec && fs::exists(path)) {
        return "couldn't remove " + path.string() + ": " + ec.message();
      }
    } else {
      // The credentials folder keeps the file readable by Administrators and SYSTEM only
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << secret;
      out.flush();
      if (!out) {
        return "couldn't write " + path.string();
      }
    }
    std::lock_guard lock {st.mutex};
    st.secret = secret;
    BOOST_LOG(info) << "Webhook secret "sv << (secret.empty() ? "removed"sv : "saved"sv);
    return {};
  }

  std::string send_test() {
    {
      auto &st = state();
      std::lock_guard lock {st.mutex};
      if (!st.enabled) {
        return st.url_error == core::url_error_e::empty ? "Webhooks are off: save a URL and apply first" : std::string("Webhooks are off: ") + core::url_error_text(st.url_error);
      }
    }
    enqueue("test", {{"message", "Test event from the Shell web UI"}});
    return {};
  }

  nlohmann::json status() {
    auto &st = state();
    std::lock_guard lock {st.mutex};
    nlohmann::json out;
    out["enabled"] = st.enabled;
    out["url_error"] = st.url_error == core::url_error_e::ok ? nlohmann::json(nullptr) : nlohmann::json(core::url_error_text(st.url_error));
    out["host"] = st.url_host;
    out["secret_set"] = !st.secret.empty();
    out["events"] = st.filter.groups;
    out["queued"] = st.queue.size();
    out["sent"] = st.sent;
    out["failed"] = st.failed;
    out["dropped"] = st.dropped;
    if (st.last) {
      out["last"] = {
        {"event", st.last->event},
        {"time", st.last->time},
        {"ok", st.last->ok},
        {"http_code", st.last->http_code},
        {"error", st.last->error},
        {"attempts", st.last->attempts},
      };
    } else {
      out["last"] = nullptr;
    }
    return out;
  }
}  // namespace webhook
