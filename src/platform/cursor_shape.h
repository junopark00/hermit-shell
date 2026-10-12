/**
 * @file src/platform/cursor_shape.h
 * @brief Shell: conversion of a captured mouse pointer shape (the Desktop Duplication formats) to
 *        BGRA32 for clients that draw the cursor themselves (docs/features.md "Local cursor").
 *
 * Header-only and free of Windows and project headers so shell/tests/cursor_shape_test.cpp can
 * build it on its own. The capture side copies the raw shape into a raw_t; the consumer converts
 * it once with convert() and sends the result with the id from shape_id().
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace platf::cursor_shape {
  // DXGI_OUTDUPL_POINTER_SHAPE_TYPE values (dxgi1_2.h), repeated so this header needs no Windows headers.
  constexpr std::uint32_t type_monochrome = 1;  ///< AND mask over XOR mask, 1 bit per pixel, twice the visible height
  constexpr std::uint32_t type_color = 2;  ///< 32 bpp BGRA with straight alpha
  constexpr std::uint32_t type_masked_color = 4;  ///< 32 bpp BGR; alpha 0xFF marks pixels that invert the screen

  constexpr std::uint32_t max_dimension = 256;  ///< wider or taller shapes are not sent to clients
  constexpr std::uint32_t bytes_per_pixel = 4;
  /// Pixels that invert the screen (XOR) cannot be drawn by a static cursor; they become opaque
  /// mid-grey, which stays visible on white and on black backgrounds.
  constexpr std::uint32_t xor_approximation = 0xFF808080;

  /// A pointer shape as Desktop Duplication reports it (DXGI_OUTDUPL_POINTER_SHAPE_INFO plus the buffer).
  struct raw_t {
    std::uint32_t type = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;  ///< monochrome: AND and XOR masks stacked, so twice the visible height
    std::uint32_t pitch = 0;  ///< bytes per row in data
    std::uint32_t hotspot_x = 0;
    std::uint32_t hotspot_y = 0;
    std::vector<std::uint8_t> data;
  };

  /// A converted shape: top-down rows of B, G, R, A bytes with straight (not premultiplied) alpha.
  struct bgra_t {
    std::uint32_t id = 0;  ///< shape_id(); never 0 (0 on the wire means "no shape, use the default arrow")
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t hotspot_x = 0;  ///< < width
    std::uint16_t hotspot_y = 0;  ///< < height
    std::vector<std::uint8_t> pixels;  ///< width * height * 4 bytes
  };

  enum class convert_e {
    ok,
    oversize,  ///< wider or taller than max_dimension; out is left empty
    invalid,  ///< unknown type, zero size or a buffer too short for the declared size; out is left empty
  };

  /// 32-bit FNV-1a; `hash` continues an earlier run.
  inline std::uint32_t fnv1a(const void *data, std::size_t len, std::uint32_t hash = 2166136261u) {
    auto p = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < len; ++i) {
      hash ^= p[i];
      hash *= 16777619u;
    }
    return hash;
  }

  /// The id of a converted shape: FNV-1a over width, height, hotspot_x, hotspot_y (each as a
  /// little-endian u16, in this order) followed by the pixel bytes. 0 is mapped to 1 so that 0 can
  /// mean "no shape".
  inline std::uint32_t shape_id(const bgra_t &shape) {
    const std::uint16_t head[4] = {shape.width, shape.height, shape.hotspot_x, shape.hotspot_y};
    std::uint8_t bytes[8];
    for (int i = 0; i < 4; ++i) {
      bytes[i * 2] = static_cast<std::uint8_t>(head[i] & 0xFF);
      bytes[i * 2 + 1] = static_cast<std::uint8_t>(head[i] >> 8);
    }
    auto hash = fnv1a(bytes, sizeof(bytes));
    hash = fnv1a(shape.pixels.data(), shape.pixels.size(), hash);
    return hash ? hash : 1;
  }

  /// Convert a raw shape to BGRA32 with straight alpha. Colour shapes are copied, masked colour and
  /// monochrome shapes are flattened: opaque pixels keep their colour (black or white for
  /// monochrome), transparent pixels get alpha 0 and screen-inverting pixels become xor_approximation.
  /// The hotspot is clamped into the shape.
  inline convert_e convert(const raw_t &raw, bgra_t &out) {
    const bool monochrome = raw.type == type_monochrome;
    if (raw.type != type_color && raw.type != type_masked_color && !monochrome) {
      return convert_e::invalid;
    }

    const std::uint32_t width = raw.width;
    const std::uint32_t height = monochrome ? raw.height / 2 : raw.height;
    if (width == 0 || height == 0) {
      return convert_e::invalid;
    }
    if (width > max_dimension || height > max_dimension) {
      return convert_e::oversize;
    }

    // The buffer must hold every row the conversion reads
    const std::size_t row_bytes = monochrome ? (width + 7) / 8 : static_cast<std::size_t>(width) * bytes_per_pixel;
    const std::size_t rows = monochrome ? static_cast<std::size_t>(height) * 2 : height;
    if (raw.pitch < row_bytes || raw.data.size() < static_cast<std::size_t>(raw.pitch) * (rows - 1) + row_bytes) {
      return convert_e::invalid;
    }

    bgra_t shape;
    shape.width = static_cast<std::uint16_t>(width);
    shape.height = static_cast<std::uint16_t>(height);
    shape.hotspot_x = static_cast<std::uint16_t>(std::min(raw.hotspot_x, width - 1));
    shape.hotspot_y = static_cast<std::uint16_t>(std::min(raw.hotspot_y, height - 1));
    shape.pixels.resize(static_cast<std::size_t>(width) * height * bytes_per_pixel);

    const std::uint8_t grey[4] = {
      static_cast<std::uint8_t>(xor_approximation & 0xFF),
      static_cast<std::uint8_t>((xor_approximation >> 8) & 0xFF),
      static_cast<std::uint8_t>((xor_approximation >> 16) & 0xFF),
      static_cast<std::uint8_t>(xor_approximation >> 24),
    };

    for (std::uint32_t y = 0; y < height; ++y) {
      const std::uint8_t *src_row = raw.data.data() + static_cast<std::size_t>(y) * raw.pitch;
      std::uint8_t *dst_row = shape.pixels.data() + static_cast<std::size_t>(y) * width * bytes_per_pixel;

      if (raw.type == type_color) {
        std::memcpy(dst_row, src_row, row_bytes);
        continue;
      }

      if (raw.type == type_masked_color) {
        for (std::uint32_t x = 0; x < width; ++x) {
          const std::uint8_t *src = src_row + static_cast<std::size_t>(x) * bytes_per_pixel;
          std::uint8_t *dst = dst_row + static_cast<std::size_t>(x) * bytes_per_pixel;
          if (src[3] == 0xFF) {
            // XOR pixel: the screen colour inverted
            std::memcpy(dst, grey, 4);
          } else {
            // Opaque colour (alpha 0; other values are not legal and are treated the same way)
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 0xFF;
          }
        }
        continue;
      }

      // Monochrome: AND mask rows first, XOR mask rows after them
      const std::uint8_t *and_row = src_row;
      const std::uint8_t *xor_row = raw.data.data() + static_cast<std::size_t>(y + height) * raw.pitch;
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::uint8_t bit = static_cast<std::uint8_t>(0x80 >> (x & 7));
        const bool and_bit = (and_row[x / 8] & bit) != 0;
        const bool xor_bit = (xor_row[x / 8] & bit) != 0;
        std::uint8_t *dst = dst_row + static_cast<std::size_t>(x) * bytes_per_pixel;
        if (!and_bit) {
          // Opaque: black when the XOR bit is clear, white when it is set
          const std::uint8_t v = xor_bit ? 0xFF : 0x00;
          dst[0] = dst[1] = dst[2] = v;
          dst[3] = 0xFF;
        } else if (xor_bit) {
          std::memcpy(dst, grey, 4);
        } else {
          dst[0] = dst[1] = dst[2] = dst[3] = 0;
        }
      }
    }

    shape.id = shape_id(shape);
    out = std::move(shape);
    return convert_e::ok;
  }

  /// The wire layout shared by the control stream messages (stream.cpp) and
  /// GET /actions/cursor?shape=<id> (nvhttp.cpp). All integers little-endian.
  namespace wire {
    constexpr std::uint8_t version = 1;

    /// CURSOR_SHAPE payload (control stream type 0x3100), without the 4-byte control header:
    /// version u8, shape_id u32, width u16, height u16, hotspot_x u16, hotspot_y u16, offset u32,
    /// then the pixel bytes [offset, offset + count) of the BGRA32 image. A shape larger than one
    /// packet arrives as several messages with increasing offsets; the image is complete when
    /// offset + count == width * height * 4.
    constexpr std::size_t shape_header_bytes = 17;

    /// CURSOR_STATE payload (control stream type 0x3101): version u8, shape_id u32 (0 = no shape,
    /// show the default arrow), visible u8, host_composing u8 (1 = the cursor is also drawn into
    /// the video because another session did not ask for a local cursor).
    constexpr std::size_t state_bytes = 7;

    inline void put16(std::vector<std::uint8_t> &out, std::uint16_t v) {
      out.push_back(static_cast<std::uint8_t>(v & 0xFF));
      out.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    inline void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
      put16(out, static_cast<std::uint16_t>(v & 0xFFFF));
      put16(out, static_cast<std::uint16_t>(v >> 16));
    }

    /// Append a CURSOR_SHAPE payload carrying the pixel bytes [offset, offset + count).
    inline void append_shape(std::vector<std::uint8_t> &out, const bgra_t &shape, std::uint32_t offset, std::size_t count) {
      out.reserve(out.size() + shape_header_bytes + count);
      out.push_back(version);
      put32(out, shape.id);
      put16(out, shape.width);
      put16(out, shape.height);
      put16(out, shape.hotspot_x);
      put16(out, shape.hotspot_y);
      put32(out, offset);
      const std::size_t begin = std::min<std::size_t>(offset, shape.pixels.size());
      const std::size_t end = std::min<std::size_t>(begin + count, shape.pixels.size());
      out.insert(out.end(), shape.pixels.begin() + begin, shape.pixels.begin() + end);
    }

    /// Append a CURSOR_STATE payload.
    inline void append_state(std::vector<std::uint8_t> &out, std::uint32_t shape_id, bool visible, bool host_composing) {
      out.reserve(out.size() + state_bytes);
      out.push_back(version);
      put32(out, shape_id);
      out.push_back(visible ? 1 : 0);
      out.push_back(host_composing ? 1 : 0);
    }
  }  // namespace wire
}  // namespace platf::cursor_shape
