/**
 * @file src/platform/windows/clipboard_protocol.h
 * @brief Shell: the messages between shell.exe and its clipboard helper (tools/shellclip.exe).
 *
 * shell.exe runs as SYSTEM and does not see everything on the clipboard (a copy made in File
 * Explorer lists only OLE's "DataObject" format to it, so CF_HDROP looks absent). The helper runs
 * as the signed-in user, like rdpclip.exe does for Remote Desktop, and does the clipboard reads and
 * writes on request. The two talk over one pipe the helper inherits as its standard input and
 * output. This header is the whole wire format; it depends on nothing else so that both programs
 * and the standalone test (shell/tests/clipboard_image_test.cpp) share it.
 *
 * Every message is one frame, both ways (integers little-endian):
 *   u8 code | u32 payload length | payload
 * A request's code is an op_e, a reply's code a reply_e. Payloads:
 *   snapshot        request: none; reply: u32 change number | content type ("text", "image", "files", "none")
 *   get_text        request: none; reply: UTF-8 text
 *   get_image       request: none; reply: PNG
 *   get_files       request: none; reply: a path list (below)
 *   set_text        request: UTF-8 text; reply: none
 *   set_image       request: PNG; reply: none
 *   set_files       request: a path list; reply: none
 *   describe        request: none; reply: a line of text for the log
 * A path list is u32 count | (u32 length | UTF-8 absolute path)*, the top-level items of CF_HDROP.
 * A reply other than ok carries no payload, except bad_request, which may carry a reason.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platf::clipboard::protocol {
  enum class op_e : std::uint8_t {
    snapshot = 1,
    get_text = 2,
    get_image = 3,
    get_files = 4,
    set_text = 5,
    set_image = 6,
    set_files = 7,
    describe = 8,
  };

  /// The outcome of a request. The first six mirror clipboard::status_e (see clipboard_access.h for
  /// the mapping); bad_request means the helper could not parse the request.
  enum class reply_e : std::uint8_t {
    ok = 0,
    none = 1,
    busy = 2,
    not_convertible = 3,
    too_large = 4,
    failed = 5,
    bad_request = 6,
  };

  constexpr std::size_t header_size = 5;

  // Largest payloads, per op, in either direction. A frame over its op's limit is a protocol error:
  // the side that sees it drops the connection (shell.exe then restarts the helper).
  constexpr std::size_t max_text_payload = 64u * 1024 * 1024;
  constexpr std::size_t max_image_payload = 32u * 1024 * 1024;  ///< same as clipboard::max_image_bytes
  constexpr std::size_t max_paths_payload = 8u * 1024 * 1024;
  constexpr std::size_t max_describe_payload = 64u * 1024;
  constexpr std::size_t max_snapshot_payload = 4 + 16;
  constexpr std::size_t max_payload = max_text_payload;  ///< the largest of them

  /// Top-level items in a path list: more than this many copied items could not be sent anyway
  /// (same as clipboard::max_file_entries).
  constexpr std::size_t max_path_entries = 1000;

  /// Largest payload a request with this op may carry.
  constexpr std::size_t max_request_payload(op_e op) {
    switch (op) {
      case op_e::set_text:
        return max_text_payload;
      case op_e::set_image:
        return max_image_payload;
      case op_e::set_files:
        return max_paths_payload;
      default:
        return 0;
    }
  }

  /// Largest payload a reply to this op may carry.
  constexpr std::size_t max_reply_payload(op_e op) {
    switch (op) {
      case op_e::snapshot:
        return max_snapshot_payload;
      case op_e::get_text:
        return max_text_payload;
      case op_e::get_image:
        return max_image_payload;
      case op_e::get_files:
        return max_paths_payload;
      case op_e::describe:
        return max_describe_payload;
      default:
        return max_describe_payload;  // bad_request may carry a short reason
    }
  }

  constexpr bool is_op(std::uint8_t code) {
    return code >= static_cast<std::uint8_t>(op_e::snapshot) && code <= static_cast<std::uint8_t>(op_e::describe);
  }

  constexpr bool is_reply(std::uint8_t code) {
    return code <= static_cast<std::uint8_t>(reply_e::bad_request);
  }

  inline void put_u32(std::string &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
  }

  inline std::uint32_t get_u32(std::string_view in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[at + i])) << (8 * i);
    }
    return v;
  }

  /// The 5-byte header of a frame.
  inline std::string encode_header(std::uint8_t code, std::uint32_t length) {
    std::string out;
    out.push_back(static_cast<char>(code));
    put_u32(out, length);
    return out;
  }

  /**
   * @brief Reads a frame header.
   * @return False for fewer than header_size bytes.
   */
  inline bool decode_header(std::string_view header, std::uint8_t &code, std::uint32_t &length) {
    if (header.size() < header_size) {
      return false;
    }
    code = static_cast<std::uint8_t>(header[0]);
    length = get_u32(header, 1);
    return true;
  }

  /// Appends one frame to out. Payloads over UINT32_MAX cannot be framed; callers check the op's
  /// limit first, which is far below that.
  inline void encode_frame(std::string &out, std::uint8_t code, std::string_view payload) {
    out += encode_header(code, static_cast<std::uint32_t>(payload.size()));
    out.append(payload.data(), payload.size());
  }

  enum class decode_e {
    ok,
    incomplete,  ///< more bytes are needed
    too_large,  ///< the payload length is over max_payload: a protocol error
  };

  /**
   * @brief Takes one frame from the start of in, when it is complete.
   * @param max_payload The limit for this frame's payload (see max_request_payload, max_reply_payload).
   * @param consumed Set to the frame's size on ok.
   */
  inline decode_e decode_frame(std::string_view in, std::size_t max_payload, std::uint8_t &code, std::string_view &payload, std::size_t &consumed) {
    std::uint32_t length = 0;
    if (!decode_header(in, code, length)) {
      return decode_e::incomplete;
    }
    if (length > max_payload) {
      return decode_e::too_large;
    }
    if (in.size() - header_size < length) {
      return decode_e::incomplete;
    }
    payload = in.substr(header_size, length);
    consumed = header_size + length;
    return decode_e::ok;
  }

  // ---- Payloads -----------------------------------------------------------------------

  inline std::string encode_snapshot(std::uint32_t seq, std::string_view type) {
    std::string out;
    put_u32(out, seq);
    out.append(type.data(), type.size());
    return out;
  }

  /// False unless the payload is a change number followed by a short content type.
  inline bool decode_snapshot(std::string_view payload, std::uint32_t &seq, std::string &type) {
    if (payload.size() < 4 || payload.size() > max_snapshot_payload) {
      return false;
    }
    seq = get_u32(payload, 0);
    type.assign(payload.data() + 4, payload.size() - 4);
    return type == "text" || type == "image" || type == "files" || type == "none";
  }

  /// Encodes up to max_path_entries paths; empty when there are more, or a path is empty or
  /// holds a NUL (which would cut the CF_HDROP list short).
  inline std::string encode_paths(const std::vector<std::string> &paths) {
    if (paths.empty() || paths.size() > max_path_entries) {
      return {};
    }
    std::string out;
    put_u32(out, static_cast<std::uint32_t>(paths.size()));
    for (const auto &path : paths) {
      if (path.empty() || path.find('\0') != std::string::npos || path.size() > max_paths_payload) {
        return {};
      }
      put_u32(out, static_cast<std::uint32_t>(path.size()));
      out += path;
    }
    return out.size() <= max_paths_payload ? out : std::string {};
  }

  /**
   * @brief Decodes a path list: at least one and at most max_path_entries paths, none empty or
   * holding a NUL, and nothing after the last one.
   * @return False for anything else; paths is then empty.
   */
  inline bool decode_paths(std::string_view payload, std::vector<std::string> &paths) {
    paths.clear();
    if (payload.size() < 4 || payload.size() > max_paths_payload) {
      return false;
    }
    std::uint32_t count = get_u32(payload, 0);
    if (count == 0 || count > max_path_entries) {
      return false;
    }
    std::size_t pos = 4;
    paths.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (payload.size() - pos < 4) {
        paths.clear();
        return false;
      }
      std::uint32_t length = get_u32(payload, pos);
      pos += 4;
      if (length == 0 || payload.size() - pos < length) {
        paths.clear();
        return false;
      }
      std::string path(payload.data() + pos, length);
      if (path.find('\0') != std::string::npos) {
        paths.clear();
        return false;
      }
      paths.push_back(std::move(path));
      pos += length;
    }
    if (pos != payload.size()) {
      paths.clear();
      return false;
    }
    return true;
  }
}  // namespace platf::clipboard::protocol
