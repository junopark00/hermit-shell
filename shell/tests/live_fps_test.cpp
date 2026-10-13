// Standalone checks for src/live_fps.h (live frame rate changes): the `fps` argument of
// GET /actions/fps (integers, `default` and `0` as "clear", rejected text), the frame rate a request
// gives (never above the stream's, never below min_fps), and which captured frames the encoder keeps
// at a lowered rate (60 -> 30 and 60 -> 20 at real 16.667 ms timestamps, capture jitter, late frames),
// and the encode loop with a held image (frames reaching the encoder after their capture time, a held
// image encoded at its deadline, a stream rate far above the capture rate).
//
// Build and run from the repository root in MSYS2 UCRT64 (one line):
//   g++ -std=c++20 -O1 -iquote src shell/tests/live_fps_test.cpp -o build/live_fps_test.exe
//   build/live_fps_test.exe
#include "live_fps.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

namespace lf = stream::live_fps;

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
  const auto value = lf::parse_fps(arg);
  return value && *value == expected;
}

static bool rejected(std::string_view arg) {
  return !lf::parse_fps(arg).has_value();
}

/// Runs the encoder's decimation over frames captured at the given times (microseconds) and
/// returns the indices of the frames it keeps, the way the encode loop does after a change.
static std::vector<int> kept_frames(const std::vector<std::int64_t> &times_us, int fps) {
  const std::int64_t interval_us = 1000000 / fps;
  std::int64_t next_us = 0;
  std::vector<int> kept;
  for (int i = 0; i < (int) times_us.size(); ++i) {
    if (!lf::keep_frame(times_us[i], next_us, interval_us / 4)) {
      continue;
    }
    next_us = lf::next_deadline(next_us, times_us[i], interval_us);
    kept.push_back(i);
  }
  return kept;
}

/// Capture times of `count` frames at `source_fps` from t = 1 s, rounded to whole microseconds.
static std::vector<std::int64_t> capture_times(int count, int source_fps) {
  std::vector<std::int64_t> times;
  for (int i = 0; i < count; ++i) {
    times.push_back(1000000 + (std::int64_t) i * 1000000 / source_fps);
  }
  return times;
}

/// One encoded frame of simulate_loop.
struct encoded_t {
  std::int64_t ts_us;  ///< capture time
  std::int64_t at_us;  ///< when the encoder took it
  bool held;  ///< it was the held image, encoded at its deadline
};

/// Result of simulate_loop.
struct loop_result_t {
  std::vector<encoded_t> encoded;
  std::int64_t max_ahead_us = 0;  ///< the most the next deadline was ahead of now + one interval
};

/// Models the encode loop of video.cpp with a held image: frames captured at `times_us` reach the
/// encoder `latency_us` later (encoding takes no time); a frame that is not due is held, and a held
/// image waits held_wait_us for a newer one, else it is encoded (and the next deadline is kept within
/// one interval of now).
static loop_result_t simulate_loop(const std::vector<std::int64_t> &times_us, std::int64_t latency_us, int fps) {
  const std::int64_t interval_us = 1000000 / fps;
  loop_result_t result;
  std::int64_t next_us = 0;
  std::int64_t now_us = 0;
  std::optional<std::int64_t> held_ts;
  std::size_t i = 0;
  while (i < times_us.size() || held_ts) {
    std::int64_t ts_us;
    bool held = false;
    if (held_ts) {
      const std::int64_t deadline_us = now_us + lf::held_wait_us(next_us, now_us, interval_us);
      if (i < times_us.size() && times_us[i] + latency_us <= deadline_us) {
        now_us = std::max(now_us, times_us[i] + latency_us);
        ts_us = times_us[i++];
      } else {
        now_us = deadline_us;
        ts_us = *held_ts;
        held = true;
      }
      held_ts.reset();
    } else {
      now_us = std::max(now_us, times_us[i] + latency_us);
      ts_us = times_us[i++];
    }
    const bool due = lf::keep_frame(ts_us, next_us, interval_us / 4);
    if (!held && !due) {
      held_ts = ts_us;
      continue;
    }
    next_us = lf::next_deadline(next_us, ts_us, interval_us);
    if (held || !due) {
      next_us = std::min(next_us, now_us + interval_us);
    }
    result.encoded.push_back({ts_us, now_us, held});
    result.max_ahead_us = std::max(result.max_ahead_us, next_us - (now_us + interval_us));
  }
  return result;
}

/// Held images encoded before the last frame (the last one is always held: nothing newer comes).
static int held_before_last(const loop_result_t &result) {
  int count = 0;
  for (std::size_t i = 0; i + 1 < result.encoded.size(); ++i) {
    count += result.encoded[i].held ? 1 : 0;
  }
  return count;
}

