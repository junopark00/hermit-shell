// Standalone checks for src/platform/cursor_shape.h (local cursor): the conversion of the three
// Desktop Duplication pointer shape formats to BGRA32 with straight alpha, the FNV-1a shape id,
// the 256x256 cap and the rejection of short buffers. Synthetic data only; no Windows API.
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
    cs::raw_t raw;
    raw.type = cs::type_masked_color;
    raw.width = 2;
    raw.height = 1;
    raw.pitch = 8;
    raw.data.assign(8, 0);
    put_pixel(raw.data, raw.pitch, 0, 0, 0x00AABBCC);  // alpha 0: opaque colour
    put_pixel(raw.data, raw.pitch, 1, 0, 0xFF010203);  // alpha FF: XOR with the screen

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("alpha 0 becomes opaque colour", pixel(shape, 0, 0) == 0xFFAABBCC);
    check("XOR pixel becomes opaque mid-grey", pixel(shape, 1, 0) == cs::xor_approximation);
  }

  std::printf("monochrome shape\n");
  {
    // 8x2 visible pixels: height is 4 rows (AND mask, then XOR mask), pitch 1 byte
    cs::raw_t raw;
    raw.type = cs::type_monochrome;
    raw.width = 8;
    raw.height = 4;
    raw.pitch = 1;
    raw.hotspot_x = 3;
    raw.hotspot_y = 0;
    // Row 0: AND 0b00110011, XOR 0b01010101 -> black, white, grey, transparent pattern:
    //   x0: and0 xor0 black; x1: and0 xor1 white; x2: and1 xor0 transparent; x3: and1 xor1 grey ...
    // Row 1: AND 0xFF, XOR 0x00 -> all transparent
    raw.data = {0x33, 0xFF, 0x55, 0x00};

    cs::bgra_t shape;
    check("converts", cs::convert(raw, shape) == cs::convert_e::ok);
    check("visible height is half the mask height", shape.width == 8 && shape.height == 2);
    check("black", pixel(shape, 0, 0) == 0xFF000000);
    check("white", pixel(shape, 1, 0) == 0xFFFFFFFF);
    check("transparent", pixel(shape, 2, 0) == 0x00000000);
    check("inverted becomes grey", pixel(shape, 3, 0) == cs::xor_approximation);
    check("pattern repeats", pixel(shape, 4, 0) == 0xFF000000 && pixel(shape, 7, 0) == cs::xor_approximation);
    check("second row transparent", pixel(shape, 0, 1) == 0 && pixel(shape, 7, 1) == 0);
    check("hotspot kept", shape.hotspot_x == 3 && shape.hotspot_y == 0);

    raw.data.resize(3);
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
