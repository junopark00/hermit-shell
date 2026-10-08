/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Copied files for clipboard sync: the APCF archive, file lists and the staging folder.
 *
 * Shell: the clipboard reads and writes themselves are in clipboard_access.cpp (shared with the
 * clipboard helper) and reached through clipboard_helper.cpp; this file reads and writes files
 * only, which shell.exe does impersonating the signed-in user.
 */
#include "clipboard.h"

#include <Windows.h>
#include <UserEnv.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <set>
#include <system_error>

#include "clipboard_access.h"

namespace platf::clipboard {
  namespace {
    /// Case-insensitive key for duplicate detection (Windows paths ignore case): the Unicode
    /// uppercase mapping of the invariant locale, not only ASCII, close to how NTFS compares names.
    /// Hermit folds the same way (see its foldPath()).
    std::wstring fold(const std::string &utf8) {
      std::wstring w = widen(utf8);
      if (w.empty()) {
        return w;
      }
      int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr, 0);
      if (n <= 0) {
        return w;
      }
      std::wstring upper(static_cast<std::size_t>(n), L'\0');
      if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), static_cast<int>(w.size()), upper.data(), n, nullptr, nullptr, 0) != n) {
        return w;
      }
      return upper;
    }

    void put_u32(std::string &out, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
      }
    }

    void put_u64(std::string &out, std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
      }
    }

    class byte_reader {
    public:
      explicit byte_reader(const std::string &data):
          data_ {data} {}

      bool u8(std::uint8_t &v) {
        if (remaining() < 1) {
          return false;
        }
        v = static_cast<std::uint8_t>(data_[pos_++]);
        return true;
      }

      bool u32(std::uint32_t &v) {
        if (remaining() < 4) {
          return false;
        }
        v = 0;
        for (int i = 0; i < 4; ++i) {
          v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[pos_++])) << (8 * i);
        }
        return true;
      }

      bool u64(std::uint64_t &v) {
        if (remaining() < 8) {
          return false;
        }
        v = 0;
        for (int i = 0; i < 8; ++i) {
          v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[pos_++])) << (8 * i);
        }
        return true;
      }

      bool bytes(std::uint64_t n, std::string &out) {
        if (remaining() < n) {
          return false;
        }
        out.assign(data_, pos_, static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
        return true;
      }

      std::size_t remaining() const {
        return data_.size() - pos_;
      }

    private:
      const std::string &data_;
      std::size_t pos_ = 0;
    };

    std::string generic_utf8(const std::filesystem::path &p) {
      return narrow(p.generic_wstring());
    }

    /// The reparse tag of a file or folder; 0 when it is not a reparse point or cannot be read.
    DWORD reparse_tag(const std::filesystem::path &p) {
      WIN32_FIND_DATAW data {};
      HANDLE find = FindFirstFileExW(p.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
      if (find == INVALID_HANDLE_VALUE) {
        return 0;
      }
      FindClose(find);
      return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? data.dwReserved0 : 0;
    }

    bool read_file(const std::filesystem::path &p, std::uint64_t expected, std::string &out) {
      std::ifstream in(p, std::ios::binary);
      if (!in) {
        return false;
      }
      out.assign(static_cast<std::size_t>(expected), '\0');
      if (expected > 0 && !in.read(out.data(), static_cast<std::streamsize>(expected))) {
        return false;
      }
      // The file must not have grown while we read it; a changed file would be truncated.
      return in.peek() == std::char_traits<char>::eof();
    }
  }  // namespace

  bool is_link(const std::filesystem::path &p) {
    return is_link_tag(reparse_tag(p));
  }

  std::string_view content_error_reason(const std::string &error) {
    // list_paths and archive_paths (sending), then decode_archive (receiving)
    if (error.starts_with("unsupported file name") || error == "unsafe path") {
      return reason_unsupported_name;
    }
    if (error.starts_with("duplicate name") || error == "duplicate path" || error == "file used as directory") {
      return reason_duplicate_name;
    }
    if (error == "nothing to copy") {
      return reason_nothing_to_copy;
    }
    return {};
  }

  bool is_safe_relative_path(const std::string &path) {
    if (path.empty() || path.size() > 1024) {
      return false;
    }
    if (widen(path).empty()) {
      return false;  // not valid UTF-8
    }
    static const char *reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

    std::size_t start = 0;
    while (start <= path.size()) {
      std::size_t end = path.find('/', start);
      if (end == std::string::npos) {
        end = path.size();
      }
      std::string part = path.substr(start, end - start);
      // NTFS limits a name to 255 UTF-16 code units (Hermit measures QString::length() the same way)
      if (part.empty() || part == "." || part == ".." || widen(part).size() > 255) {
        return false;
      }
      for (unsigned char c : part) {
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
          return false;
        }
      }
      if (part.back() == '.' || part.back() == ' ') {
        return false;  // Windows silently strips these, which could alias another entry
      }
      std::string base = part.substr(0, part.find('.'));
      std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
      });
      for (const char *name : reserved) {
        if (base == name) {
          return false;
        }
      }
      if (end == path.size()) {
        break;
      }
      start = end + 1;
    }
    return true;
  }

  std::string encode_archive(const std::vector<archive_entry> &entries) {
    std::string out = "APCF";
    put_u32(out, 1);
    put_u32(out, static_cast<std::uint32_t>(entries.size()));
    for (const auto &entry : entries) {
      out.push_back(entry.directory ? 1 : 0);
      put_u32(out, static_cast<std::uint32_t>(entry.path.size()));
      out += entry.path;
      put_u64(out, entry.directory ? 0 : entry.data.size());
      if (!entry.directory) {
        out += entry.data;
      }
    }
    return out;
  }

  bool decode_archive(const std::string &archive, std::vector<archive_entry> &entries, std::string &error) {
    entries.clear();
    if (archive.size() > max_files_archive_bytes) {
      error = "archive too large";
      return false;
    }
    byte_reader in {archive};
    std::string magic;
    std::uint32_t version = 0, count = 0;
    if (!in.bytes(4, magic) || magic != "APCF" || !in.u32(version) || version != 1 || !in.u32(count)) {
      error = "not an APCF v1 archive";
      return false;
    }
    if (count == 0) {
      error = "empty archive";  // malformed (400), not over a limit
      return false;
    }
    if (count > max_file_entries) {
      error = "entry count out of range";
      return false;
    }

    std::set<std::wstring> seen;
    std::set<std::wstring> files;
    std::uint64_t total = 0;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      archive_entry entry;
      std::uint8_t kind = 0;
      std::uint32_t path_length = 0;
      std::uint64_t size = 0;
      if (!in.u8(kind) || kind > 1 || !in.u32(path_length) || path_length > 1024 || !in.bytes(path_length, entry.path) || !in.u64(size)) {
        error = "malformed entry";
        return false;
      }
      entry.directory = kind == 1;
      if (!is_safe_relative_path(entry.path)) {
        error = "unsafe path";
        return false;
      }
      if (entry.directory && size != 0) {
        error = "directory with data";
        return false;
      }
      total += size;
      if (total > max_files_bytes || !in.bytes(size, entry.data)) {
        error = total > max_files_bytes ? "files too large" : "truncated data";
        return false;
      }
      auto key = fold(entry.path);
      if (!seen.insert(key).second) {
        error = "duplicate path";
        return false;
      }
      if (!entry.directory) {
        files.insert(key);
      }
      entries.push_back(std::move(entry));
    }
    if (in.remaining() != 0) {
      error = "trailing data";
      return false;
    }
    // A file must not also be used as a parent directory of another entry.
    for (const auto &entry : entries) {
      auto key = fold(entry.path);
      for (std::size_t slash = key.find(L'/'); slash != std::wstring::npos; slash = key.find(L'/', slash + 1)) {
        if (files.count(key.substr(0, slash))) {
          error = "file used as directory";
          return false;
        }
      }
    }
    return true;
  }

  bool list_paths(const std::vector<std::filesystem::path> &roots, std::uint64_t max_total, std::vector<file_list_entry> &entries, std::string &error) {
    namespace fs = std::filesystem;
    entries.clear();
    std::uint64_t total = 0;
    std::set<std::wstring> seen;

    auto add = [&](const fs::path &item, const fs::path &base, bool directory) -> bool {
      file_list_entry entry;
      entry.directory = directory;
      entry.path = generic_utf8(item.lexically_relative(base));
      entry.source = item;
      if (!is_safe_relative_path(entry.path)) {
        error = "unsupported file name: " + entry.path;
        return false;
      }
      if (!seen.insert(fold(entry.path)).second) {
        error = "duplicate name: " + entry.path;
        return false;
      }
      if (entries.size() >= max_file_entries) {
        error = "too many files";
        return false;
      }
      WIN32_FILE_ATTRIBUTE_DATA data {};
      if (GetFileAttributesExW(item.c_str(), GetFileExInfoStandard, &data)) {
        entry.write_time = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (!directory) {
          entry.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
      } else if (!directory) {
        error = "cannot read size of " + entry.path;
        return false;
      }
      if (!directory) {
        total += entry.size;
        if (total > max_total) {
          error = "files too large";
          return false;
        }
      }
      entries.push_back(std::move(entry));
      return true;
    };

    for (const auto &root : roots) {
      std::error_code ec;
      if (is_link(root)) {
        continue;
      }
      auto status = fs::status(root, ec);
      if (ec || !root.has_filename()) {
        error = "cannot copy " + generic_utf8(root);
        return false;
      }
      fs::path base = root.parent_path();
      if (fs::is_regular_file(status)) {
        if (!add(root, base, false)) {
          return false;
        }
      } else if (fs::is_directory(status)) {
        if (!add(root, base, true)) {
          return false;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
          error = "cannot list " + generic_utf8(root);
          return false;
        }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
          if (ec) {
            error = "cannot list " + generic_utf8(root);
            return false;
          }
          const auto &path = it->path();
          if (is_link(path)) {
            it.disable_recursion_pending();  // never follow links or junctions
            continue;
          }
          auto item_status = it->status(ec);
          if (ec) {
            continue;
          }
          if (fs::is_directory(item_status)) {
            if (!add(path, base, true)) {
              return false;
            }
          } else if (fs::is_regular_file(item_status)) {
            if (!add(path, base, false)) {
              return false;
            }
          }
        }
      }
    }
    if (entries.empty()) {
      error = "nothing to copy";
      return false;
    }
    return true;
  }

  bool archive_paths(const std::vector<std::filesystem::path> &roots, std::string &archive, std::string &error) {
    std::vector<file_list_entry> listed;
    if (!list_paths(roots, max_files_bytes, listed, error)) {
      return false;
    }
    std::vector<archive_entry> entries;
    entries.reserve(listed.size());
    for (const auto &item : listed) {
      archive_entry entry;
      entry.directory = item.directory;
      entry.path = item.path;
      if (!item.directory && !read_file(item.source, item.size, entry.data)) {
        error = "cannot read " + item.path;
        return false;
      }
      entries.push_back(std::move(entry));
    }
    archive = encode_archive(entries);
    return true;
  }

  std::uint64_t filetime_to_unix_ms(std::uint64_t filetime) {
    constexpr std::uint64_t unix_epoch = 116444736000000000ull;  // 1970-01-01 as a FILETIME
    return filetime > unix_epoch ? (filetime - unix_epoch) / 10000 : 0;
  }

  std::string format_file_list(std::uint32_t seq, const std::string &snapshot, const std::vector<file_list_entry> &entries) {
    std::uint64_t total = 0;
    for (const auto &entry : entries) {
      total += entry.size;
    }
    std::string out = "seq=" + std::to_string(seq) + "\nsnapshot=" + snapshot + "\nentries=" + std::to_string(entries.size()) + "\nbytes=" + std::to_string(total) + "\n";
    for (const auto &entry : entries) {
      // Paths never contain tabs or line breaks (is_safe_relative_path rejects control characters).
      out += entry.directory ? 'd' : 'f';
      out += '\t' + std::to_string(entry.size) + '\t' + std::to_string(filetime_to_unix_ms(entry.write_time)) + '\t' + entry.path + '\n';
    }
    return out;
  }

  HANDLE open_listed_file(const file_list_entry &entry, bool &changed, std::string &error) {
    changed = false;
    // Sharing like std::ifstream, so files that another program has open can still be copied.
    auto open = [&](DWORD flags) {
      HANDLE file = CreateFileW(entry.source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | flags, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        changed = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
        error = changed ? "file is gone" : "cannot open file (error " + std::to_string(code) + ")";
      }
      return file;
    };
    // Opened without following a reparse point first, so a link put in place of the listed file
    // is seen as one instead of followed.
    HANDLE file = open(FILE_FLAG_OPEN_REPARSE_POINT);
    if (file == INVALID_HANDLE_VALUE) {
      return INVALID_HANDLE_VALUE;
    }
    FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag))) {
      error = "cannot read file attributes (error " + std::to_string(GetLastError()) + ")";
      CloseHandle(file);
      return INVALID_HANDLE_VALUE;
    }
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      if (is_link_tag(tag.ReparseTag)) {
        changed = true;
        error = "file changed since it was copied";
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
      }
      // A cloud placeholder or another reparse point that is not a link: reopened the normal way,
      // so reading it goes through its filter (OneDrive then fetches the content).
      CloseHandle(file);
      file = open(0);
      if (file == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
      }
    }
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(file, &info)) {
      error = "cannot read file information (error " + std::to_string(GetLastError()) + ")";
      CloseHandle(file);
      return INVALID_HANDLE_VALUE;
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const std::uint64_t write_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || size != entry.size || write_time != entry.write_time) {
      changed = true;
      error = "file changed since it was copied";
      CloseHandle(file);
      return INVALID_HANDLE_VALUE;
    }
    return file;
  }

  bool extract_archive(const std::vector<archive_entry> &entries, const std::filesystem::path &staging_root, std::vector<std::filesystem::path> &top_level, std::string &error) {
    namespace fs = std::filesystem;
    top_level.clear();
    std::error_code ec;
    fs::create_directories(staging_root, ec);
    if (ec) {
      error = "cannot create staging folder";
      return false;
    }

    // Keep only the newest few earlier transfers; the clipboard no longer points at older ones.
    std::vector<fs::path> previous;
    for (fs::directory_iterator it(staging_root, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec) && it->path().filename().wstring().rfind(L"xfer-", 0) == 0) {
        previous.push_back(it->path());
      }
    }
    std::sort(previous.begin(), previous.end());
    while (previous.size() > 4) {
      fs::remove_all(previous.front(), ec);
      previous.erase(previous.begin());
    }

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    fs::path folder = staging_root / (L"xfer-" + std::to_wstring(stamp));
    if (!fs::create_directory(folder, ec) || ec) {
      error = "cannot create transfer folder";
      return false;
    }

    auto fail = [&](const std::string &message) {
      error = message;
      fs::remove_all(folder, ec);
      top_level.clear();
      return false;
    };

    std::set<std::wstring> tops;
    for (const auto &entry : entries) {
      fs::path relative {widen(entry.path)};
      fs::path target = folder / relative;
      if (entry.directory) {
        fs::create_directories(target, ec);
        if (ec) {
          return fail("cannot create folder " + entry.path);
        }
      } else {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
          return fail("cannot create folder for " + entry.path);
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size())) || !out.flush()) {
          return fail("cannot write " + entry.path);
        }
      }
      std::wstring top = relative.begin()->wstring();
      if (tops.insert(fold(narrow(top))).second) {
        top_level.push_back(folder / top);
      }
    }
    return true;
  }

  std::filesystem::path staging_root(HANDLE user_token) {
    if (user_token != nullptr) {
      DWORD size = 0;
      GetUserProfileDirectoryW(user_token, nullptr, &size);
      std::wstring profile(size, L'\0');
      if (size > 0 && GetUserProfileDirectoryW(user_token, profile.data(), &size)) {
        profile.resize(wcslen(profile.c_str()));
        return std::filesystem::path(profile) / L"AppData" / L"Local" / L"Temp" / L"ShellClipboard";
      }
    }
    wchar_t temp[MAX_PATH + 1] {};
    DWORD length = GetTempPathW(MAX_PATH + 1, temp);
    return std::filesystem::path(std::wstring(temp, length)) / L"ShellClipboard";
  }
}  // namespace platf::clipboard