/// Longest time between two encoded frames.
static std::int64_t max_gap_us(const loop_result_t &result) {
  std::int64_t gap = 0;
  for (std::size_t i = 1; i < result.encoded.size(); ++i) {
    gap = std::max(gap, result.encoded[i].at_us - result.encoded[i - 1].at_us);
  }
  return gap;
}

/// True when the kept indices after the first `skip` ones step by exactly `step`.
static bool steps_by(const std::vector<int> &kept, int skip, int step) {
  if ((int) kept.size() < skip + 2) {
    return false;
  }
  for (std::size_t i = skip + 1; i < kept.size(); ++i) {
    if (kept[i] - kept[i - 1] != step) {
      return false;
    }
  }
  return true;
}

int main() {
  std::printf("parse_fps\n");
  check("30 stays 30", parses_to("30", 30));
  check("1 parses (effective_fps raises it)", parses_to("1", 1));
  check("240 parses (effective_fps caps it)", parses_to("240", 240));
  check("999999 parses", parses_to("999999", 999999));
  check("0 clears the override", parses_to("0", 0));
  check("default clears the override", parses_to("default", 0));
  check("empty is rejected", rejected(""));
  check("a sign is rejected", rejected("-30"));
  check("a plus sign is rejected", rejected("+30"));
  check("letters are rejected", rejected("thirty"));
  check("a decimal is rejected", rejected("29.97"));
  check("trailing text is rejected", rejected("30fps"));
  check("more than six digits are rejected", rejected("1000000"));
  check("Default (upper case) is rejected", rejected("Default"));

  std::printf("effective_fps\n");
  check("no override: the stream's frame rate", lf::effective_fps(0, 60) == 60);
  check("a lower request wins", lf::effective_fps(30, 60) == 30);
  check("a request equal to the stream's rate is the stream's rate", lf::effective_fps(60, 60) == 60);
  check("a higher request is capped to the stream's rate", lf::effective_fps(120, 60) == 60);
  check("a request below min_fps is raised to it", lf::effective_fps(5, 60) == lf::min_fps);
  check("a request of 1 is raised to min_fps", lf::effective_fps(1, 120) == 10);
  check("min_fps itself is kept", lf::effective_fps(10, 60) == 10);
  check("a negative override counts as none", lf::effective_fps(-1, 60) == 60);
  check("a stream below min_fps is not raised", lf::effective_fps(5, 8) == 8);

  std::printf("keep_frame\n");
  check("the first frame after a change is kept", lf::keep_frame(1000000, 0, 8333));
  check("a frame at its deadline is kept", lf::keep_frame(50000, 50000, 8333));
  check("a frame inside the tolerance is kept", lf::keep_frame(41667, 50000, 8333));
  check("a frame just outside the tolerance is dropped", !lf::keep_frame(41666, 50000, 8333));
  check("a frame half an interval early is dropped", !lf::keep_frame(33333, 50000, 8333));
  check("a late frame is kept", lf::keep_frame(90000, 50000, 8333));

  std::printf("next_deadline\n");
  check("on time: one interval after the previous deadline", lf::next_deadline(50000, 50000, 33333) == 83333);
  check("slightly late: the schedule holds", lf::next_deadline(50000, 60000, 33333) == 83333);
  check("early inside the tolerance: the schedule holds", lf::next_deadline(50000, 42000, 33333) == 83333);
  check("first frame: one interval after it", lf::next_deadline(0, 1000000, 33333) == 1033333);
  check("just under half an interval late: the schedule holds", lf::next_deadline(50000, 66665, 33333) == 83333);
  check("half an interval late: re-anchored one interval after the frame", lf::next_deadline(50000, 66666, 33333) == 99999);
  check("much later: re-anchored one interval after the frame", lf::next_deadline(50000, 500000, 33333) == 533333);

  std::printf("decimation\n");
  {
    const auto kept = kept_frames(capture_times(600, 60), 30);
    // The first frame anchors the schedule one interval later, so the second is dropped.
    check("60 -> 30: the first frame is kept, the second dropped", kept.size() > 2 && kept[0] == 0 && kept[1] == 2);
    check("60 -> 30: every other frame", steps_by(kept, 0, 2));
    check("60 -> 30: 300 of 600 frames (10 s)", kept.size() == 300);
  }
  {
    const auto kept = kept_frames(capture_times(600, 60), 20);
    check("60 -> 20: every third frame", steps_by(kept, 0, 3));
    check("60 -> 20: 200 of 600 frames (10 s)", kept.size() == 200);
  }
  {
    const auto kept = kept_frames(capture_times(1200, 120), 30);
    // The fourth frame lands exactly on the tolerance edge (25 ms after the first); then every fourth.
    check("120 -> 30: every fourth frame", kept.size() > 2 && kept[1] == 3 && steps_by(kept, 1, 4));
  }
  {
    // 60 -> 40: the deadlines fall between frames; two of every three are kept, not every other.
    const auto kept = kept_frames(capture_times(600, 60), 40);
    check("60 -> 40: 2 of 3 frames (~400 of 600)", kept.size() >= 399 && kept.size() <= 401);
  }
  {
    // Capture jitter of +-3 ms on every frame at 60 -> 30: still every other frame.
    auto times = capture_times(600, 60);
    for (std::size_t i = 0; i < times.size(); ++i) {
      times[i] += (i % 3 == 0) ? 3000 : (i % 3 == 1 ? -3000 : 0);
    }
    const auto kept = kept_frames(times, 30);
    check("60 -> 30 with +-3 ms jitter: every other frame", steps_by(kept, 0, 2));
  }
  {
    // A pause (static content) of 2 s, then 60 fps again: the schedule re-anchors on the first frame
    // after the pause instead of letting a burst through to catch up.
    auto times = capture_times(60, 60);
    const auto resume = capture_times(60, 60);
    for (const auto t : resume) {
      times.push_back(t + 3000000);
    }
    const auto kept = kept_frames(times, 30);
    int after_pause = 0;
    for (const int i : kept) {
      after_pause += i >= 60 ? 1 : 0;
    }
    check("after a 2 s pause: no burst (30 of 60 frames kept)", after_pause == 30);
  }

  std::printf("held_wait_us\n");
  check("deadline ahead: until the deadline plus the tolerance", lf::held_wait_us(50000, 30000, 33333) == 28333);
  check("deadline passed but inside the tolerance: the rest of it", lf::held_wait_us(50000, 55000, 33333) == 3333);
  check("deadline and tolerance passed: no wait", lf::held_wait_us(50000, 70000, 33333) == 0);
  check("deadline far ahead: at most an interval and a quarter", lf::held_wait_us(500000, 0, 33333) == 41666);

  std::printf("encode loop with a held image\n");
  {
    // The due frame reaches the encoder 3 ms after its capture time; the older held image must not win.
    const auto result = simulate_loop(capture_times(600, 60), 3000, 30);
    check("60 -> 30, 3 ms latency: no held image encoded while frames come", held_before_last(result) == 0);
    check("60 -> 30, 3 ms latency: 300 new frames + the last one held", result.encoded.size() == 301);
  }
  {
    const auto result = simulate_loop(capture_times(1200, 120), 3000, 30);
    check("120 -> 30, 3 ms latency: no held image encoded while frames come", held_before_last(result) == 0);
    check("120 -> 30, 3 ms latency: ~300 frames in 10 s", result.encoded.size() >= 299 && result.encoded.size() <= 302);
  }
  {
    // Two changes, then nothing: the second is held and encoded at its deadline plus the tolerance.
    const auto result = simulate_loop({1000000, 1010000}, 0, 30);
    check("held image: encoded when nothing newer comes", result.encoded.size() == 2 && result.encoded[1].held);
    check("held image: at the deadline plus the tolerance", result.encoded.size() == 2 && result.encoded[1].at_us == 1033333 + 8333);
  }
  {
    // Stream at 240 fps (interval far below the old minimum-fps wait), capture at 40 fps, lowered to
    // 30: about 30 fps, no long gap, and the deadline never runs ahead of the clock.
    const auto result = simulate_loop(capture_times(400, 40), 3000, 30);
    const auto fps10 = result.encoded.size();
    check("240 -> 30 at a 40 fps capture: ~30 fps (290..310 in 10 s)", fps10 >= 290 && fps10 <= 310);
    check("240 -> 30 at a 40 fps capture: no gap above two intervals", max_gap_us(result) <= 2 * 33333);
    check("240 -> 30 at a 40 fps capture: next stays within now + interval + tolerance", result.max_ahead_us <= 33333 / 4);
  }

  static_assert(lf::parse_fps("default").value() == 0, "default at compile time");
  static_assert(lf::effective_fps(30, 60) == 30, "lower at compile time");
  static_assert(lf::next_deadline(0, 100, 50) == 150, "anchor at compile time");
  static_assert(lf::held_wait_us(30, 0, 40) == 40, "held wait at compile time");

  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
