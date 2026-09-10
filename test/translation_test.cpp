// Option A: hermetic unit tests for the evdev → EIS translation (forward()). A RecordingSink captures the calls forward() makes; synthetic input_event frames are fed through a pipe. No libei, no KWin.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "evdev_util.hpp"
#include "kwin-input-bridge.hpp"

// evdev codes the expectations below hard-code; ≥ BTN_MISC (0x100) is the key-vs-button trap.
static_assert(BTN_LEFT == 272, "evdev code assumption");
static_assert(KEY_A == 30, "evdev code assumption");
static_assert(KEY_OK == 352, "evdev code assumption");

namespace {

using kibtest::fmt;
using kibtest::mk;
using kibtest::syn;
using ::testing::ElementsAre;

struct RecordingSink : kib::EisSink {
  std::vector<std::string> log;
  void motion_rel(double dx, double dy) override { log.push_back(fmt("rel %g %g", dx, dy)); }
  void motion_abs(double x, double y) override { log.push_back(fmt("abs %g %g", x, y)); }
  void button(std::uint32_t c, bool p) override { log.push_back(fmt("btn %u %d", c, p)); }
  void scroll(std::int32_t dx, std::int32_t dy) override { log.push_back(fmt("scroll %d %d", dx, dy)); }
  void key(std::uint32_t c, bool p) override { log.push_back(fmt("key %u %d", c, p)); }
  void commit(bool p, bool a, bool k) override { log.push_back(fmt("commit %d %d %d", p, a, k)); }
};

// forward() reads until EAGAIN or EOF; closing the write end gives a deterministic EOF.
std::vector<std::string>
run_forward(kib::Role role, const std::vector<input_event>& evs, bool has_abs = false, int max_x = 0, int max_y = 0) {
  int fds[2];
  EXPECT_EQ(pipe(fds), 0);
  for (const auto& e : evs) EXPECT_EQ(::write(fds[1], &e, sizeof(e)), static_cast<ssize_t>(sizeof(e)));
  ::close(fds[1]);
  kib::Source src;
  src.fd = kib::unique_fd(fds[0]);
  src.role = role;
  src.has_abs = has_abs;
  src.abs_max_x = max_x;
  src.abs_max_y = max_y;
  RecordingSink sink;
  kib::forward(src, sink);
  return sink.log;
}

// The trailing "commit 0 0 0" in each expectation is forward()'s end-of-read flush.

TEST(Translation, RelativeMotionAndButton) {
  EXPECT_THAT(
      run_forward(kib::Role::relative, {mk(EV_REL, REL_X, 5), mk(EV_KEY, BTN_LEFT, 1), syn()}),
      ElementsAre("rel 5 0", "btn 272 1", "commit 1 0 0", "commit 0 0 0"));
}

TEST(Translation, KeyboardLowCode) {
  EXPECT_THAT(
      run_forward(kib::Role::keyboard, {mk(EV_KEY, KEY_A, 1), syn()}),
      ElementsAre("key 30 1", "commit 0 0 1", "commit 0 0 0"));
}

// A multimedia KEY_* code above BTN_MISC must still route to key(), not button().
TEST(Translation, KeyboardHighCodeIsNotAButton) {
  EXPECT_THAT(
      run_forward(kib::Role::keyboard, {mk(EV_KEY, KEY_OK, 1), syn()}),
      ElementsAre("key 352 1", "commit 0 0 1", "commit 0 0 0"));
}

// The same code on a mouse device is a button.
TEST(Translation, MouseButton) {
  EXPECT_THAT(
      run_forward(kib::Role::relative, {mk(EV_KEY, BTN_LEFT, 1), syn()}),
      ElementsAre("btn 272 1", "commit 1 0 0", "commit 0 0 0"));
}

// Hi-res scroll forwards 1:1 (already in libei's 1/120 units); evdev +up → libei −y, +right → +x.
TEST(Translation, WheelHiRes) {
  EXPECT_THAT(
      run_forward(
          kib::Role::relative,
          {mk(EV_REL, REL_WHEEL_HI_RES, 30), syn(), mk(EV_REL, REL_HWHEEL_HI_RES, 30), syn()}),
      ElementsAre("scroll 0 -30", "commit 1 0 0", "scroll 30 0", "commit 1 0 0", "commit 0 0 0"));
}

// The coarse REL_WHEEL/REL_HWHEEL echo is dropped — only the hi-res channel scrolls — so a sub-detent touchpad frame (coarse 0, hi-res present) still moves.
TEST(Translation, CoarseWheelIgnored) {
  EXPECT_THAT(
      run_forward(kib::Role::relative, {mk(EV_REL, REL_WHEEL, 1), mk(EV_REL, REL_WHEEL_HI_RES, 30), syn()}),
      ElementsAre("scroll 0 -30", "commit 1 0 0", "commit 0 0 0"));
}

// Autorepeat (value 2) is dropped — KWin generates its own — so no key() and the frame commits empty.
TEST(Translation, AutorepeatDropped) {
  EXPECT_THAT(
      run_forward(kib::Role::keyboard, {mk(EV_KEY, KEY_A, 2), syn()}),
      ElementsAre("commit 0 0 0", "commit 0 0 0"));
}

TEST(Translation, SwallowedKeyDropped) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  for (const auto& e : {mk(EV_KEY, KEY_F13, 1), mk(EV_KEY, KEY_A, 1), syn()})
    ASSERT_EQ(::write(fds[1], &e, sizeof(e)), static_cast<ssize_t>(sizeof(e)));
  ::close(fds[1]);
  kib::Source src;
  src.fd = kib::unique_fd(fds[0]);
  src.role = kib::Role::keyboard;
  RecordingSink sink;
  kib::forward(src, sink, {KEY_F13});
  EXPECT_THAT(sink.log, ElementsAre("key 30 1", "commit 0 0 1", "commit 0 0 0"));
}

