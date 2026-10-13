// Standalone checks for src/live_fec.h (live FEC changes): the `percent` argument of
// GET /actions/fec (integers, the 100 clamp, `default` and `0` as "clear", rejected text) and the
// per-frame precedence of the client's override over the configured percentage.
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/live_fec_test.cpp -o build/live_fec_test.exe
//   build/live_fec_test.exe
#include "live_fec.h"

#include <cstdio>
#include <optional>
#include <string_view>

namespace lf = stream::live_fec;

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

static bool parses_to(std::string_view arg, int expected) {
  const auto value = lf::parse_percent(arg);
  return value && *value == expected;
}

static bool rejected(std::string_view arg) {
  return !lf::parse_percent(arg).has_value();
}

int main() {
  std::printf("parse_percent\n");
  check("1 is the lowest percentage", parses_to("1", 1));
  check("20 stays 20", parses_to("20", 20));
  check("100 stays 100", parses_to("100", 100));
  check("101 is clamped to 100", parses_to("101", 100));
  check("999999 is clamped to 100", parses_to("999999", 100));
  check("0 clears the override", parses_to("0", 0));
  check("default clears the override", parses_to("default", 0));
  check("empty is rejected", rejected(""));
  check("a sign is rejected", rejected("-5"));
  check("a plus sign is rejected", rejected("+5"));
  check("letters are rejected", rejected("abc"));
  check("trailing text is rejected", rejected("20%"));
  check("a decimal is rejected", rejected("2.5"));
  check("more than six digits are rejected", rejected("1000000"));
  check("Default (upper case) is rejected", rejected("Default"));

  std::printf("effective_percent\n");
  check("no override: the configured percentage", lf::effective_percent(0, 20) == 20);
  check("override above the configured value wins", lf::effective_percent(50, 20) == 50);
  check("override below the configured value wins", lf::effective_percent(5, 20) == 5);
  check("override of 100 wins", lf::effective_percent(lf::max_percent, 20) == 100);
  check("a negative override counts as none", lf::effective_percent(-1, 20) == 20);
  check("the configured value passes through unclamped", lf::effective_percent(0, 255) == 255);

  static_assert(lf::parse_percent("150").value() == lf::max_percent, "clamp at compile time");
  static_assert(lf::effective_percent(0, 20) == 20, "default at compile time");

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
