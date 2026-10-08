/**
 * @file tools/shellclip.cpp
 * @brief Shell: the clipboard helper, run by shell.exe as the signed-in user.
 *
 * shell.exe runs as SYSTEM and does not see a copy made in File Explorer (only OLE's "DataObject"
 * format is listed to it), so it starts this program with the user's token and asks it to read and
 * write the clipboard, the way Remote Desktop runs rdpclip.exe. The only input is the pipe that
 * shell.exe hands over as standard input and output (src/platform/windows/clipboard_protocol.h
 * describes the messages); it answers requests until that pipe closes, which also happens when
 * shell.exe exits. It has no window, opens nothing by name, never runs or opens the paths it
 * carries, and does nothing on its own.
 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "src/platform/windows/clipboard_access.h"
#include "src/platform/windows/clipboard_protocol.h"

namespace cb = platf::clipboard;
namespace proto = platf::clipboard::protocol;

namespace {
  /// Reads exactly size bytes. False on EOF or any error: time to exit.
  bool read_all(HANDLE in, char *data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
      DWORD chunk = static_cast<DWORD>(size - done > (1u << 20) ? (1u << 20) : size - done);
      DWORD got = 0;
      if (!ReadFile(in, data + done, chunk, &got, nullptr) || got == 0) {
        return false;
      }
      done += got;
    }
    return true;
  }

  bool write_all(HANDLE out, const char *data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
      DWORD chunk = static_cast<DWORD>(size - done > (1u << 20) ? (1u << 20) : size - done);
      DWORD put = 0;
      if (!WriteFile(out, data + done, chunk, &put, nullptr) || put == 0) {
        return false;
      }
      done += put;
    }
    return true;
  }

  bool reply(HANDLE out, proto::reply_e code, std::string_view payload = {}) {
    std::string frame;
    proto::encode_frame(frame, static_cast<std::uint8_t>(code), payload);
    return write_all(out, frame.data(), frame.size());
  }

  /// Does one request and writes its reply. False when the pipe is gone.
  bool serve(HANDLE out, proto::op_e op, const std::string &payload) {
    std::string data;
    cb::status_e status = cb::status_e::failed;
    switch (op) {
      case proto::op_e::snapshot: {
        std::uint32_t seq = 0;
        std::string type;
        status = cb::in_process::snapshot(seq, type);
        if (status == cb::status_e::ok) {
          data = proto::encode_snapshot(seq, type);
        }
        break;
      }
      case proto::op_e::get_text:
        status = cb::in_process::get_text(data);
        if (status == cb::status_e::ok && data.size() > proto::max_text_payload) {
          data.clear();
          status = cb::status_e::too_large;
        }
        break;
      case proto::op_e::get_image:
        status = cb::in_process::get_image_png(data);  // at most max_image_bytes when ok
        break;
      case proto::op_e::get_files: {
        std::vector<std::filesystem::path> paths;
        status = cb::in_process::get_file_drop_list(paths);
        if (status == cb::status_e::ok) {
          std::vector<std::string> listed;
          listed.reserve(paths.size());
          for (const auto &p : paths) {
            listed.push_back(cb::narrow(p.wstring()));
          }
          data = proto::encode_paths(listed);
          if (data.empty()) {
            status = cb::status_e::too_large;  // more top-level items than can be sent anyway
          }
        }
        break;
      }
      case proto::op_e::set_text:
        status = cb::in_process::set_text(payload);
        break;
      case proto::op_e::set_image:
        status = cb::in_process::set_image_png(payload);
        break;
      case proto::op_e::set_files: {
        std::vector<std::string> listed;
        if (!proto::decode_paths(payload, listed)) {
          return reply(out, proto::reply_e::bad_request, "malformed path list");
        }
        std::vector<std::filesystem::path> paths;
        paths.reserve(listed.size());
        for (const auto &item : listed) {
          std::wstring wide = cb::widen(item);
          if (wide.empty()) {
            return reply(out, proto::reply_e::bad_request, "path is not UTF-8");
          }
          paths.emplace_back(std::move(wide));
        }
        status = cb::in_process::set_file_drop_list(paths);
        break;
      }
      case proto::op_e::describe:
        data = cb::in_process::describe_for_log();
        if (data.size() > proto::max_describe_payload) {
          data.resize(proto::max_describe_payload);
        }
        status = cb::status_e::ok;
        break;
    }
    if (status != cb::status_e::ok) {
      data.clear();
    }
    return reply(out, cb::to_reply(status), data);
  }
}  // namespace

int main() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  // Started by hand (a console, a file or nothing at all): there is nothing to do.
  if (in == nullptr || in == INVALID_HANDLE_VALUE || out == nullptr || out == INVALID_HANDLE_VALUE ||
      GetFileType(in) != FILE_TYPE_PIPE || GetFileType(out) != FILE_TYPE_PIPE) {
    return 2;
  }

  while (true) {
    char header[proto::header_size] {};
    if (!read_all(in, header, sizeof(header))) {
      return 0;  // shell.exe closed the pipe (or is gone)
    }
    std::uint8_t code = 0;
    std::uint32_t length = 0;
    proto::decode_header(std::string_view(header, sizeof(header)), code, length);
    if (!proto::is_op(code)) {
      return 3;  // not shell.exe's protocol: stop rather than guess
    }
    auto op = static_cast<proto::op_e>(code);
    if (length > proto::max_request_payload(op)) {
      // Over the op's limit; the payload is not read, so the stream cannot be resynchronized.
      reply(out, proto::reply_e::too_large);
      return 3;
    }
    std::string payload(length, '\0');
    if (length > 0 && !read_all(in, payload.data(), payload.size())) {
      return 0;
    }
    if (!serve(out, op, payload)) {
      return 0;
    }
  }
}
