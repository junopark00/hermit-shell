// Standalone checks for src/webhook_core.h (webhook notifications): the URL rules (https only,
// http for a private IP or localhost on request, no userinfo, no spaces), the event list, the event
// JSON shape, HMAC-SHA256 against the RFC 4231 vector and the bounded queue. No network.
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/webhook_test.cpp -lcrypto -o build/webhook_test.exe
//   build/webhook_test.exe
#include "webhook_core.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace core = webhook::core;

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok) {
  if (ok) {
    ++g_pass;
    std::printf("  ok   %s\n", name);
  } else {
    ++g_fail;
    std::printf("  FAIL %s\n", name);
  }
}

// Stands in for net::from_address: literal private, link-local and Tailscale IPv4 addresses.
static bool fake_private(std::string_view host) {
  // a literal address only (net::from_address gets an address, never a name)
  for (unsigned char c : host) {
    if (!(std::isxdigit(c) || c == '.' || c == ':')) {
      return false;
    }
  }
  return host.starts_with("10.") || host.starts_with("192.168.") || host.starts_with("100.64.") || host.starts_with("172.16.") || host.starts_with("169.254.") || host == "fd00::1" || host == "127.0.0.1";
}

int main() {
  using E = core::url_error_e;
  std::printf("validate_url\n");
  {
    core::url_parts_t parts;
    check("https URL accepted", core::validate_url("https://hooks.example.com/shell?x=1", false, fake_private, &parts) == E::ok);
    check("  host parsed", parts.host == "hooks.example.com" && parts.port.empty() && parts.https);
    check("https with port", core::validate_url("https://hooks.example.com:8443/a", false, fake_private, &parts) == E::ok && parts.port == "8443");
    check("scheme case ignored", core::validate_url("HTTPS://hooks.example.com/", false, fake_private) == E::ok);
    check("empty is 'off'", core::validate_url("", false, fake_private) == E::empty);
    check("http refused by default", core::validate_url("http://192.168.1.10:8080/hook", false, fake_private) == E::scheme);
    check("http private IP accepted with the option", core::validate_url("http://192.168.1.10:8080/hook", true, fake_private, &parts) == E::ok && !parts.https && parts.host == "192.168.1.10");
    check("http Tailscale IP accepted with the option", core::validate_url("http://100.64.3.4/hook", true, fake_private) == E::ok);
    check("http localhost accepted with the option", core::validate_url("http://localhost:9000/", true, fake_private) == E::ok);
    check("http LOCALHOST accepted with the option", core::validate_url("http://LOCALHOST/", true, fake_private) == E::ok);
    check("http bracketed private IPv6 accepted", core::validate_url("http://[fd00::1]:8080/x", true, fake_private, &parts) == E::ok && parts.host == "fd00::1" && parts.port == "8080");
    check("http public host refused even with the option", core::validate_url("http://hooks.example.com/", true, fake_private) == E::http_not_private);
    check("http public IP refused even with the option", core::validate_url("http://8.8.8.8/", true, fake_private) == E::http_not_private);
    check("http host name that looks private refused (not a literal IP)", core::validate_url("http://10.example.com/", true, fake_private) == E::http_not_private);
    check("ftp refused", core::validate_url("ftp://hooks.example.com/", true, fake_private) == E::scheme);
    check("no scheme refused", core::validate_url("hooks.example.com/hook", true, fake_private) == E::scheme);
    check("userinfo refused", core::validate_url("https://user:pw@hooks.example.com/", false, fake_private) == E::userinfo);
    check("userinfo without password refused", core::validate_url("https://user@hooks.example.com/", false, fake_private) == E::userinfo);
    check("@ in the path is fine", core::validate_url("https://hooks.example.com/a@b", false, fake_private) == E::ok);
    check("space refused", core::validate_url("https://hooks.example.com/a b", false, fake_private) == E::bad_char);
    check("control character refused", core::validate_url("https://hooks.example.com/a\nb", false, fake_private) == E::bad_char);
    check("empty host refused", core::validate_url("https:///hook", false, fake_private) == E::host);
    check("bad port refused", core::validate_url("https://hooks.example.com:80x/", false, fake_private) == E::host);
    check("port 99999 refused", core::validate_url("https://hooks.example.com:99999/", false, fake_private) == E::host);
    check("port 0 refused", core::validate_url("https://hooks.example.com:0/", false, fake_private) == E::host);
    check("port 65535 accepted", core::validate_url("https://hooks.example.com:65535/", false, fake_private) == E::ok);
    check("empty port after a colon refused", core::validate_url("https://hooks.example.com:/", false, fake_private) == E::host);
    check("unbracketed IPv6 refused", core::validate_url("https://::1/x", false, fake_private) == E::host);
    check("two colons in a host refused", core::validate_url("https://host:1:2/", false, fake_private) == E::host);
    check("bad host character refused", core::validate_url("https://hooks_example.com/", false, fake_private) == E::host);
    check("unclosed bracket refused", core::validate_url("https://[fd00::1/", false, fake_private) == E::host);
    check("error text exists", std::string(core::url_error_text(E::http_not_private)).find("http://") != std::string::npos);
  }

  std::printf("parse_events\n");
  {
    auto f = core::parse_events("pairing,stream,app");
    check("default list: three groups", f.groups.size() == 3 && f.unknown.empty());
    f = core::parse_events(" Stream , APP ");
    check("spaces and case ignored", f.groups == std::set<std::string> {"app", "stream"});
    f = core::parse_events("stream,bogus,,app");
    check("unknown names reported, empty items skipped", f.groups.size() == 2 && f.unknown == std::vector<std::string> {"bogus"});
    f = core::parse_events("");
    check("empty list: no groups", f.groups.empty());
    f = core::parse_events("none");
    check("none: no groups, not unknown", f.groups.empty() && f.unknown.empty());
    f = core::parse_events("none,stream");
    check("none with a group: the group wins", f.groups == std::set<std::string> {"stream"} && f.unknown.empty());
    check("event_group", core::event_group("pairing.succeeded") == "pairing" && core::event_group("test") == "test");
    const auto only_stream = core::parse_events("stream");
    check("stream.ended allowed", core::event_allowed(only_stream, "stream.ended"));
    check("pairing.failed not allowed", !core::event_allowed(only_stream, "pairing.failed"));
    check("test always allowed", core::event_allowed(core::parse_events(""), "test"));
  }

  std::printf("make_event\n");
  {
    const auto time = std::chrono::system_clock::time_point(std::chrono::seconds(1791798600));  // 2026-10-12T09:50:00Z
    check("iso8601_utc", core::iso8601_utc(time) == "2026-10-12T09:50:00Z");
    nlohmann::ordered_json fields;
    fields["client"] = "Tablet";
    fields["app"] = "Desktop";
    const auto body = core::make_event("stream.started", time, "GAMING-PC", fields);
    const auto line = core::dump_event(body);
    check("key order: event, time, host, then fields", line == R"({"event":"stream.started","time":"2026-10-12T09:50:00Z","host":"GAMING-PC","client":"Tablet","app":"Desktop"})");
    check("no client_ip without one", !body.contains("client_ip"));
    const auto with_ip = core::make_event("pairing.failed", time, "GAMING-PC", {{"client", "Phone"}, {"reason", "No PIN was entered in time"}}, "192.168.1.20");
    check("client_ip last when given", core::dump_event(with_ip) == R"({"event":"pairing.failed","time":"2026-10-12T09:50:00Z","host":"GAMING-PC","client":"Phone","reason":"No PIN was entered in time","client_ip":"192.168.1.20"})");
    const auto no_fields = core::make_event("test", time, "GAMING-PC", nullptr);
    check("null fields tolerated", core::dump_event(no_fields) == R"({"event":"test","time":"2026-10-12T09:50:00Z","host":"GAMING-PC"})");
    const auto bad_utf8 = core::make_event("test", time, "GAMING-PC", {{"message", std::string("\xff" "x")}});
    check("invalid UTF-8 replaced, not thrown", core::dump_event(bad_utf8).find("\xEF\xBF\xBD") != std::string::npos);
  }

  std::printf("hmac_sha256_hex\n");
  {
    // RFC 4231 test case 2
    check("RFC 4231 case 2", core::hmac_sha256_hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 4231 test case 1: key of 20 bytes 0x0b, data "Hi There"
    check("RFC 4231 case 1", core::hmac_sha256_hex(std::string(20, '\x0b'), "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    check("different key, different signature", core::hmac_sha256_hex("other", "what do ya want for nothing?") != core::hmac_sha256_hex("Jefe", "what do ya want for nothing?"));
    check("64 hex characters", core::hmac_sha256_hex("k", "").size() == 64);
  }

  std::printf("bounded_queue\n");
  {
    core::bounded_queue_t<int> q(core::queue_capacity);
    check("capacity 64", q.capacity() == 64);
    bool dropped = false;
    for (int i = 0; i < 64; ++i) {
      dropped = q.push(i) || dropped;
    }
    check("64 pushes without a drop", !dropped && q.size() == 64);
    check("65th push drops the oldest", q.push(64) && q.size() == 64);
    check("front is 1 now", q.pop() == 1 && q.size() == 63);
    for (int i = 0; i < 62; ++i) {
      q.pop();
    }
    check("last is the newest", q.pop() == 64 && q.empty());
    core::bounded_queue_t<std::string> small(2);
    small.push("a");
    small.push("b");
    check("small queue drop", small.push("c") && small.size() == 2 && small.pop() == "b");
    small.clear();
    check("clear", small.empty());
    check("retry constants", core::max_retries == 2 && core::retry_backoff[0].count() == 2 && core::retry_backoff[1].count() == 8 && core::connect_timeout_s == 5 && core::total_timeout_s == 10);
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
