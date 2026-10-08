/**
 * @file src/platform/windows/clipboard_access.cpp
 * @brief Shell: clipboard reads and writes in the calling process (see clipboard_access.h).
 *
 * Linked by shell.exe and by the clipboard helper, tools/shellclip.exe. Depends on nothing else in
 * Shell (no logging, no Boost), so the helper stays small and the standalone test can link it.
 */
#include "clipboard_access.h"

#include <Windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <string>

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

  }  // namespace

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

  status_e in_process::snapshot(std::uint32_t &seq, std::string &type) {
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

  std::string in_process::describe_for_log() {
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

  status_e in_process::get_text(std::string &text) {
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

  status_e in_process::set_text(const std::string &text) {
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

  status_e in_process::get_image_png(std::string &png) {
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

  status_e in_process::set_image_png(const std::string &png) {
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

  status_e in_process::get_file_drop_list(std::vector<std::filesystem::path> &paths) {
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

  status_e in_process::set_file_drop_list(const std::vector<std::filesystem::path> &paths) {
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

}  // namespace platf::clipboard
