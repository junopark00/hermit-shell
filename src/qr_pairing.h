/**
 * @file src/qr_pairing.h
 * @brief Shell: the pure parts of QR pairing (docs/features.md "QR pairing"): the token alphabet,
 * the record id, the link, the host rule and the record state machine with its check order. No
 * logging, no network and no Shell globals, so shell/tests/qr_pairing_test.cpp can build it alone.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace nvhttp::qr {
  using clock = std::chrono::steady_clock;

  /// How long a QR code can be used, from its creation; the host decides, the link carries no time.
  constexpr auto lifetime = std::chrono::seconds(120);
  /// How long a used record stays visible to the web UI (state `used` with the device name).
  constexpr auto used_display = std::chrono::seconds(60);
  /// The token: 16 random bytes, shown as 26 base32 characters (RFC 4648, upper case, no padding).
  constexpr std::size_t token_bytes = 16;
  constexpr std::size_t token_chars = 26;
  /// The record id: the first 16 hex characters of SHA-256(id_domain ‖ token). It travels over
  /// plain HTTP in place of the token; the domain prefix keeps it apart from the key derivation
  /// hash SHA-256(salt ‖ token).
  constexpr std::string_view id_domain = "hermit-qr-id";
  constexpr std::size_t id_chars = 16;
  /// A link longer than this gets its `name` cut (QR version 13 at error correction M holds 331 bytes).
  constexpr std::size_t link_max_bytes = 320;
  /// `name` is the host name, at most this many characters before encoding.
  constexpr std::size_t name_max_chars = 32;
  /// `host` as typed or chosen in the web UI, at most this many characters (a DNS name's limit).
  constexpr std::size_t host_max_chars = 253;

  constexpr std::string_view base32_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

  /// RFC 4648 base32 of `bytes`, upper case, without padding.
  inline std::string base32_encode(const std::uint8_t *bytes, std::size_t size) {
    std::string out;
    out.reserve((size * 8 + 4) / 5);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i) {
      buffer = (buffer << 8) | bytes[i];
      bits += 8;
      while (bits >= 5) {
        out.push_back(base32_alphabet[(buffer >> (bits - 5)) & 0x1F]);
        bits -= 5;
      }
    }
    if (bits > 0) {
      out.push_back(base32_alphabet[(buffer << (5 - bits)) & 0x1F]);
    }
    return out;
  }

  /// The reverse of base32_encode: false for a character outside the alphabet or padding bits
  /// that are not zero (so that each byte string has one spelling).
  inline bool base32_decode(std::string_view text, std::vector<std::uint8_t> &bytes) {
    bytes.clear();
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
      auto pos = base32_alphabet.find(c);
      if (pos == std::string_view::npos) {
        return false;
      }
      buffer = (buffer << 5) | static_cast<std::uint32_t>(pos);
      bits += 5;
      if (bits >= 8) {
        bytes.push_back(static_cast<std::uint8_t>((buffer >> (bits - 8)) & 0xFF));
        bits -= 8;
      }
    }
    return bits < 5 && (buffer & ((1u << bits) - 1)) == 0;
  }

  /// Whether `token` is 26 characters of the base32 alphabet (what a client accepts after upper-casing).
  inline bool token_valid(std::string_view token) {
    if (token.size() != token_chars) {
      return false;
    }
    for (char c : token) {
      if (base32_alphabet.find(c) == std::string_view::npos) {
        return false;
      }
    }
    return true;
  }

  /// The token for 16 random bytes.
  inline std::string token_from_bytes(const std::uint8_t *bytes) {
    return base32_encode(bytes, token_bytes);
  }

  inline std::string hex_lower(const std::uint8_t *bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
      out.push_back(digits[bytes[i] >> 4]);
      out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
  }

  /// SHA-256 of `data` as 64 lower-case hex characters.
  inline std::string sha256_hex(std::string_view data) {
    std::uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha256(), nullptr);
    return hex_lower(digest, size);
  }

  /**
   * @brief The `fp` of the link: SHA-256 of the DER form of a PEM certificate, 64 lower-case hex
   * characters (what `openssl x509 -fingerprint -sha256` prints, without the colons). Empty when
   * the PEM does not parse.
   */
  inline std::string cert_fingerprint(std::string_view pem) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
      return {};
    }
    X509 *x509 = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!x509) {
      return {};
    }
    std::uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    std::string out;
    if (X509_digest(x509, EVP_sha256(), digest, &size) == 1) {
      out = hex_lower(digest, size);
    }
    X509_free(x509);
    return out;
  }

  /// The record id of `token` (see id_domain). Both sides compute it; the link does not carry it.
  inline std::string token_id(std::string_view token) {
    std::string input;
    input.reserve(id_domain.size() + token.size());
    input.append(id_domain);
    input.append(token);
    return sha256_hex(input).substr(0, id_chars);
  }

  /// RFC 3986 percent-encoding: the unreserved characters stay, every other byte becomes %XX.
  inline std::string percent_encode(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0F]);
      }
    }
    return out;
  }

  /// The first `count` UTF-8 code points of `text` (a cut never splits a character).
  inline std::string_view utf8_prefix(std::string_view text, std::size_t count) {
    std::size_t i = 0;
    std::size_t seen = 0;
    while (i < text.size() && seen < count) {
      unsigned char c = static_cast<unsigned char>(text[i]);
      std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
      if (i + len > text.size()) {
        break;
      }
      i += len;
      ++seen;
    }
    return text.substr(0, i);
  }

  /// The number of UTF-8 code points in `text`.
  inline std::size_t utf8_length(std::string_view text) {
    std::size_t n = 0;
    for (unsigned char c : text) {
      if ((c & 0xC0) != 0x80) {
        ++n;
      }
    }
    return n;
  }

  /**
   * @brief Whether `host` may go into the link: not empty, at most host_max_chars, and only the
   * characters of an IPv4 address, a bracketed IPv6 address or a host name (letters, digits, `.`,
   * `-`, `:`, `[`, `]`). `%` (a zone id) and anything else are refused.
   */
  inline bool valid_host(std::string_view host) {
    if (host.empty() || host.size() > host_max_chars) {
      return false;
    }
    for (unsigned char c : host) {
      bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief The pairing link `hermit://pair?v=1&host=&port=&fp=&token=&name=`, keys in this order.
   * `name` is cut to name_max_chars characters, and further until the link fits link_max_bytes.
   * @param host As valid_host accepts it (not encoded).
   * @param port The HTTP port of nvhttp.
   * @param fingerprint SHA-256 of the DER server certificate, 64 lower-case hex characters.
   * @param token The 26-character token.
   * @param name The host name shown by the client (UTF-8).
   */
  inline std::string build_link(std::string_view host, std::uint16_t port, std::string_view fingerprint, std::string_view token, std::string_view name) {
    std::string base = "hermit://pair?v=1&host=";
    base.append(host);
    base.append("&port=");
    base.append(std::to_string(port));
    base.append("&fp=");
    base.append(fingerprint);
    base.append("&token=");
    base.append(token);
    base.append("&name=");

    std::size_t chars = utf8_length(name);
    if (chars > name_max_chars) {
      chars = name_max_chars;
    }
    std::string encoded = percent_encode(utf8_prefix(name, chars));
    while (chars > 0 && base.size() + encoded.size() > link_max_bytes) {
      --chars;
      encoded = percent_encode(utf8_prefix(name, chars));
    }
    return base + encoded;
  }

  enum class state_e {
    none,  ///< no record
    pending,  ///< created, nobody has started with it
    in_use,  ///< a session holds it (bound_uniqueid, bound_address)
    used,  ///< a device paired with it
    expired,  ///< lifetime passed before a pairing finished
  };

  inline constexpr std::string_view state_name(state_e state) {
    switch (state) {
      case state_e::pending:
        return "pending";
      case state_e::in_use:
        return "in_use";
      case state_e::used:
        return "used";
      case state_e::expired:
        return "expired";
      case state_e::none:
        break;
    }
    return "none";
  }

  /// Why a getservercert with `qr=<id>` is refused, in the order the checks run.
  enum class refusal_e {
    none,  ///< no record, or the id is not the record's
    network,  ///< the client address is not PC, LAN or Tailscale
    expired,  ///< lifetime passed
    used,  ///< already used, or held by a session from another address
  };

  /// The `qr_reason` attribute of a refusal.
  inline constexpr std::string_view reason(refusal_e why) {
    switch (why) {
      case refusal_e::network:
        return "network";
      case refusal_e::expired:
        return "expired";
      case refusal_e::used:
        return "used";
      case refusal_e::none:
        break;
    }
    return "none";
  }

  /// The `status_message` of a refusal: `QR_<reason>: ` and the sentence clients map to their own text.
  inline constexpr std::string_view message(refusal_e why) {
    switch (why) {
      case refusal_e::network:
        return "QR_network: QR pairing is accepted from the local network only";
      case refusal_e::expired:
        return "QR_expired: QR code expired; create a new one";
      case refusal_e::used:
        return "QR_used: QR code already used; create a new one";
      case refusal_e::none:
        break;
    }
    return "QR_none: No QR pairing is pending; create one in the web UI";
  }

  /// The one QR record a host keeps, in memory only.
  struct record_t {
    std::string token;  ///< never logged
    std::string id;
    std::uint32_t perm = 0;  ///< the permissions the paired device gets (PERM bits, masked with _all)
    clock::time_point created {};
    state_e state = state_e::none;
    std::string bound_uniqueid;  ///< the session holding the record (in_use) or that used it
    std::string bound_address;
    std::string paired_name;  ///< the device name once used (shown as text only)
    std::string created_by_address;
    clock::time_point paired_at {};
  };

  /// A fresh pending record.
  inline record_t make_record(std::string token, std::uint32_t perm, std::string created_by_address, clock::time_point now) {
    record_t r;
    r.id = token_id(token);
    r.token = std::move(token);
    r.perm = perm;
    r.created = now;
    r.state = state_e::pending;
    r.created_by_address = std::move(created_by_address);
    return r;
  }

  inline bool lifetime_over(const record_t &r, clock::time_point now) {
    return now - r.created > lifetime;
  }

  /**
   * @brief The state the web UI sees at `now`: a pending or in_use record past its lifetime is
   * `expired`; a used record is shown for used_display and is then `none`.
   */
  inline state_e visible_state(const record_t &r, clock::time_point now) {
    switch (r.state) {
      case state_e::pending:
      case state_e::in_use:
        return lifetime_over(r, now) ? state_e::expired : r.state;
      case state_e::used:
        return now - r.paired_at > used_display ? state_e::none : state_e::used;
      default:
        return r.state;
    }
  }

  /// Seconds left before a pending or in_use record expires; 0 otherwise.
  inline int remaining_seconds(const record_t &r, clock::time_point now) {
    auto state = visible_state(r, now);
    if (state != state_e::pending && state != state_e::in_use) {
      return 0;
    }
    auto left = std::chrono::duration_cast<std::chrono::seconds>(r.created + lifetime - now).count();
    return left < 0 ? 0 : static_cast<int>(left);
  }

  /**
   * @brief The checks of a getservercert with `qr=<id>`, in order; the first failure is the answer.
   * 1. no record or another id → none; 2. a WAN address → network; 3. lifetime over → expired;
   * 4. used, or in_use from another address → used. An in_use record from the same address passes
   * (the session is replaced, as any retry from the same device is).
   * @param r The record (state none when there is none).
   * @param id The `qr` argument.
   * @param local_address Whether net::from_address puts the client address in PC or LAN.
   * @param address The client address.
   */
  inline std::optional<refusal_e> check(const record_t &r, std::string_view id, bool local_address, std::string_view address, clock::time_point now) {
    if (r.state == state_e::none || r.id != id) {
      return refusal_e::none;
    }
    if (!local_address) {
      return refusal_e::network;
    }
    if (r.state == state_e::expired || lifetime_over(r, now)) {
      return refusal_e::expired;
    }
    if (r.state == state_e::used || (r.state == state_e::in_use && r.bound_address != address)) {
      return refusal_e::used;
    }
    return std::nullopt;
  }

  /// A session took the record (after check passed).
  inline void bind(record_t &r, std::string uniqueid, std::string address) {
    r.state = state_e::in_use;
    r.bound_uniqueid = std::move(uniqueid);
    r.bound_address = std::move(address);
  }

  /**
   * @brief The session holding the record ended without pairing (its own phase failure, a retry
   * or `/unpair` from its address): the record is pending again so a retry within the lifetime
   * works. Only the bound uniqueid and address may release it; returns whether it was released.
   */
  inline bool release(record_t &r, std::string_view uniqueid, std::string_view address) {
    if (r.state != state_e::in_use || r.bound_uniqueid != uniqueid || r.bound_address != address) {
      return false;
    }
    r.state = state_e::pending;
    r.bound_uniqueid.clear();
    r.bound_address.clear();
    return true;
  }

  /// The session holding the record paired: the record is used, with the device name for the web UI.
  inline bool complete(record_t &r, std::string_view uniqueid, std::string name, clock::time_point now) {
    if (r.state != state_e::in_use || r.bound_uniqueid != uniqueid) {
      return false;
    }
    r.state = state_e::used;
    r.paired_name = std::move(name);
    r.paired_at = now;
    return true;
  }

  /// The lifetime passed while pending or in_use (drop_stale_sessions); returns whether it changed.
  inline bool expire(record_t &r) {
    if (r.state != state_e::pending && r.state != state_e::in_use) {
      return false;
    }
    r.state = state_e::expired;
    return true;
  }

  /// The preset name the log shows for a permission set: the presets of the pairing page, else custom.
  inline std::string_view preset_name(std::uint32_t perm) {
    constexpr std::uint32_t all = 0x071F1F00;
    constexpr std::uint32_t stream = 0x07000000 | 0x00001F00;
    constexpr std::uint32_t view = 0x03000000;
    if (perm == all) {
      return "all";
    }
    if (perm == stream) {
      return "stream";
    }
    if (perm == view) {
      return "view";
    }
    return "custom";
  }
}  // namespace nvhttp::qr
