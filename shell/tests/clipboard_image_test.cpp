// Standalone checks for src/platform/windows/clipboard.cpp: DIB <-> PNG conversion, the APCF file
// archive (encode, decode, validation, extraction, packing a folder tree) and the 422 reasons.
// Uses synthetic data and a temporary folder only; never touches the real clipboard (set_image_png
// is only given data it refuses before opening the clipboard).
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/clipboard_image_test.cpp src/platform/windows/clipboard.cpp -lwindowscodecs -lole32 -luuid -luserenv -lshell32 -o build/clipboard_image_test.exe
//   build/clipboard_image_test.exe
#include "platform/windows/clipboard.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace cb = platf::clipboard;

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

struct Rgba {
  uint8_t r, g, b, a;
};

// Packed bottom-up DIB with a BITMAPINFOHEADER. bpp is 24 or 32 (BI_RGB).
static std::string make_dib(int width, int height, int bpp, const std::vector<Rgba> &pixels) {
  const int stride = ((width * bpp + 31) / 32) * 4;
  BITMAPINFOHEADER h {};
  h.biSize = sizeof(h);
  h.biWidth = width;
  h.biHeight = height;
  h.biPlanes = 1;
  h.biBitCount = static_cast<WORD>(bpp);
  h.biCompression = BI_RGB;
  h.biSizeImage = static_cast<DWORD>(stride * height);
  std::string dib(reinterpret_cast<const char *>(&h), sizeof(h));
  for (int row = height - 1; row >= 0; --row) {
    std::string line(stride, '\0');
    for (int x = 0; x < width; ++x) {
      const Rgba &p = pixels[row * width + x];
      int o = x * (bpp / 8);
      line[o + 0] = static_cast<char>(p.b);
      line[o + 1] = static_cast<char>(p.g);
      line[o + 2] = static_cast<char>(p.r);
      if (bpp == 32) {
        line[o + 3] = static_cast<char>(p.a);
      }
    }
    dib += line;
  }
  return dib;
}

// Reads pixel (x, y) (top-left origin) from the bottom-up 32-bit DIBV5 produced by png_to_dibv5.
static Rgba dibv5_pixel(const std::string &dib, int x, int y) {
  BITMAPV5HEADER h;
  std::memcpy(&h, dib.data(), sizeof(h));
  const int width = h.bV5Width;
  const int height = h.bV5Height;
  const auto *bits = reinterpret_cast<const uint8_t *>(dib.data() + h.bV5Size);
  const uint8_t *p = bits + static_cast<size_t>(height - 1 - y) * width * 4 + static_cast<size_t>(x) * 4;
  return Rgba {p[2], p[1], p[0], p[3]};
}

