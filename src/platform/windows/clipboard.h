/**
 * @file src/platform/windows/clipboard.h
 * @brief Rich clipboard access (sequence number, content type, PNG images) for clipboard sync.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace platf::clipboard {
  /// Largest image (encoded PNG) accepted or returned by clipboard sync.
  constexpr std::size_t max_image_bytes = 32 * 1024 * 1024;

  /// Largest decoded image size, in pixels, accepted when converting to a DIB.
  constexpr std::uint64_t max_image_pixels = 8192ull * 8192ull;

  /// Outcome of reading or writing the clipboard.
  enum class status_e {
    ok,
    none,  ///< read: nothing of the asked type is on the clipboard
    busy,  ///< another program kept the clipboard open through every retry of open_with_retry
    not_convertible,  ///< read: an image that cannot be read or converted to PNG; write: data that is not a PNG
    too_large,  ///< read: an image whose PNG is over max_image_bytes
    failed,  ///< anything else
  };

  // Machine-readable reasons: the first line of the body of a 422 reply, before the text for people.
  constexpr std::string_view reason_unsupported_name = "unsupported-name";  ///< a name the other side cannot create
  constexpr std::string_view reason_duplicate_name = "duplicate-name";  ///< two names that differ only in case, or a file also used as a folder
  constexpr std::string_view reason_nothing_to_copy = "nothing-to-copy";  ///< only links and junctions, or nothing at all
  constexpr std::string_view reason_image_not_convertible = "image-not-convertible";  ///< an image that cannot be converted

  /**
   * @brief The 422 reason for an error of list_paths, archive_paths or decode_archive caused by the
   * copied files themselves.
   * @return One of the reason_* codes; empty for any other error (limits, I/O, a malformed archive).
   */
  std::string_view content_error_reason(const std::string &error);

  /**
   * @brief OpenClipboard() with short retries, since another process may hold it briefly.
   * @return True if the clipboard is open; the caller must call CloseClipboard().
   */
  bool open_with_retry();

  /// Current clipboard sequence number (changes on every clipboard write).
  std::uint32_t sequence();

  /**
   * @brief Kind of content currently on the clipboard, checked in this order.
   * @return "text", "image", "files" or "none".
   */
  std::string current_type();

  /**
   * @brief Shell: the sequence number and the content kind read together, with the clipboard open,
   * so they belong to the same content. A program that writes the clipboard empties it first (the
   * sequence number changes then) and adds its formats afterwards; reading in between would
   * report the new number with "none", and the client would not look at that number again.
   * @return busy when another program holds the clipboard (it is being written): ask again later.
   */
  status_e snapshot(std::uint32_t &seq, std::string &type);

  /**
   * @brief Clipboard text (CF_UNICODETEXT) as UTF-8.
   * @return ok (the text may still be empty), none, busy, or failed when the listed text cannot be read.
   */
  status_e get_text(std::string &text);

  /// Places UTF-8 text on the clipboard as CF_UNICODETEXT, with line feeds turned into CR LF.
  status_e set_text(const std::string &text);

  /**
   * @brief Clipboard image as PNG, from the "PNG" format or converted from CF_DIB.
   * @return ok, none, busy, not_convertible (an image whose data cannot be read or converted) or
   * too_large (its PNG is over max_image_bytes); png is empty unless ok.
   */
  status_e get_image_png(std::string &png);

  /**
   * @brief Places a PNG on the clipboard as both "PNG" and CF_DIBV5.
   * @return ok, busy, not_convertible for data that does not start like a PNG, or failed (a PNG that
   * cannot be decoded, or another failure).
   */
  status_e set_image_png(const std::string &png);

  /// Converts a packed DIB (BITMAPINFOHEADER-family header, optional masks/palette, pixels) to PNG.
  std::string dib_to_png(const std::string &dib);

  /**
   * @brief Width and height from a PNG's IHDR chunk, without decoding it.
   * @return False if the data does not start like a PNG.
   */
  bool png_size(const std::string &png, std::uint32_t &width, std::uint32_t &height);

  /// Converts a PNG to a bottom-up 32-bit BI_BITFIELDS CF_DIBV5 block. Empty on failure.
  std::string png_to_dibv5(const std::string &png);

  // ---- Files ----------------------------------------------------------------------------
  //
  // Copied files travel as one "APCF" archive (all integers little-endian):
  //   "APCF" | u32 version (1) | u32 entry count
  //   entry: u8 kind (0 file, 1 directory) | u32 path length | UTF-8 path | u64 size | data
  // Paths are relative, use '/' separators, and each top-level item is a path without '/'.
  // The same format is implemented by the Hermit client.

  /// Total file data per transfer.
  constexpr std::uint64_t max_files_bytes = 256ull * 1024 * 1024;

  /// Files plus directories per transfer.
  constexpr std::size_t max_file_entries = 1000;

  /// Largest archive accepted from a client: the data limit plus per-entry overhead.
  constexpr std::size_t max_files_archive_bytes = max_files_bytes + max_file_entries * 1100 + 12;

  struct archive_entry {
    bool directory = false;
    std::string path;  ///< relative UTF-8 path with '/' separators
    std::string data;  ///< file contents (empty for directories)
  };

  /// True for a relative path that is safe to create under a staging directory on Windows
  /// (names of at most 255 UTF-16 code units, no reserved names or characters).
  bool is_safe_relative_path(const std::string &path);

  /**
   * @brief True when a reparse tag names a symbolic link or a mount point (junction), which
   * copying never follows. Other reparse points (OneDrive and other cloud placeholders,
   * deduplicated or ProjFS files) are ordinary files and folders to read.
   * @note IsReparseTagNameSurrogate() from ntifs.h, which the MinGW headers only ship for drivers.
   */
  constexpr bool is_link_tag(std::uint32_t tag) {
    return (tag & 0x20000000u) != 0;
  }

  /// True for a symbolic link or a mount point (junction); false for anything else, including a
  /// missing path.
  bool is_link(const std::filesystem::path &p);

  std::string encode_archive(const std::vector<archive_entry> &entries);

  /// Parses and fully validates an archive (paths, duplicates, limits). Sets error on failure.
  bool decode_archive(const std::string &archive, std::vector<archive_entry> &entries, std::string &error);

  /**
   * @brief Top-level paths currently on the clipboard as CF_HDROP.
   * @return ok, none (no CF_HDROP, or no paths in it) or busy.
   */
  status_e get_file_drop_list(std::vector<std::filesystem::path> &paths);

  /**
   * @brief Packs the given files and folders (recursively) into an archive.
   * Symbolic links and junctions are skipped (see is_link). Fails if the limits are exceeded.
   */
  bool archive_paths(const std::vector<std::filesystem::path> &roots, std::string &archive, std::string &error);

  // ---- Streamed files -------------------------------------------------------------------
  //
  // Newer clients fetch only a list of the copied files (GET type=filelist) and then the bytes of
  // each file when it is pasted (GET type=filedata), read from disk while they are sent.

  /// Total file data in one streamed file list. Nothing is held in memory, so it can exceed max_files_bytes.
  constexpr std::uint64_t max_stream_files_bytes = 4ull * 1024 * 1024 * 1024;

  struct file_list_entry {
    bool directory = false;
    std::string path;  ///< relative UTF-8 path with '/' separators
    std::filesystem::path source;  ///< absolute path on this PC
    std::uint64_t size = 0;  ///< file size (0 for directories)
    std::uint64_t write_time = 0;  ///< last write time as a FILETIME value (100 ns units since 1601, UTC)
  };

  /**
   * @brief Lists the given files and folders (recursively) without reading them, with the same
   * rules as archive_paths (names, duplicates, links skipped, max_file_entries) and at most
   * max_total bytes of file data. Folders come before their contents.
   */
  bool list_paths(const std::vector<std::filesystem::path> &roots, std::uint64_t max_total, std::vector<file_list_entry> &entries, std::string &error);

  /**
   * @brief Text manifest of a file list, one "key=value" line per field and then one line per entry:
   *   seq=<clipboard sequence> | snapshot=<id> | entries=<count> | bytes=<total file bytes>
   *   <f|d> TAB <size> TAB <last write, Unix ms> TAB <relative UTF-8 path with '/'>
   */
  std::string format_file_list(std::uint32_t seq, const std::string &snapshot, const std::vector<file_list_entry> &entries);

  /// Unix time in milliseconds for a FILETIME value; 0 for times before 1970.
  std::uint64_t filetime_to_unix_ms(std::uint64_t filetime);

  /**
   * @brief Opens a listed file for reading (a link in its place counts as changed; a cloud
   * placeholder is opened so that reading it fetches the content) and checks that it still has
   * the listed size and last write time.
   * @param changed Set to true when the file is gone or differs from the list.
   * @return The handle, or INVALID_HANDLE_VALUE with error set.
   */
  HANDLE open_listed_file(const file_list_entry &entry, bool &changed, std::string &error);

  /**
   * @brief Writes archive entries under a new folder in staging_root and returns the top-level items.
   * Older transfer folders in staging_root are removed, keeping the newest few.
   */
  bool extract_archive(const std::vector<archive_entry> &entries, const std::filesystem::path &staging_root, std::vector<std::filesystem::path> &top_level, std::string &error);

  /// Places the given paths on the clipboard as CF_HDROP with "Preferred DropEffect" = copy.
  status_e set_file_drop_list(const std::vector<std::filesystem::path> &paths);

  /**
   * @brief Folder for received files: <LocalAppData>\Temp\ShellClipboard of the given user,
   * or of the current account when user_token is nullptr.
   */
  std::filesystem::path staging_root(HANDLE user_token);
}  // namespace platf::clipboard
