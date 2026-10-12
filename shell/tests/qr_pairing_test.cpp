// Standalone checks for src/qr_pairing.h (QR pairing): the base32 token (26 upper-case characters,
// round trip, one spelling per value), the record id against vectors from Python's hashlib, the
// certificate fingerprint against `openssl x509 -fingerprint -sha256`, the link (key order,
// percent-encoding, the 320-byte cut of `name`), the host rule and the record state machine in
// the order of its checks (none, network, expired, used; binding, release and completion by the
// bound address only). No network.
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/qr_pairing_test.cpp -lcrypto -o build/qr_pairing_test.exe
//   build/qr_pairing_test.exe [cert.pem expected-fingerprint]
// The optional arguments check cert_fingerprint against a certificate file and the hex digest
// `openssl x509 -in cert.pem -noout -fingerprint -sha256` prints (colons and case do not matter).
#include "qr_pairing.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace qr = nvhttp::qr;
using namespace std::chrono_literals;

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

static std::string bytes_token(std::initializer_list<int> bytes) {
  std::vector<std::uint8_t> raw;
  for (int b : bytes) {
    raw.push_back(static_cast<std::uint8_t>(b));
  }
  return qr::base32_encode(raw.data(), raw.size());
}

int main(int argc, char **argv) {
  const auto t0 = qr::clock::now();

  std::printf("base32 token\n");
  {
    std::uint8_t counting[16];
    for (int i = 0; i < 16; ++i) {
      counting[i] = static_cast<std::uint8_t>(i);
    }
    auto token = qr::token_from_bytes(counting);
    // Python: base64.b32encode(bytes(range(16))).rstrip(b'=')
    check("16 counting bytes", token == "AAAQEAYEAUDAOCAJBIFQYDIOB4");
    check("26 characters", token.size() == qr::token_chars);
    check("token_valid accepts it", qr::token_valid(token));
    std::uint8_t ones[16];
    for (auto &b : ones) {
      b = 0xFF;
    }
    check("16 0xFF bytes", qr::token_from_bytes(ones) == "77777777777777777777777774");
    check("a3f1c4d2e5b60718293a4b5c6d7e8f90", bytes_token({0xa3, 0xf1, 0xc4, 0xd2, 0xe5, 0xb6, 0x07, 0x18, 0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90}) == "UPY4JUXFWYDRQKJ2JNOG27UPSA");
    check("RFC 4648 \"foobar\"", qr::base32_encode(reinterpret_cast<const std::uint8_t *>("foobar"), 6) == "MZXW6YTBOI");

    std::vector<std::uint8_t> back;
    check("decodes", qr::base32_decode(token, back) && back.size() == 16 && std::equal(back.begin(), back.end(), counting));
    check("decodes 0xFF", qr::base32_decode("77777777777777777777777774", back) && back.size() == 16 && back[15] == 0xFF);
    check("rejects a character outside the alphabet", !qr::base32_decode("AAAQEAYEAUDAOCAJBIFQYDIOB1", back));
    check("rejects lower case (clients upper-case first)", !qr::base32_decode("aaaqeayeaudaocajbifqydiob4", back));
    check("rejects non-zero padding bits (one spelling per value)", !qr::base32_decode("AAAQEAYEAUDAOCAJBIFQYDIOB5", back));
    check("token_valid: 25 characters", !qr::token_valid("AAAQEAYEAUDAOCAJBIFQYDIOB"));
    check("token_valid: digit 1", !qr::token_valid("AAAQEAYEAUDAOCAJBIFQYDIO11"));
    check("token_valid: lower case", !qr::token_valid("aaaqeayeaudaocajbifqydiob4"));
  }

  std::printf("record id\n");
  {
    // Python: hashlib.sha256(b'hermit-qr-id' + token).hexdigest()[:16]
    check("id of AAAQEAYEAUDAOCAJBIFQYDIOB4", qr::token_id("AAAQEAYEAUDAOCAJBIFQYDIOB4") == "34d0f11c23c59e23");
    check("id of 77777777777777777777777774", qr::token_id("77777777777777777777777774") == "d5966489ad3efe54");
    check("id of UPY4JUXFWYDRQKJ2JNOG27UPSA", qr::token_id("UPY4JUXFWYDRQKJ2JNOG27UPSA") == "2f70206d4958f005");
    check("16 hex characters", qr::token_id("X").size() == qr::id_chars);
    check("sha256 of empty input", qr::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // the domain prefix: the id is not a prefix of the key derivation hash of the bare token
    check("domain separated", qr::token_id("AAAQEAYEAUDAOCAJBIFQYDIOB4") != qr::sha256_hex("AAAQEAYEAUDAOCAJBIFQYDIOB4").substr(0, 16));
  }

  std::printf("certificate fingerprint\n");
  {
    check("garbage PEM gives empty", qr::cert_fingerprint("not a certificate").empty());
    if (argc >= 3) {
      std::ifstream in(argv[1], std::ios::binary);
      std::stringstream pem;
      pem << in.rdbuf();
      std::string expected;
      for (const char *p = argv[2]; *p; ++p) {
        if (*p != ':') {
          expected.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
        }
      }
      auto got = qr::cert_fingerprint(pem.str());
      check("64 hex characters", got.size() == 64);
      check("equals openssl x509 -fingerprint -sha256", got == expected);
    } else {
      std::printf("  (skip) pass cert.pem and the openssl fingerprint to check cert_fingerprint\n");
    }
  }

  std::printf("link\n");
  {
    const std::string fp(64, 'a');
    const std::string token = "AAAQEAYEAUDAOCAJBIFQYDIOB4";
    auto link = qr::build_link("192.168.0.10", 47989, fp, token, "Juno PC");
    check("key order and encoding", link == "hermit://pair?v=1&host=192.168.0.10&port=47989&fp=" + fp + "&token=" + token + "&name=Juno%20PC");
    check("percent-encoding keeps unreserved only", qr::percent_encode("A-z._~0 /?&=%\xEA\xB0\x80") == "A-z._~0%20%2F%3F%26%3D%25%EA%B0%80");
    check("bracketed IPv6 host as is", qr::build_link("[fd00::1]", 47989, fp, token, "").find("&host=[fd00::1]&port=") != std::string::npos);
    check("empty name allowed", qr::build_link("h", 1, fp, token, "").ends_with("&name="));

    // name: 32 characters at most, counted in code points
    std::string long_name(40, 'n');
    auto cut = qr::build_link("h", 47989, fp, token, long_name);
    check("name cut to 32 characters", cut.ends_with("&name=" + std::string(32, 'n')));
    std::string hangul;
    for (int i = 0; i < 33; ++i) {
      hangul += "\xEA\xB0\x80";  // 가
    }
    auto cut_hangul = qr::build_link("h", 47989, fp, token, hangul);
    // each 가 is 9 bytes encoded; 32 of them do not fit in 320 bytes, so the name is cut further,
    // whole characters at a time
    const auto name_at = cut_hangul.find("&name=") + 6;
    const auto room = (qr::link_max_bytes - name_at) / 9;
    check("name cut on character boundaries", cut_hangul.size() == name_at + room * 9 && room < 32 && cut_hangul.ends_with("%EA%B0%80"));

    // 320 bytes: a 63-character host name and a 32-character Korean name do not fit, so the name
    // gives way until the link fits
    std::string host63(63, 'h');
    std::string hangul32;
    for (int i = 0; i < 32; ++i) {
      hangul32 += "\xEA\xB0\x80";
    }
    auto bounded = qr::build_link(host63, 47989, fp, token, hangul32);
    check("link at most 320 bytes", bounded.size() <= qr::link_max_bytes);
    check("link keeps as much of the name as fits", bounded.size() > qr::link_max_bytes - 9);
    // a 253-character host alone passes 320 bytes: only the name gives way, down to nothing
    check("the name gives way entirely for a long host", qr::build_link(std::string(253, 'h'), 65535, fp, token, hangul32).ends_with("&name="));
    check("short link is not cut", link.size() < qr::link_max_bytes && link.ends_with("Juno%20PC"));
  }

  std::printf("host rule\n");
  {
    check("IPv4", qr::valid_host("192.168.0.10"));
    check("Tailscale IPv4", qr::valid_host("100.64.1.2"));
    check("bracketed IPv6", qr::valid_host("[fd00::1]"));
    check("host name", qr::valid_host("desktop-01.lan"));
    check("empty", !qr::valid_host(""));
    check("zone id", !qr::valid_host("[fe80::1%eth0]"));
    check("space", !qr::valid_host("192.168.0.10 "));
    check("query character", !qr::valid_host("host&port=1"));
    check("254 characters", !qr::valid_host(std::string(254, 'a')));
    check("253 characters", qr::valid_host(std::string(253, 'a')));
  }

  std::printf("state machine: check order\n");
  {
    qr::record_t none;
    check("no record: none", qr::check(none, "0123456789abcdef", true, "192.168.0.20", t0) == qr::refusal_e::none);

    auto r = qr::make_record("AAAQEAYEAUDAOCAJBIFQYDIOB4", 0x071F1F00, "127.0.0.1", t0);
    check("fresh record is pending", r.state == qr::state_e::pending && r.id == "34d0f11c23c59e23" && r.token == "AAAQEAYEAUDAOCAJBIFQYDIOB4");
    check("wrong id: none (before the network check)", qr::check(r, "ffffffffffffffff", false, "8.8.8.8", t0) == qr::refusal_e::none);
    check("WAN address: network", qr::check(r, r.id, false, "8.8.8.8", t0) == qr::refusal_e::network);
    check("WAN address before expiry", qr::check(r, r.id, false, "8.8.8.8", t0 + 121s) == qr::refusal_e::network);
    check("120 s: still valid", !qr::check(r, r.id, true, "192.168.0.20", t0 + 120s));
    check("after 120 s: expired", qr::check(r, r.id, true, "192.168.0.20", t0 + 120s + 1ms) == qr::refusal_e::expired);
    check("pending from LAN passes", !qr::check(r, r.id, true, "192.168.0.20", t0 + 10s));

    qr::bind(r, "uid-A", "192.168.0.20");
    check("bound: in_use", r.state == qr::state_e::in_use && r.bound_uniqueid == "uid-A" && r.bound_address == "192.168.0.20");
    check("in_use from another address: used", qr::check(r, r.id, true, "192.168.0.21", t0 + 11s) == qr::refusal_e::used);
    check("in_use from the same address passes (retry replaces)", !qr::check(r, r.id, true, "192.168.0.20", t0 + 11s));
    check("expired wins over used", qr::check(r, r.id, true, "192.168.0.21", t0 + 200s) == qr::refusal_e::expired);
    check("WAN wins over used", qr::check(r, r.id, false, "1.2.3.4", t0 + 11s) == qr::refusal_e::network);

    // release: the bound address and uniqueid only
    auto held = r;
    check("another address cannot release", !qr::release(held, "uid-A", "192.168.0.21") && held.state == qr::state_e::in_use);
    check("another uniqueid cannot release", !qr::release(held, "uid-B", "192.168.0.20") && held.state == qr::state_e::in_use);
    check("the holder releases: pending again", qr::release(held, "uid-A", "192.168.0.20") && held.state == qr::state_e::pending && held.bound_uniqueid.empty() && held.bound_address.empty());
    check("released record passes again from a new address", !qr::check(held, held.id, true, "192.168.0.21", t0 + 12s));
    check("pending cannot be released", !qr::release(held, "uid-A", "192.168.0.20"));

    // completion: the holder only; then used for everyone
    auto done = r;
    check("another uniqueid cannot complete", !qr::complete(done, "uid-B", "Phone", t0 + 20s));
    check("the holder completes: used", qr::complete(done, "uid-A", "Phone", t0 + 20s) && done.state == qr::state_e::used && done.paired_name == "Phone");
    check("used from the same address: used", qr::check(done, done.id, true, "192.168.0.20", t0 + 21s) == qr::refusal_e::used);
    check("used from another address: used", qr::check(done, done.id, true, "192.168.0.21", t0 + 21s) == qr::refusal_e::used);
    check("used cannot be released", !qr::release(done, "uid-A", "192.168.0.20") && done.state == qr::state_e::used);
    check("used cannot expire", !qr::expire(done) && done.state == qr::state_e::used);

    // expiry by drop_stale_sessions
    auto old = r;
    check("in_use expires", qr::expire(old) && old.state == qr::state_e::expired);
    check("expired: expired even within the lifetime", qr::check(old, old.id, true, "192.168.0.20", t0 + 30s) == qr::refusal_e::expired);
    check("expired cannot be released", !qr::release(old, "uid-A", "192.168.0.20"));
    qr::record_t fresh = qr::make_record("77777777777777777777777774", 0x03000000, "127.0.0.1", t0);
    check("pending expires", qr::expire(fresh) && fresh.state == qr::state_e::expired);
  }

  std::printf("state machine: web UI view\n");
  {
    qr::record_t none;
    check("none", qr::visible_state(none, t0) == qr::state_e::none && qr::remaining_seconds(none, t0) == 0);
    auto r = qr::make_record("UPY4JUXFWYDRQKJ2JNOG27UPSA", 0x07001F00, "127.0.0.1", t0);
    check("pending, 120 s left", qr::visible_state(r, t0) == qr::state_e::pending && qr::remaining_seconds(r, t0) == 120);
    check("pending, 90 s left", qr::remaining_seconds(r, t0 + 30s) == 90);
    check("pending past the lifetime shows expired", qr::visible_state(r, t0 + 121s) == qr::state_e::expired && qr::remaining_seconds(r, t0 + 121s) == 0);
    qr::bind(r, "uid-A", "192.168.0.20");
    check("in_use", qr::visible_state(r, t0 + 40s) == qr::state_e::in_use && qr::remaining_seconds(r, t0 + 40s) == 80);
    check("in_use past the lifetime shows expired", qr::visible_state(r, t0 + 130s) == qr::state_e::expired);
    qr::complete(r, "uid-A", "Phone", t0 + 50s);
    check("used, no time left", qr::visible_state(r, t0 + 51s) == qr::state_e::used && qr::remaining_seconds(r, t0 + 51s) == 0);
    check("used shown for 60 s", qr::visible_state(r, t0 + 110s) == qr::state_e::used);
    check("used gone after 60 s", qr::visible_state(r, t0 + 111s) == qr::state_e::none);
    check("state names", qr::state_name(qr::state_e::in_use) == "in_use" && qr::state_name(qr::state_e::none) == "none");
  }

  std::printf("refusal texts\n");
  {
    check("none", qr::reason(qr::refusal_e::none) == "none" && qr::message(qr::refusal_e::none) == "QR_none: No QR pairing is pending; create one in the web UI");
    check("network", qr::reason(qr::refusal_e::network) == "network" && qr::message(qr::refusal_e::network) == "QR_network: QR pairing is accepted from the local network only");
    check("expired", qr::reason(qr::refusal_e::expired) == "expired" && qr::message(qr::refusal_e::expired) == "QR_expired: QR code expired; create a new one");
    check("used", qr::reason(qr::refusal_e::used) == "used" && qr::message(qr::refusal_e::used) == "QR_used: QR code already used; create a new one");
    for (auto why : {qr::refusal_e::none, qr::refusal_e::network, qr::refusal_e::expired, qr::refusal_e::used}) {
      auto msg = std::string {qr::message(why)};
      auto prefix = "QR_" + std::string {qr::reason(why)} + ": ";
      check("message starts with QR_<reason>: ", msg.starts_with(prefix));
    }
  }

  std::printf("preset names\n");
  {
    check("all", qr::preset_name(0x071F1F00) == "all");
    check("stream", qr::preset_name(0x07001F00) == "stream");
    check("view", qr::preset_name(0x03000000) == "view");
    check("custom", qr::preset_name(0x07000000) == "custom");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
