/**
 * @file src/live_fec.h
 * @brief Shell: the pure parts of live FEC changes (docs/features.md "Live FEC changes"): the
 * `percent` argument of GET /actions/fec and the per-frame choice between the client's override
 * and the configured percentage. No logging and no Shell globals, so shell/tests/live_fec_test.cpp
 * can build it alone.
 */
#pragma once

#include <optional>
#include <string_view>

namespace stream::live_fec {
  /// The most a client can ask for: parity packets equal to the data packets.
  constexpr int max_percent = 100;

  /**
   * @brief The FEC percentage of the next frame.
   * @param override_percent The client's live request (0 = none).
   * @param configured_percent `fec_percentage` from the config.
   * @return The override when set, else the configured percentage: without a request nothing changes.
   */
  constexpr int effective_percent(int override_percent, int configured_percent) {
    return override_percent > 0 ? override_percent : configured_percent;
  }

  /**
   * @brief Parses the `percent` query argument of GET /actions/fec.
   * @return 0 for `default` or `0` (clear the override), 1..max_percent for a non-negative integer
   * (values above max_percent are clamped to it), nothing for anything else (empty, a sign, letters,
   * more than six digits).
   */
  constexpr std::optional<int> parse_percent(std::string_view arg) {
    if (arg == "default") {
      return 0;
    }
    if (arg.empty() || arg.size() > 6) {
      return std::nullopt;
    }
    int value = 0;
    for (const char c : arg) {
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      value = value * 10 + (c - '0');
    }
    return value > max_percent ? max_percent : value;
  }
}  // namespace stream::live_fec
