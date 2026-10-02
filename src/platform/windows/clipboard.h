/**
 * @file src/platform/windows/clipboard.h
 * @brief Rich clipboard access (sequence number, content type, PNG images) for clipboard sync.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Windows.h>

namespace platf::clipboard {
  /// Largest image (encoded PNG) accepted or returned by clipboard sync.
  constexpr std::size_t max_image_bytes = 32 * 1024 * 1024;

  /// Largest decoded image size, in pixels, accepted when converting to a DIB.
  constexpr std::uint64_t max_image_pixels = 8192ull * 8192ull;

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

  /// Clipboard image as PNG, from the "PNG" format or converted from CF_DIB. Empty if none.
  std::string get_image_png();

  /// Places a PNG on the clipboard as both "PNG" and CF_DIBV5. Returns false on invalid input or failure.
  bool set_image_png(const std::string &png);

  /// Converts a packed DIB (BITMAPINFOHEADER-family header, optional masks/palette, pixels) to PNG.
  std::string dib_to_png(const std::string &dib);

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

  /// True for a relative path that is safe to create under a staging directory on Windows.
  bool is_safe_relative_path(const std::string &path);

  std::string encode_archive(const std::vector<archive_entry> &entries);

  /// Parses and fully validates an archive (paths, duplicates, limits). Sets error on failure.
  bool decode_archive(const std::string &archive, std::vector<archive_entry> &entries, std::string &error);

  /// Top-level paths currently on the clipboard as CF_HDROP. Empty if none.
  std::vector<std::filesystem::path> get_file_drop_list();

  /**
   * @brief Packs the given files and folders (recursively) into an archive.
   * Symbolic links and other reparse points are skipped. Fails if the limits are exceeded.
   */
  bool archive_paths(const std::vector<std::filesystem::path> &roots, std::string &archive, std::string &error);

  /**
   * @brief Writes archive entries under a new folder in staging_root and returns the top-level items.
   * Older transfer folders in staging_root are removed, keeping the newest few.
   */
  bool extract_archive(const std::vector<archive_entry> &entries, const std::filesystem::path &staging_root, std::vector<std::filesystem::path> &top_level, std::string &error);

  /// Places the given paths on the clipboard as CF_HDROP with "Preferred DropEffect" = copy.
  bool set_file_drop_list(const std::vector<std::filesystem::path> &paths);

  /**
   * @brief Folder for received files: <LocalAppData>\Temp\ShellClipboard of the given user,
   * or of the current account when user_token is nullptr.
   */
  std::filesystem::path staging_root(HANDLE user_token);
}  // namespace platf::clipboard