static bool same_rgb(Rgba a, Rgba b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

int main() {
  const std::vector<Rgba> pixels = {
    {255, 0, 0, 255},
    {0, 255, 0, 255},
    {0, 0, 255, 255},
    {10, 20, 30, 255},
    {200, 150, 100, 255},
    {255, 255, 255, 255},
  };  // 3 x 2, top row first

  std::printf("DIB -> PNG -> DIBV5 round trip\n");
  for (int bpp : {24, 32}) {
    std::string dib = make_dib(3, 2, bpp, pixels);
    std::string png = cb::dib_to_png(dib);
    char label[96];
    std::snprintf(label, sizeof(label), "%d bpp DIB encodes to PNG", bpp);
    check(label, png.size() > 8 && std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0);

    std::string v5 = cb::png_to_dibv5(png);
    std::snprintf(label, sizeof(label), "%d bpp PNG decodes to DIBV5", bpp);
    check(label, v5.size() == sizeof(BITMAPV5HEADER) + 3 * 2 * 4);
    if (v5.size() != sizeof(BITMAPV5HEADER) + 3 * 2 * 4) {
      continue;
    }

    BITMAPV5HEADER h;
    std::memcpy(&h, v5.data(), sizeof(h));
    std::snprintf(label, sizeof(label), "%d bpp DIBV5 header is 3x2 bottom-up 32-bit bitfields", bpp);
    check(label, h.bV5Width == 3 && h.bV5Height == 2 && h.bV5BitCount == 32 && h.bV5Compression == BI_BITFIELDS &&
                   h.bV5RedMask == 0x00FF0000 && h.bV5AlphaMask == 0xFF000000 && h.bV5SizeImage == 24);

    bool all = true;
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 3; ++x) {
        Rgba got = dibv5_pixel(v5, x, y);
        all = all && same_rgb(got, pixels[y * 3 + x]) && got.a == 255;
      }
    }
    std::snprintf(label, sizeof(label), "%d bpp pixels and orientation survive the round trip", bpp);
    check(label, all);
  }

  std::printf("Row padding\n");
  {
    // Width 5 at 24 bpp needs 15 bytes per row, padded to 16.
    std::vector<Rgba> wide;
    for (int i = 0; i < 10; ++i) {
      wide.push_back(Rgba {static_cast<uint8_t>(i * 20), static_cast<uint8_t>(255 - i * 20), static_cast<uint8_t>(i * 7), 255});
    }
    std::string v5 = cb::png_to_dibv5(cb::dib_to_png(make_dib(5, 2, 24, wide)));
    bool all = v5.size() == sizeof(BITMAPV5HEADER) + 5 * 2 * 4;
    for (int y = 0; all && y < 2; ++y) {
      for (int x = 0; x < 5; ++x) {
        all = all && same_rgb(dibv5_pixel(v5, x, y), wide[y * 5 + x]);
      }
    }
    check("padded 24 bpp rows convert correctly", all);
  }

  std::printf("Rejects bad input\n");
  check("empty DIB", cb::dib_to_png("").empty());
  check("truncated DIB header", cb::dib_to_png(std::string(10, '\0')).empty());
  {
    std::string dib = make_dib(3, 2, 24, pixels);
    BITMAPINFOHEADER h;
    std::memcpy(&h, dib.data(), sizeof(h));
    h.biSize = 0x7FFFFFFF;
    std::memcpy(dib.data(), &h, sizeof(h));
    check("header size larger than data", cb::dib_to_png(dib).empty());
  }
  {
    std::string dib = make_dib(3, 2, 24, pixels);
    dib.resize(sizeof(BITMAPINFOHEADER));
    check("DIB without pixel data", cb::dib_to_png(dib).empty());
  }
  check("empty PNG", cb::png_to_dibv5("").empty());
  check("non-PNG bytes", cb::png_to_dibv5("definitely not a png").empty());
  {
    std::string png = cb::dib_to_png(make_dib(3, 2, 24, pixels));
    check("truncated PNG", cb::png_to_dibv5(png.substr(0, png.size() / 2)).empty());
  }
  check("PNG over the size limit", cb::png_to_dibv5(std::string(cb::max_image_bytes + 1, 'x')).empty());
  // Refused before the clipboard is opened, so the real clipboard is left alone
  check("writing non-PNG data is not convertible", cb::set_image_png("definitely not a png") == cb::status_e::not_convertible);
  check("writing nothing is not convertible", cb::set_image_png("") == cb::status_e::not_convertible);
  {
    std::uint32_t width = 0, height = 0;
    std::string png = cb::dib_to_png(make_dib(3, 2, 24, pixels));
    check("PNG size read from IHDR", cb::png_size(png, width, height) && width == 3 && height == 2);
    check("no PNG size for other data", !cb::png_size("definitely not a png file", width, height));
    // Claim 8193 x 8192 pixels in IHDR: png_size reports it, the conversion refuses it.
    std::string big = png;
    const unsigned char dims[8] = {0, 0, 0x20, 0x01, 0, 0, 0x20, 0x00};
    std::memcpy(big.data() + 16, dims, sizeof(dims));
    check("PNG size of an oversized header", cb::png_size(big, width, height) && width == 8193 && height == 8192);
    check("PNG over the pixel limit", static_cast<std::uint64_t>(width) * height > cb::max_image_pixels && cb::png_to_dibv5(big).empty());
  }

  std::printf("Path validation\n");
  for (const char *ok : {"a.txt", "folder/b.bin", "한글/파일.txt", "a/b/c/d", "name with spaces.txt"}) {
    std::string label = std::string("accepts ") + ok;
    check(label.c_str(), cb::is_safe_relative_path(ok));
  }
  for (const char *bad : {"", "/abs", "a/", "a//b", "..", "a/../b", ".", "C:/x", "a\\b", "con", "CON.txt", "dir/lpt1.log",
                          "trail.", "trail ", "q?.txt", "pipe|x"}) {
    std::string label = std::string("rejects \"") + bad + "\"";
    check(label.c_str(), !cb::is_safe_relative_path(bad));
  }
  {
    // Names are limited to 255 UTF-16 code units (the NTFS limit), not 255 UTF-8 bytes.
    auto repeat = [](const std::string &unit, int n) {
      std::string out;
      for (int i = 0; i < n; ++i) {
        out += unit;
      }
      return out;
    };
    check("accepts 255 ASCII characters", cb::is_safe_relative_path(repeat("a", 255)));
    check("rejects 256 ASCII characters", !cb::is_safe_relative_path(repeat("a", 256)));
    check("accepts 255 Hangul syllables (765 bytes)", cb::is_safe_relative_path(repeat("한", 255)));
    check("rejects 256 Hangul syllables", !cb::is_safe_relative_path(repeat("한", 256)));
    check("accepts 127 emoji plus one letter (255 code units)", cb::is_safe_relative_path(repeat("😀", 127) + "a"));
    check("rejects 128 emoji (256 code units)", !cb::is_safe_relative_path(repeat("😀", 128)));
  }

  std::printf("Archive encode/decode\n");
  std::vector<cb::archive_entry> sample = {
    {true, "docs", ""},
    {false, "docs/readme.txt", "hello\r\nworld"},
    {true, "docs/empty", ""},
    {false, "photo.bin", std::string("\x00\x01\x02\xff", 4)},
    {false, "zero.txt", ""},
  };
  std::string archive = cb::encode_archive(sample);
  {
    std::vector<cb::archive_entry> decoded;
    std::string error;
    bool ok = cb::decode_archive(archive, decoded, error) && decoded.size() == sample.size();
    for (std::size_t i = 0; ok && i < sample.size(); ++i) {
      ok = decoded[i].directory == sample[i].directory && decoded[i].path == sample[i].path && decoded[i].data == sample[i].data;
    }
    check("round trip keeps kinds, paths and bytes", ok);
  }
  // reason: the 422 reason the error maps to; empty for errors answered otherwise (400, 413)
  auto rejects = [](const char *label, const std::vector<cb::archive_entry> &entries, std::string_view reason) {
    std::vector<cb::archive_entry> decoded;
    std::string error;
    check(label, !cb::decode_archive(cb::encode_archive(entries), decoded, error) && !error.empty() && cb::content_error_reason(error) == reason);
  };
  rejects("duplicate path (case-insensitive)", {{false, "A.txt", "1"}, {false, "a.TXT", "2"}}, cb::reason_duplicate_name);
  rejects("duplicate path (Latin-1 case)", {{false, "Ärger.txt", "1"}, {false, "ärger.txt", "2"}}, cb::reason_duplicate_name);
  rejects("duplicate path (Greek case)", {{false, "ΣΟΦΙΑ.txt", "1"}, {false, "σοφια.txt", "2"}}, cb::reason_duplicate_name);
  rejects("duplicate folder (Cyrillic case)", {{true, "Папка", ""}, {true, "папка", ""}}, cb::reason_duplicate_name);
  {
    std::vector<cb::archive_entry> decoded;
    std::string error;
    check("different non-ASCII names are not duplicates",
          cb::decode_archive(cb::encode_archive({{false, "한글.txt", "1"}, {false, "Ärger.txt", "2"}, {false, "ärgerlich.txt", "3"}}), decoded, error));
  }
  rejects("file used as a directory", {{false, "a", "1"}, {false, "a/b", "2"}}, cb::reason_duplicate_name);
  rejects("unsafe path inside archive", {{false, "../evil", "x"}}, cb::reason_unsupported_name);
  rejects("empty archive", {}, {});
  {
    std::vector<cb::archive_entry> decoded;
    std::string error;
    check("empty archive is malformed, not over a limit", !cb::decode_archive(cb::encode_archive({}), decoded, error) && error == "empty archive");
    std::vector<cb::archive_entry> many(cb::max_file_entries + 1);
    for (std::size_t i = 0; i < many.size(); ++i) {
      many[i].path = "f" + std::to_string(i);
    }
    check("too many entries is over the limit", !cb::decode_archive(cb::encode_archive(many), decoded, error) && error == "entry count out of range");
  }
  std::printf("422 reasons\n");
  check("unsupported file name", cb::content_error_reason("unsupported file name: a/con") == "unsupported-name");
  check("duplicate name", cb::content_error_reason("duplicate name: a/B.txt") == "duplicate-name");
  check("nothing to copy", cb::content_error_reason("nothing to copy") == "nothing-to-copy");
  check("image reason", cb::reason_image_not_convertible == "image-not-convertible");
  for (const char *other : {"files too large", "too many files", "archive too large", "entry count out of range", "empty archive",
                            "malformed entry", "cannot read a.txt", "no user session"}) {
    std::string label = std::string("no 422 reason for \"") + other + "\"";
    check(label.c_str(), cb::content_error_reason(other).empty());
  }
  {
    std::vector<cb::archive_entry> decoded;
    std::string error;
    check("bad magic", !cb::decode_archive("NOPE" + archive.substr(4), decoded, error));
    check("trailing garbage", !cb::decode_archive(archive + "x", decoded, error));
    check("truncated archive", !cb::decode_archive(archive.substr(0, archive.size() - 3), decoded, error));
    std::string dir_with_data = cb::encode_archive({{false, "d", "abc"}});
    dir_with_data[12] = 1;  // flip the entry kind to "directory" while it still carries data
    check("directory with data", !cb::decode_archive(dir_with_data, decoded, error));
  }
  {
    // Claim a size far over the limit without allocating it.
    std::string big = cb::encode_archive({{false, "big.bin", ""}});
    std::uint64_t huge = cb::max_files_bytes + 1;
    std::memcpy(big.data() + big.size() - 8, &huge, 8);
    std::vector<cb::archive_entry> decoded;
    std::string error;
    check("declared size over the limit", !cb::decode_archive(big, decoded, error));
  }

  std::printf("Extract and pack on disk\n");
  namespace fs = std::filesystem;
  fs::path scratch = fs::temp_directory_path() / ("apcf-test-" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(scratch);
  {
    std::vector<fs::path> tops;
    std::string error;
    bool ok = cb::extract_archive(sample, scratch / "staging", tops, error);
    check("extract succeeds", ok);
    check("top-level items are docs and the two files", ok && tops.size() == 3 && tops[0].filename() == L"docs" && tops[1].filename() == L"photo.bin");
    if (ok) {
      std::ifstream in(tops[0] / L"readme.txt", std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      check("extracted file bytes match", text == "hello\r\nworld");
      check("empty folder is created", fs::is_directory(tops[0] / L"empty"));
      check("zero-byte file is created", fs::exists(tops[2]) && fs::file_size(tops[2]) == 0);

      // Pack the extracted tree back and compare with the original entries.
      std::string repacked;
      bool packed = cb::archive_paths(tops, repacked, error);
      std::vector<cb::archive_entry> decoded;
      bool same = packed && cb::decode_archive(repacked, decoded, error) && decoded.size() == sample.size();
      for (const auto &want : sample) {
        bool found = false;
        for (const auto &got : decoded) {
          found = found || (got.path == want.path && got.directory == want.directory && got.data == want.data);
        }
        same = same && found;
      }
      check("packing a folder tree reproduces the entries", same);
    }
    for (int i = 0; i < 6; ++i) {
      Sleep(2);
      cb::extract_archive({{false, "n.txt", "x"}}, scratch / "staging", tops, error);
    }
    int transfers = 0;
    for (auto &e : fs::directory_iterator(scratch / "staging")) {
      transfers += e.is_directory() ? 1 : 0;
    }
    check("older transfer folders are pruned to 5", transfers == 5);
  }
  {
    std::vector<fs::path> tops;
    std::string error;
    std::string packed;
    check("packing nothing fails", !cb::archive_paths({}, packed, error));
  }

  std::printf("Links and other reparse points\n");
  {
    check("symbolic link tag is a link", cb::is_link_tag(IO_REPARSE_TAG_SYMLINK));
    check("mount point tag is a link", cb::is_link_tag(IO_REPARSE_TAG_MOUNT_POINT));
    check("cloud placeholder tag is not a link", !cb::is_link_tag(0x9000001A));  // IO_REPARSE_TAG_CLOUD
    check("dedup tag is not a link", !cb::is_link_tag(0x80000013));  // IO_REPARSE_TAG_DEDUP
    check("no tag is not a link", !cb::is_link_tag(0));

    fs::path tree = scratch / "links";
    fs::create_directories(tree / "real");
    std::ofstream(tree / "real" / "a.txt", std::ios::binary) << "a";
    check("plain file is not a link", !cb::is_link(tree / "real" / "a.txt"));
    check("plain folder is not a link", !cb::is_link(tree / "real"));
    check("missing path is not a link", !cb::is_link(tree / "missing"));

    // A directory junction needs no special rights (unlike a symbolic link); mklink /J makes one.
    fs::path junction = tree / "junction";
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + junction.wstring() + L"\" \"" + (tree / "real").wstring() + L"\" >nul";
    if (_wsystem(cmd.c_str()) == 0 && fs::exists(junction)) {
      check("junction is a link", cb::is_link(junction));
      std::vector<cb::file_list_entry> listed;
      std::string error;
      bool ok = cb::list_paths({tree}, 1 << 20, listed, error);
      bool junction_listed = false;
      for (const auto &entry : listed) {
        junction_listed = junction_listed || entry.path.find("junction") != std::string::npos;
      }
      check("listing skips the junction", ok && !junction_listed && listed.size() == 3);  // links, links/real, links/real/a.txt
      check("listing only a junction copies nothing", !cb::list_paths({junction}, 1 << 20, listed, error) && error == "nothing to copy" &&
                                                        cb::content_error_reason(error) == cb::reason_nothing_to_copy);
      RemoveDirectoryW(junction.c_str());  // removes the junction, not its target
    } else {
      std::printf("  skip junction checks (mklink /J failed)\n");
    }
  }
  fs::remove_all(scratch);

  std::printf("\nPassed %d, failed %d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
