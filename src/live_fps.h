/**
 * @file src/live_fps.h
 * @brief Shell: the pure parts of live frame rate changes (docs/features.md "Live frame rate
 * changes"): the `fps` argument of GET /actions/fps, the frame rate a request gives, and which
 * captured frames the encoder keeps at that rate. No logging and no Shell globals, so
 * shell/tests/live_fps_test.cpp can build it alone.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace stream::live_fps {
  /// The lowest frame rate a client can ask for; lower requests are raised to it.
  constexpr int min_fps = 10;

  /**
   * @brief Parses the `fps` query argument of GET /actions/fps.
   * @return 0 for `default` or `0` (clear the override), the value for any other non-negative
   * integer of at most six digits, nothing for anything else (empty, a sign, letters).
   */
  constexpr std::optional<int> parse_fps(std::string_view arg) {
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
    return value;
  }

  /**
   * @brief The frame rate the encoder runs at.
   * @param override_fps The client's live request (0 = none).
   * @param stream_fps The frame rate the client asked for at launch.
   * @return The stream's frame rate without a request or for a request at or above it, else the
   * request raised to min_fps (but never above the stream's rate): a client can only lower its own
   * frame rate.
   */
  constexpr int effective_fps(int override_fps, int stream_fps) {
    if (override_fps <= 0 || override_fps >= stream_fps) {
      return stream_fps;
    }
    return std::min(std::max(override_fps, min_fps), stream_fps);
  }

  /**
   * @brief Whether the encoder encodes a captured frame or drops it while the frame rate is lowered.
   * @param ts_us Capture time of the frame (steady clock, microseconds).
   * @param next_us When the next frame is due (0 right after a change: the first frame is kept).
   * @param tolerance_us How early a frame may come and still count as due, for capture jitter.
   */
  constexpr bool keep_frame(std::int64_t ts_us, std::int64_t next_us, std::int64_t tolerance_us) {
    return ts_us >= next_us - tolerance_us;
  }

  /**
   * @brief When the frame after a kept one is due.
   * @param next_us When the kept frame was due (0 for the first frame after a change).
   * @param ts_us Capture time of the kept frame.
   * @param interval_us 1,000,000 / the lowered frame rate.
   * @return One interval after the previous deadline, so jitter and a capture rate that is not a
   * multiple of the lowered rate do not slow it down. One interval after the frame itself for the
   * first frame and for a frame half an interval or more late (after static content): the schedule
   * re-anchors instead of letting a burst of frames through to catch up.
   */
  constexpr std::int64_t next_deadline(std::int64_t next_us, std::int64_t ts_us, std::int64_t interval_us) {
    if (next_us == 0 || ts_us - next_us >= interval_us / 2) {
      return ts_us + interval_us;
    }
    return next_us + interval_us;
  }

  /**
   * @brief How long the encoder waits for a newer image while it holds a dropped one.
   * @param next_us When the next frame is due (capture time, steady clock, microseconds).
   * @param now_us The steady clock now.
   * @param interval_us 1,000,000 / the lowered frame rate.
   * @return The time until the deadline plus the keep_frame tolerance (interval / 4): a due frame
   * reaches the encoder a little after its capture time and must win over the older held image.
   * Never negative and never more than one interval and a quarter, so a deadline far ahead cannot
   * stall the picture.
   */
  constexpr std::int64_t held_wait_us(std::int64_t next_us, std::int64_t now_us, std::int64_t interval_us) {
    return std::clamp<std::int64_t>(next_us + interval_us / 4 - now_us, 0, interval_us + interval_us / 4);
  }
}  // namespace stream::live_fps
