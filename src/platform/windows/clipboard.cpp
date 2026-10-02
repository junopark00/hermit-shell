/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Rich clipboard access (sequence number, content type, PNG images) for clipboard sync.
 */
#include "clipboard.h"

#include <Windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <UserEnv.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <set>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace platf::clipboard {
  namespace {
    /// Initializes COM for the calling thread for the lifetime of the object when needed.
    class com_scope {
    public:
      com_scope():
          hr_ {CoInitializeEx(nullptr, COINIT_MULTITHREADED)} {}

      ~com_scope() {
        // S_OK and S_FALSE both need a matching uninit; RPC_E_CHANGED_MODE means COM was
        // already initialized as STA on this thread, which WIC also works with.
        if (SUCCEEDED(hr_)) {
          CoUninitialize();
        }
      }

      com_scope(const com_scope &) = delete;
      com_scope &operator=(const com_scope &) = delete;

    private:
      HRESULT hr_;
    };

    UINT png_format() {
      static const UINT format = RegisterClipboardFormatW(L"PNG");
      return format;
    }

    ComPtr<IWICImagingFactory> wic_factory() {
      ComPtr<IWICImagingFactory> factory;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return nullptr;
      }
      return factory;
    }

    std::string stream_bytes(IStream *stream) {
      HGLOBAL global = nullptr;
      if (FAILED(GetHGlobalFromStream(stream, &global))) {
        return {};
      }
      STATSTG stat {};
      if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
        return {};
      }
      auto size = static_cast<std::size_t>(stat.cbSize.QuadPart);
      auto *data = static_cast<const char *>(GlobalLock(global));
      if (data == nullptr) {
        return {};
      }
      std::string out(data, size);
      GlobalUnlock(global);
      return out;
    }

    std::string encode_png(IWICImagingFactory *factory, IWICBitmapSource *source) {
      ComPtr<IStream> stream;
      if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        return {};
      }
      ComPtr<IWICBitmapEncoder> encoder;
      if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
          FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
        return {};
      }
      ComPtr<IWICBitmapFrameEncode> frame;
      if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
        return {};
      }
      UINT width = 0, height = 0;
      if (FAILED(source->GetSize(&width, &height)) || FAILED(frame->SetSize(width, height))) {
        return {};
      }
      WICPixelFormatGUID format;
      if (FAILED(source->GetPixelFormat(&format)) || FAILED(frame->SetPixelFormat(&format))) {
        return {};
      }
      // WriteSource converts to the pixel format the encoder negotiated above if they differ.
      if (FAILED(frame->WriteSource(source, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        return {};
      }
      return stream_bytes(stream.Get());
    }

    std::string read_global(HANDLE handle) {
      if (handle == nullptr) {
        return {};
      }
      auto size = GlobalSize(handle);
      auto *data = static_cast<const char *>(GlobalLock(handle));
      if (data == nullptr) {
        return {};
      }
      std::string out(data, size);
      GlobalUnlock(handle);
      return out;
    }

    HGLOBAL make_global(const std::string &bytes) {
      HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
      if (global == nullptr) {
        return nullptr;
      }
      void *data = GlobalLock(global);
      if (data == nullptr) {
        GlobalFree(global);
        return nullptr;
      }
      std::memcpy(data, bytes.data(), bytes.size());
      GlobalUnlock(global);
      return global;
    }

    // Local UTF-8 helpers so this file does not depend on the rest of Shell.
    std::wstring widen(const std::string &utf8) {
      if (utf8.empty()) {
        return {};
      }
      int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
      if (n <= 0) {
        return {};
      }
      std::wstring out(static_cast<std::size_t>(n), L'\0');
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
      return out;
    }

    std::string narrow(const std::wstring &wide) {
      if (wide.empty()) {
        return {};
      }
      int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
      if (n <= 0) {
        return {};
      }
      std::string out(static_cast<std::size_t>(n), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
      return out;
    }

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

  bool open_with_retry() {
    for (int attempt = 0; attempt < 20; ++attempt) {
      if (OpenClipboard(nullptr)) {
        return true;
      }
      Sleep(15);
    }
    return false;
  }

  std::uint32_t sequence() {
    return GetClipboardSequenceNumber();
  }

  status_e snapshot(std::uint32_t &seq, std::string &type) {
    // Short retries only: this answers a request on the HTTPS io thread.
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (OpenClipboard(nullptr)) {
        seq = GetClipboardSequenceNumber();
        type = current_type();
        CloseClipboard();
        return status_e::ok;
      }
      Sleep(10);
    }
    return status_e::busy;
  }

  namespace {
    std::string user_object_name(HANDLE object) {
      if (!object) {
        return "?";
      }
      wchar_t name[256] = {};
      DWORD needed = 0;
      if (!GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed)) {
        return "?";
      }
      std::string out;
      for (const wchar_t *c = name; *c; ++c) {
        out += *c < 128 ? static_cast<char>(*c) : '?';
      }
      return out;
    }
  }  // namespace

  std::string describe_for_log() {
    std::string out = "station " + user_object_name(GetProcessWindowStation()) + ", desktop " +
                      user_object_name(GetThreadDesktop(GetCurrentThreadId()));
    DWORD owner_pid = 0;
    if (HWND owner = GetClipboardOwner()) {
      GetWindowThreadProcessId(owner, &owner_pid);
    }
    out += ", owner pid " + std::to_string(owner_pid) + ", " + std::to_string(CountClipboardFormats()) + " formats:";
    if (open_with_retry()) {
      for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        char name[128] = {};
        if (format >= 0xC000 && GetClipboardFormatNameA(format, name, sizeof(name)) > 0) {
          out += std::string(" ") + name;
        } else {
          out += " " + std::to_string(format);
        }
      }
      CloseClipboard();
    } else {
      out += " (clipboard busy)";
    }
    return out;
  }

  std::string current_type() {
    // IsClipboardFormatAvailable does not need the clipboard to be open.
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      return "text";
    }
    if (IsClipboardFormatAvailable(png_format()) || IsClipboardFormatAvailable(CF_DIB) ||
        IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_BITMAP)) {
      return "image";
    }
    if (IsClipboardFormatAvailable(CF_HDROP)) {
      return "files";
    }
    return "none";
  }

  status_e get_text(std::string &text) {
    text.clear();
    // IsClipboardFormatAvailable does not need the clipboard to be open.
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      return status_e::none;
    }
    if (!open_with_retry()) {
      return status_e::busy;
    }
    // The format is listed, so a failure from here on is a read error, not an empty clipboard.
    status_e status = status_e::failed;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle != nullptr) {
      auto *data = static_cast<const wchar_t *>(GlobalLock(handle));
      if (data != nullptr) {
        // Up to the terminating null, but never past the end of the block
        std::size_t length = wcsnlen(data, GlobalSize(handle) / sizeof(wchar_t));
        text = narrow(std::wstring(data, length));
        GlobalUnlock(handle);
        status = status_e::ok;
      }
    }
    CloseClipboard();
    return status;
  }

  status_e set_text(const std::string &text) {
    // Windows programs expect CR LF line breaks; a line feed already after a CR is kept as is.
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 2);
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
        crlf += '\r';
      }
      crlf += text[i];
    }
    // Invalid UTF-8 becomes U+FFFD instead of failing the whole text.
    std::wstring wide;
    if (!crlf.empty()) {
      int n = MultiByteToWideChar(CP_UTF8, 0, crlf.data(), static_cast<int>(crlf.size()), nullptr, 0);
      if (n <= 0) {
        return status_e::failed;
      }
      wide.assign(static_cast<std::size_t>(n), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, crlf.data(), static_cast<int>(crlf.size()), wide.data(), n);
    }
    HGLOBAL global = make_global(std::string(reinterpret_cast<const char *>(wide.c_str()), (wide.size() + 1) * sizeof(wchar_t)));
    if (global == nullptr) {
      return status_e::failed;
    }
    if (!open_with_retry()) {
      GlobalFree(global);
      return status_e::busy;
    }
    // After a successful SetClipboardData the system owns the handle.
    bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, global) != nullptr;
    CloseClipboard();
    if (!ok) {
      GlobalFree(global);
      return status_e::failed;
    }
    return status_e::ok;
  }

  std::string dib_to_png(const std::string &dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
      return {};
    }
    BITMAPINFOHEADER header;
    std::memcpy(&header, dib.data(), sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > dib.size()) {
      return {};
    }

    // Offset of the pixel array inside the packed DIB: header, then masks (only for a plain
    // BITMAPINFOHEADER; V4/V5 headers embed them), then the color table.
    std::size_t offset = header.biSize;
    if (header.biSize == sizeof(BITMAPINFOHEADER) && (header.biCompression == BI_BITFIELDS || header.biCompression == 6 /* BI_ALPHABITFIELDS */)) {
      offset += (header.biCompression == BI_BITFIELDS ? 3 : 4) * sizeof(DWORD);
    }
    std::size_t colors = header.biClrUsed;
    if (colors == 0 && header.biBitCount <= 8) {
      colors = std::size_t {1} << header.biBitCount;
    }
    offset += colors * sizeof(RGBQUAD);
    if (offset >= dib.size()) {
      return {};
    }

    // WIC decodes BMP files, so prepend a BITMAPFILEHEADER.
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42;  // "BM"
    file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
    file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
    std::string bmp(reinterpret_cast<const char *>(&file), sizeof(file));
    bmp += dib;

    com_scope com;
    auto factory = wic_factory();
    if (!factory) {
      return {};
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(bmp.data()), static_cast<DWORD>(bmp.size())))) {
      return {};
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), &GUID_ContainerFormatBmp, WICDecodeMetadataCacheOnDemand, &decoder))) {
      return {};
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
      return {};
    }
    return encode_png(factory.Get(), frame.Get());
  }

  bool png_size(const std::string &png, std::uint32_t &width, std::uint32_t &height) {
    // Signature (8 bytes), then the IHDR chunk: length (4), "IHDR", width and height (big-endian).
    if (png.size() < 24 || png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0 || png.compare(12, 4, "IHDR") != 0) {
      return false;
    }
    auto be32 = [&png](std::size_t at) {
      std::uint32_t v = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        v = (v << 8) | static_cast<std::uint8_t>(png[at + i]);
      }
      return v;
    };
    width = be32(16);
    height = be32(20);
    return true;
  }

  std::string png_to_dibv5(const std::string &png) {
    if (png.empty() || png.size() > max_image_bytes) {
      return {};
    }
    com_scope com;
    auto factory = wic_factory();
    if (!factory) {
      return {};
    }
    std::string copy = png;  // InitializeFromMemory needs a mutable buffer
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE *>(copy.data()), static_cast<DWORD>(copy.size())))) {
      return {};
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), &GUID_ContainerFormatPng, WICDecodeMetadataCacheOnDemand, &decoder))) {
      return {};
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
      return {};
    }
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > max_image_pixels) {
      return {};
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return {};
    }

    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    std::string pixels(stride * height, '\0');
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels.size()), reinterpret_cast<BYTE *>(pixels.data())))) {
      return {};
    }

    BITMAPV5HEADER header {};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = static_cast<LONG>(height);  // bottom-up: the most widely supported layout
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(pixels.size());
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = 0x73524742;  // LCS_sRGB ('sRGB'); the header macro is a multi-char literal
    header.bV5Intent = LCS_GM_IMAGES;

    std::string dib(reinterpret_cast<const char *>(&header), sizeof(header));
    dib.reserve(sizeof(header) + pixels.size());
    for (UINT row = height; row-- > 0;) {
      dib.append(pixels.data() + row * stride, stride);
    }
    return dib;
  }

  status_e get_image_png(std::string &png) {
    png.clear();
    // IsClipboardFormatAvailable does not need the clipboard to be open.
    if (!IsClipboardFormatAvailable(png_format()) && !IsClipboardFormatAvailable(CF_DIB) &&
        !IsClipboardFormatAvailable(CF_DIBV5) && !IsClipboardFormatAvailable(CF_BITMAP)) {
      return status_e::none;
    }
    if (!open_with_retry()) {
      return status_e::busy;
    }
    std::string dib;
    if (IsClipboardFormatAvailable(png_format())) {
      png = read_global(GetClipboardData(png_format()));
    }
    if (png.empty()) {
      // Prefer CF_DIB over CF_DIBV5: many apps write an unreliable alpha channel in V5 data,
      // which would turn the image transparent. Windows synthesizes CF_DIB when needed.
      dib = read_global(GetClipboardData(CF_DIB));
    }
    CloseClipboard();

    if (png.empty() && !dib.empty()) {
      png = dib_to_png(dib);
    }
    // An image format is listed, so no data (the program that copied it did not render it) or a
    // DIB that does not convert is an image that cannot be sent, not an empty clipboard.
    if (png.empty()) {
      return status_e::not_convertible;
    }
    if (png.size() > max_image_bytes) {
      png.clear();
      return status_e::too_large;
    }
    return status_e::ok;
  }

  status_e set_image_png(const std::string &png) {
    std::uint32_t width = 0, height = 0;
    if (!png_size(png, width, height)) {
      return status_e::not_convertible;
    }
    // Decode first so an invalid or oversized image never clears the current clipboard.
    std::string dib = png_to_dibv5(png);
    if (dib.empty()) {
      return status_e::failed;
    }
    HGLOBAL png_global = make_global(png);
    HGLOBAL dib_global = make_global(dib);
    if (png_global == nullptr || dib_global == nullptr) {
      if (png_global) {
        GlobalFree(png_global);
      }
      if (dib_global) {
        GlobalFree(dib_global);
      }
      return status_e::failed;
    }

    if (!open_with_retry()) {
      GlobalFree(png_global);
      GlobalFree(dib_global);
      return status_e::busy;
    }
    // After a successful SetClipboardData the system owns that handle; free the rest ourselves.
    bool png_owned = false;
    bool dib_owned = false;
    bool ok = EmptyClipboard() != 0;
    if (ok) {
      png_owned = SetClipboardData(png_format(), png_global) != nullptr;
      ok = png_owned;
    }
    if (ok) {
      dib_owned = SetClipboardData(CF_DIBV5, dib_global) != nullptr;
      ok = dib_owned;
    }
    CloseClipboard();
    if (!png_owned) {
      GlobalFree(png_global);
    }
    if (!dib_owned) {
      GlobalFree(dib_global);
    }
    return ok ? status_e::ok : status_e::failed;
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

  status_e get_file_drop_list(std::vector<std::filesystem::path> &paths) {
    paths.clear();
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
      return status_e::none;
    }
    if (!open_with_retry()) {
      return status_e::busy;
    }
    auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (drop != nullptr) {
      UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        path.resize(length);
        if (!path.empty()) {
          paths.emplace_back(path);
        }
      }
    }
    CloseClipboard();
    return paths.empty() ? status_e::none : status_e::ok;
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

  status_e set_file_drop_list(const std::vector<std::filesystem::path> &paths) {
    if (paths.empty()) {
      return status_e::failed;
    }
    std::wstring list;
    for (const auto &p : paths) {
      list += p.wstring();
      list.push_back(L'\0');
    }
    list.push_back(L'\0');

    std::string drop(sizeof(DROPFILES), '\0');
    DROPFILES header {};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(drop.data(), &header, sizeof(header));
    drop.append(reinterpret_cast<const char *>(list.data()), list.size() * sizeof(wchar_t));

    DWORD effect = 1;  // DROPEFFECT_COPY: paste copies instead of moving out of the staging folder
    std::string effect_bytes(reinterpret_cast<const char *>(&effect), sizeof(effect));
    static const UINT effect_format = RegisterClipboardFormatW(L"Preferred DropEffect");

    HGLOBAL drop_global = make_global(drop);
    HGLOBAL effect_global = make_global(effect_bytes);
    const bool allocated = drop_global != nullptr && effect_global != nullptr;
    if (!allocated || !open_with_retry()) {
      if (drop_global) {
        GlobalFree(drop_global);
      }
      if (effect_global) {
        GlobalFree(effect_global);
      }
      return allocated ? status_e::busy : status_e::failed;
    }
    bool drop_owned = false;
    bool effect_owned = false;
    bool ok = EmptyClipboard() != 0;
    if (ok) {
      drop_owned = SetClipboardData(CF_HDROP, drop_global) != nullptr;
      ok = drop_owned;
    }
    if (ok) {
      effect_owned = SetClipboardData(effect_format, effect_global) != nullptr;
      ok = effect_owned;
    }
    CloseClipboard();
    if (!drop_owned) {
      GlobalFree(drop_global);
    }
    if (!effect_owned) {
      GlobalFree(effect_global);
    }
    return ok ? status_e::ok : status_e::failed;
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
