/**
 * @file src/platform/windows/clipboard_access.h
 * @brief Shell: the clipboard reads and writes themselves, done by whichever process calls them.
 *
 * Linked by shell.exe (where platf::clipboard::* forwards to these when the clipboard helper is
 * not running, see clipboard_helper.cpp) and by the helper, tools/shellclip.exe, which runs them
 * as the signed-in user. The conversions declared in clipboard.h (dib_to_png, png_size,
 * png_to_dibv5) live here too, as do open_with_retry, sequence and current_type.
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "clipboard.h"
#include "clipboard_protocol.h"

namespace platf::clipboard {
  static_assert(protocol::max_image_payload == max_image_bytes, "the helper carries whole clipboard images");
  static_assert(protocol::max_path_entries == max_file_entries, "the helper carries every top-level copied item");

  /// UTF-8 to UTF-16; empty for invalid UTF-8.
  std::wstring widen(const std::string &utf8);

  /// UTF-16 to UTF-8.
  std::string narrow(const std::wstring &wide);

  /// The reply code for a status (the two enums match; bad_request has no status).
  constexpr protocol::reply_e to_reply(status_e status) {
    return static_cast<protocol::reply_e>(static_cast<std::uint8_t>(status));
  }

  /// The status for a reply code; bad_request (the helper could not parse the request) is failed.
  constexpr status_e to_status(protocol::reply_e reply) {
    return reply == protocol::reply_e::bad_request ? status_e::failed : static_cast<status_e>(static_cast<std::uint8_t>(reply));
  }

  static_assert(to_reply(status_e::ok) == protocol::reply_e::ok && to_reply(status_e::failed) == protocol::reply_e::failed, "status_e and reply_e are numbered alike");

  namespace in_process {
    /// See the functions of the same name in clipboard.h: these are their implementations, run
    /// against the clipboard of the calling process.
    status_e snapshot(std::uint32_t &seq, std::string &type);
    std::string describe_for_log();
    status_e get_text(std::string &text);
    status_e set_text(const std::string &text);
    status_e get_image_png(std::string &png);
    status_e set_image_png(const std::string &png);
    status_e get_file_drop_list(std::vector<std::filesystem::path> &paths);
    status_e set_file_drop_list(const std::vector<std::filesystem::path> &paths);
  }  // namespace in_process
}  // namespace platf::clipboard
