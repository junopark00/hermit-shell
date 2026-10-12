// Standalone checks for src/platform/cursor_shape.h (local cursor): the conversion of the three
// Desktop Duplication pointer shape formats to BGRA32 with straight alpha (including the white
// fill and black outline that stand in for screen-inverting pixels), the FNV-1a shape id, the
// 256x256 cap and the rejection of short buffers. Synthetic data only; no Windows API.
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/cursor_shape_test.cpp -o build/cursor_shape_test.exe
//   build/cursor_shape_test.exe
#include "platform/cursor_shape.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cs = platf::cursor_shape;

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

static std::uint32_t pixel(const cs::bgra_t &shape, unsigned x, unsigned y) {
  std::uint32_t v;
  std::memcpy(&v, shape.pixels.data() + (static_cast<std::size_t>(y) * shape.width + x) * 4, 4);
  return v;  // little-endian: 0xAARRGGBB
}

static void put_pixel(std::vector<std::uint8_t> &data, unsigned pitch, unsigned x, unsigned y, std::uint32_t argb) {
  std::memcpy(data.data() + static_cast<std::size_t>(y) * pitch + x * 4, &argb, 4);
}

int main() {
  std::printf("fnv1a\n");
  {
    // Reference values of 32-bit FNV-1a
    check("empty input is the offset basis", cs::fnv1a("", 0) == 0x811c9dc5u);
    check("\"a\"", cs::fnv1a("a", 1) == 0xe40c292cu);
    check("\"foobar\"", cs::fnv1a("foobar", 6) == 0xbf9cf968u);
    check("continuation equals one run", cs::fnv1a("bar", 3, cs::fnv1a("foo", 3)) == cs::fnv1a("foobar", 6));
  }

  std::printf("colour shape\n");
  {
    cs::raw_t raw;
    raw.type = cs::type_color;
    raw.width = 3;
    raw.height = 2;
    raw.pitch = 16;  // wider than 3 * 4: the padding must not reach the output
    raw.hotspot_x = 1;
    raw.hotspot_y = 1;
    raw.data.assign(raw.pitch * raw.height, 0xEE);
    put_pixel(raw.data, raw.pitch, 0, 0, 0x80112233);
    put_pixel(raw.data, raw.pitch, 2, 1, 0x00000000);
    put_pixel(raw.data, raw.pitch, 1, 1, 0xFFFFFFFF);

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("size", shape.width == 3 && shape.height == 2 && shape.pixels.size() == 3 * 2 * 4);
    check("hotspot", shape.hotspot_x == 1 && shape.hotspot_y == 1);
    check("pixels copied with their alpha", pixel(shape, 0, 0) == 0x80112233 && pixel(shape, 2, 1) == 0 && pixel(shape, 1, 1) == 0xFFFFFFFF);
    check("row padding skipped", pixel(shape, 0, 1) == 0xEEEEEEEE);
    check("id is not 0", shape.id != 0);

    // The id follows the content: same shape, same id; one byte changed, another id
    cs::bgra_t again;
    cs::convert(raw, again);
    check("id is stable", again.id == shape.id);
    put_pixel(raw.data, raw.pitch, 0, 0, 0x80112234);
    cs::convert(raw, again);
    check("id changes with a pixel", again.id != shape.id);
    put_pixel(raw.data, raw.pitch, 0, 0, 0x80112233);
    raw.hotspot_x = 0;
    cs::convert(raw, again);
    check("id changes with the hotspot", again.id != shape.id);

    // Hotspot outside the shape is clamped
    raw.hotspot_x = 10;
    raw.hotspot_y = 10;
    cs::convert(raw, again);
    check("hotspot clamped into the shape", again.hotspot_x == 2 && again.hotspot_y == 1);

    // Buffer shorter than the declared rows
    raw.data.resize(raw.pitch * raw.height - 8);
    check("short buffer rejected", cs::convert(raw, again) == cs::convert_e::invalid);
    raw.data.resize(raw.pitch * raw.height);
    raw.pitch = 8;  // narrower than the row
    check("pitch narrower than the row rejected", cs::convert(raw, again) == cs::convert_e::invalid);
  }

  std::printf("masked colour shape\n");
  {
    // One row: opaque colour | XOR white (inverts) | XOR black (leaves the screen: transparent) | XOR black
    cs::raw_t raw;
    raw.type = cs::type_masked_color;
    raw.width = 4;
    raw.height = 1;
    raw.pitch = 16;
    raw.data.assign(16, 0);
    put_pixel(raw.data, raw.pitch, 0, 0, 0x00AABBCC);  // alpha 0: opaque colour
    put_pixel(raw.data, raw.pitch, 1, 0, 0xFFFFFFFF);  // alpha FF, RGB FFFFFF: the screen inverted
    put_pixel(raw.data, raw.pitch, 2, 0, 0xFF000000);  // alpha FF, RGB 0: XOR with 0, the screen unchanged
    put_pixel(raw.data, raw.pitch, 3, 0, 0xFF000000);

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("alpha 0 becomes opaque colour", pixel(shape, 0, 0) == 0xFFAABBCC);
    check("XOR with a colour becomes opaque white", pixel(shape, 1, 0) == cs::inverted_fill);
    check("XOR with RGB 0 next to an inverted pixel is the black outline", pixel(shape, 2, 0) == cs::inverted_outline);
    check("XOR with RGB 0 away from inverted pixels is transparent", pixel(shape, 3, 0) == 0x00000000);
    check("opaque neighbour of an inverted pixel is not outlined", pixel(shape, 0, 0) == 0xFFAABBCC);

    // A partial inversion (any non-zero RGB) counts as inverted
    put_pixel(raw.data, raw.pitch, 1, 0, 0xFF010203);
    check("XOR with a dim colour is inverted too", cs::convert(raw, shape) == cs::convert_e::ok && pixel(shape, 1, 0) == cs::inverted_fill);
  }

  std::printf("monochrome bit order\n");
  {
    // 8x1 visible pixels, pitch 1: AND 0x7F clears only bit 7, so pixel 0 (the MSB) is the one opaque pixel
    cs::raw_t raw;
    raw.type = cs::type_monochrome;
    raw.width = 8;
    raw.height = 2;
    raw.pitch = 1;
    raw.data = {0x7F, 0x00};  // AND row, XOR row

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("visible height is half the mask height", shape.width == 8 && shape.height == 1);
    check("pixel 0 is the MSB: AND 0 XOR 0 is black", pixel(shape, 0, 0) == 0xFF000000);
    check("pixel 7 is the LSB: AND 1 XOR 0 is transparent", pixel(shape, 7, 0) == 0x00000000);
    check("no inverted pixel, no outline", pixel(shape, 1, 0) == 0x00000000);

    raw.data = {0xFE, 0x01};  // only pixel 7: AND 0 XOR 1 -> white
    check("AND 0 XOR 1 is white", cs::convert(raw, shape) == cs::convert_e::ok && pixel(shape, 7, 0) == 0xFFFFFFFF && pixel(shape, 0, 0) == 0);
    raw.data = {0xFF, 0x80};  // only pixel 0: AND 1 XOR 1 -> inverted
    check("AND 1 XOR 1 is inverted (white) with a black outline", cs::convert(raw, shape) == cs::convert_e::ok && pixel(shape, 0, 0) == cs::inverted_fill && pixel(shape, 1, 0) == cs::inverted_outline && pixel(shape, 2, 0) == 0);
  }

  std::printf("monochrome I-beam\n");
  {
    // 16x16 visible pixels (32 mask rows, pitch 2). Everything is AND 1 (transparent) except row 15,
    // whose pixels 0..7 are opaque: 0..3 black, 4..7 white. The XOR mask draws an I-beam of inverted
    // pixels: serifs at x 5..10 on rows 1 and 14, the stem at x 7..8 on rows 2..13.
    cs::raw_t raw;
    raw.type = cs::type_monochrome;
    raw.width = 16;
    raw.height = 32;
    raw.pitch = 2;
    raw.hotspot_x = 7;
    raw.hotspot_y = 7;
    raw.data.assign(64, 0xFF);  // AND mask: all 1
    raw.data[15 * 2] = 0x00;  // AND row 15: x 0..7 opaque
    for (unsigned y = 0; y < 16; ++y) {  // XOR mask: rows 16..31
      raw.data[32 + y * 2] = 0;
      raw.data[32 + y * 2 + 1] = 0;
    }
    raw.data[32 + 1 * 2] = 0x07;  // row 1: x5, x6, x7
    raw.data[32 + 1 * 2 + 1] = 0xE0;  // row 1: x8, x9, x10
    for (unsigned y = 2; y <= 13; ++y) {
      raw.data[32 + y * 2] = 0x01;  // x7
      raw.data[32 + y * 2 + 1] = 0x80;  // x8
    }
    raw.data[32 + 14 * 2] = 0x07;
    raw.data[32 + 14 * 2 + 1] = 0xE0;
    raw.data[32 + 15 * 2] = 0x0F;  // row 15: x 4..7 XOR 1 (with AND 0: white); x 0..3 XOR 0 (black)

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("size and hotspot", shape.width == 16 && shape.height == 16 && shape.hotspot_x == 7 && shape.hotspot_y == 7);

    // Inverted pixels: opaque white
    bool beam_white = true;
    for (unsigned x = 5; x <= 10; ++x) {
      beam_white = beam_white && pixel(shape, x, 1) == cs::inverted_fill && pixel(shape, x, 14) == cs::inverted_fill;
    }
    for (unsigned y = 2; y <= 13; ++y) {
      beam_white = beam_white && pixel(shape, 7, y) == cs::inverted_fill && pixel(shape, 8, y) == cs::inverted_fill;
    }
    check("every inverted pixel is opaque white", beam_white);

    // Transparent pixels 4-adjacent to the beam: opaque black
    check("outline above the top serif", pixel(shape, 5, 0) == cs::inverted_outline && pixel(shape, 10, 0) == cs::inverted_outline);
    check("outline left and right of the serif", pixel(shape, 4, 1) == cs::inverted_outline && pixel(shape, 11, 1) == cs::inverted_outline);
    check("outline under the serif ends", pixel(shape, 5, 2) == cs::inverted_outline && pixel(shape, 10, 2) == cs::inverted_outline);
    check("outline along the stem", pixel(shape, 6, 7) == cs::inverted_outline && pixel(shape, 9, 7) == cs::inverted_outline);
    check("outline under the bottom serif", pixel(shape, 8, 15) == cs::inverted_outline && pixel(shape, 10, 15) == cs::inverted_outline);

    // Transparent pixels only diagonal to the beam, or next to an outline pixel, stay transparent
    check("diagonal neighbours stay transparent", pixel(shape, 4, 0) == 0 && pixel(shape, 11, 0) == 0 && pixel(shape, 4, 2) == 0 && pixel(shape, 11, 2) == 0 && pixel(shape, 11, 15) == 0);
    check("the outline does not grow", pixel(shape, 4, 2) == 0 && pixel(shape, 5, 7) == 0 && pixel(shape, 3, 1) == 0);
    check("far corner transparent", pixel(shape, 0, 0) == 0 && pixel(shape, 15, 15) == 0 && pixel(shape, 15, 7) == 0);

    // Opaque pixels keep their colour even next to the beam
    check("AND 0 XOR 0 is black", pixel(shape, 0, 15) == 0xFF000000 && pixel(shape, 3, 15) == 0xFF000000);
    check("AND 0 XOR 1 is white", pixel(shape, 4, 15) == 0xFFFFFFFF && pixel(shape, 6, 15) == 0xFFFFFFFF);
    check("opaque white under the beam is not outlined", pixel(shape, 7, 15) == 0xFFFFFFFF);

    // Deterministic: the same masks give the same id
    cs::bgra_t again;
    cs::convert(raw, again);
    check("id is stable", again.id == shape.id && again.pixels == shape.pixels);

    raw.data.resize(63);
    check("short mask buffer rejected", cs::convert(raw, shape) == cs::convert_e::invalid);
  }

  std::printf("limits\n");
  {
    cs::raw_t raw;
    raw.type = cs::type_color;
    raw.width = cs::max_dimension;
    raw.height = cs::max_dimension;
    raw.pitch = raw.width * 4;
    raw.data.assign(static_cast<std::size_t>(raw.pitch) * raw.height, 0x7F);
    cs::bgra_t shape;
    check("256x256 converts", cs::convert(raw, shape) == cs::convert_e::ok && shape.pixels.size() == 256u * 256u * 4u);

    raw.width = cs::max_dimension + 1;
    raw.pitch = raw.width * 4;
    raw.data.assign(static_cast<std::size_t>(raw.pitch) * raw.height, 0x7F);
    check("257 wide is oversize", cs::convert(raw, shape) == cs::convert_e::oversize);

    raw.width = 8;
    raw.pitch = 32;
    raw.height = cs::max_dimension + 1;
    raw.data.assign(static_cast<std::size_t>(raw.pitch) * raw.height, 0x7F);
    check("257 tall is oversize", cs::convert(raw, shape) == cs::convert_e::oversize);

    // A monochrome shape of 512 mask rows is 256 visible rows: allowed
    raw.type = cs::type_monochrome;
    raw.width = 8;
    raw.pitch = 1;
    raw.height = 512;
    raw.data.assign(512, 0);
    check("monochrome 256 visible rows converts", cs::convert(raw, shape) == cs::convert_e::ok && shape.height == 256);
    raw.height = 514;
    raw.data.assign(514, 0);
    check("monochrome 257 visible rows is oversize", cs::convert(raw, shape) == cs::convert_e::oversize);

    raw.type = 7;
    check("unknown type rejected", cs::convert(raw, shape) == cs::convert_e::invalid);
    raw.type = cs::type_color;
    raw.width = 0;
    check("zero width rejected", cs::convert(raw, shape) == cs::convert_e::invalid);
  }

  std::printf("wire\n");
  {
    cs::bgra_t shape;
    shape.id = 0x11223344;
    shape.width = 2;
    shape.height = 1;
    shape.hotspot_x = 1;
    shape.hotspot_y = 0;
    shape.pixels = {1, 2, 3, 4, 5, 6, 7, 8};

    std::vector<std::uint8_t> out;
    cs::wire::append_shape(out, shape, 0, shape.pixels.size());
    const std::uint8_t expected[] = {1, 0x44, 0x33, 0x22, 0x11, 2, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    check("shape payload layout", out.size() == sizeof(expected) && std::memcmp(out.data(), expected, sizeof(expected)) == 0);
    check("shape header size constant", cs::wire::shape_header_bytes == 17);

    out.clear();
    cs::wire::append_shape(out, shape, 4, 4);
    const std::uint8_t chunk[] = {1, 0x44, 0x33, 0x22, 0x11, 2, 0, 1, 0, 1, 0, 0, 0, 4, 0, 0, 0, 5, 6, 7, 8};
    check("shape chunk carries offset and its bytes", out.size() == sizeof(chunk) && std::memcmp(out.data(), chunk, sizeof(chunk)) == 0);

    out.clear();
    cs::wire::append_shape(out, shape, 6, 100);
    check("chunk past the end is clipped", out.size() == cs::wire::shape_header_bytes + 2 && out[17] == 7 && out[18] == 8);

    out.clear();
    cs::wire::append_state(out, 0xA1B2C3D4, true, false);
    const std::uint8_t state[] = {1, 0xD4, 0xC3, 0xB2, 0xA1, 1, 0};
    check("state payload layout", out.size() == sizeof(state) && std::memcmp(out.data(), state, sizeof(state)) == 0);
    out.clear();
    cs::wire::append_state(out, 0, false, true);
    check("state flags", out.size() == 7 && out[1] == 0 && out[5] == 0 && out[6] == 1);
  }

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