// Absolute motion is normalised against the device's ABS max (here 100×100).
TEST(Translation, AbsoluteScaling) {
  EXPECT_THAT(
      run_forward(
          kib::Role::absolute,
          {mk(EV_ABS, ABS_X, 50), mk(EV_ABS, ABS_Y, 25), syn()},
          /*has_abs=*/true,
          /*max_x=*/100,
          /*max_y=*/100),
      ElementsAre("abs 0.5 0", "abs 0.5 0.25", "commit 0 1 0", "commit 0 0 0"));
}

// Both axes of a frame reach libei as a single motion vector: a client that overwrites rather than sums consecutive deltas must still see the diagonal.
TEST(Translation, RelativeAxesCoalesceIntoOneMotion) {
  EXPECT_THAT(
      run_forward(kib::Role::relative, {mk(EV_REL, REL_X, 3), mk(EV_REL, REL_Y, 4), syn()}),
      ElementsAre("rel 3 4", "commit 1 0 0", "commit 0 0 0"));
}

// A button in the middle of a frame keeps its position relative to the motion preceding it.
TEST(Translation, RelativeMotionFlushesBeforeButton) {
  EXPECT_THAT(
      run_forward(
          kib::Role::relative,
          {mk(EV_REL, REL_X, 3), mk(EV_REL, REL_Y, 4), mk(EV_KEY, BTN_LEFT, 1), mk(EV_REL, REL_X, 5), syn()}),
      ElementsAre("rel 3 4", "btn 272 1", "rel 5 0", "commit 1 0 0", "commit 0 0 0"));
}

// One commit per SYN_REPORT.
TEST(Translation, FramingPerSyn) {
  EXPECT_THAT(
      run_forward(kib::Role::relative, {mk(EV_REL, REL_X, 1), syn(), mk(EV_REL, REL_X, 2), syn()}),
      ElementsAre("rel 1 0", "commit 1 0 0", "rel 2 0", "commit 1 0 0", "commit 0 0 0"));
}

}  // namespace
