// Option B: integration test over the real libei↔libeis transport. A minimal libeis server mimics KWin's EIS (create+resume devices on seat bind; rebuild the absolute device on "resize", as KWin's updateScreens/changeDevice does) and the bridge's EisInjector drives it over a socketpair — no D-Bus, no KWin, no GPU. Single-threaded lockstep: pump both contexts until a predicate holds.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libei-1.0/libeis.h>
#include <poll.h>
#include <unistd.h>

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "evdev_util.hpp"
#include "kwin-input-bridge.hpp"

namespace {

using kibtest::fmt;
using kibtest::mk;
using kibtest::syn;
using ::testing::Contains;

int count(const std::vector<std::string>& v, const std::string& s) {
  int n = 0;
  for (const auto& x : v)
    if (x == s)
      ++n;
  return n;
}
bool contains(const std::vector<std::string>& v, const std::string& s) { return count(v, s) > 0; }

// libeis server modelled on KWin: pointer + absolute + keyboard devices, added then resumed on seat bind; the absolute device is destroyed and recreated on resize (KWin's changeDevice = remove + add + resume).
struct FakeEis {
  eis* ctx = nullptr;
  eis_seat* seat = nullptr;
  eis_device* pointer = nullptr;
  eis_device* abs = nullptr;
  eis_device* keyboard = nullptr;
  int start_emulating = 0;
  double rel_dx = 0, rel_dy = 0;  // accumulated: forward() emits one motion_rel per axis, and libei may coalesce
  std::vector<std::string> events;
  bool valid_ = false;

  FakeEis() {
    ctx = eis_new(nullptr);
    valid_ = ctx && eis_setup_backend_fd(ctx) == 0;
  }
  ~FakeEis() {
    if (pointer)
      eis_device_unref(pointer);
    if (abs)
      eis_device_unref(abs);
    if (keyboard)
      eis_device_unref(keyboard);
    if (seat)
      eis_seat_unref(seat);
    if (ctx)
      eis_unref(ctx);
  }

  bool valid() const { return valid_; }
  int add_client() { return eis_backend_fd_add_client(ctx); }
  int fd() { return eis_get_fd(ctx); }

  eis_device* new_device(
      const char* name,
      std::initializer_list<eis_device_capability> caps,
      std::uint32_t rw = 0,
      std::uint32_t rh = 0) {
    eis_device* d = eis_seat_new_device(seat);
    eis_device_configure_name(d, name);
    for (auto c : caps) eis_device_configure_capability(d, c);
    if (rw && rh) {
      eis_region* r = eis_device_new_region(d);
      eis_region_set_size(r, rw, rh);
      eis_region_set_offset(r, 0, 0);
      eis_region_add(r);
      eis_region_unref(r);
    }
    eis_device_add(d);
    eis_device_resume(d);
    return d;
  }

  void create_devices() {
    pointer = new_device("test pointer", {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL});
    abs = new_device("test abs", {EIS_DEVICE_CAP_POINTER_ABSOLUTE}, 2560, 1440);
    keyboard = new_device("test kbd", {EIS_DEVICE_CAP_KEYBOARD});
  }

  void resize_abs(std::uint32_t w, std::uint32_t h) {
    eis_device_remove(abs);
    eis_device_unref(abs);
    abs = new_device("test abs", {EIS_DEVICE_CAP_POINTER_ABSOLUTE}, w, h);
  }

  void pump() {
    eis_dispatch(ctx);
    while (eis_event* e = eis_get_event(ctx)) {
      handle(e);
      eis_event_unref(e);
    }
  }

  void handle(eis_event* e) {
    switch (eis_event_get_type(e)) {
      case EIS_EVENT_CLIENT_CONNECT: {
        eis_client* c = eis_event_get_client(e);
        eis_client_connect(c);
        seat = eis_client_new_seat(c, "seat0");
        eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER);
        eis_seat_configure_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
        eis_seat_configure_capability(seat, EIS_DEVICE_CAP_BUTTON);
        eis_seat_configure_capability(seat, EIS_DEVICE_CAP_SCROLL);
        eis_seat_configure_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
        eis_seat_add(seat);
        break;
      }
      case EIS_EVENT_SEAT_BIND:
        if (!pointer)
          create_devices();
        break;
      case EIS_EVENT_DEVICE_START_EMULATING:
        ++start_emulating;
        break;
      case EIS_EVENT_POINTER_MOTION:
        rel_dx += eis_event_pointer_get_dx(e);
        rel_dy += eis_event_pointer_get_dy(e);
        events.push_back(fmt("motion %g %g", eis_event_pointer_get_dx(e), eis_event_pointer_get_dy(e)));
        break;
      case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
        events.push_back(fmt("abs %g %g", eis_event_pointer_get_absolute_x(e), eis_event_pointer_get_absolute_y(e)));
        break;
      case EIS_EVENT_BUTTON_BUTTON:
        events.push_back(fmt("button %u %d", eis_event_button_get_button(e), eis_event_button_get_is_press(e)));
        break;
      case EIS_EVENT_SCROLL_DISCRETE:
        events.push_back(fmt("scroll %d %d", eis_event_scroll_get_discrete_dx(e), eis_event_scroll_get_discrete_dy(e)));
        break;
      case EIS_EVENT_KEYBOARD_KEY:
        events.push_back(fmt("key %u %d", eis_event_keyboard_get_key(e), eis_event_keyboard_get_key_is_press(e)));
        break;
      case EIS_EVENT_FRAME:
        events.push_back("frame");
        break;
      default:
        break;
    }
  }
};

// Lockstep both contexts (socketpair, no threads) until `done` or a generous round cap.
bool pump_until(FakeEis& server, kib::EisInjector& inj, std::function<bool()> done, int max_rounds = 300) {
  for (int i = 0; i < max_rounds; ++i) {
    if (done())
      return true;
    pollfd pfds[2] = {{server.fd(), POLLIN, 0}, {inj.fd(), POLLIN, 0}};
    poll(pfds, 2, 20);
    server.pump();
    inj.process_events();
  }
  return done();
}

// Drive forward() through the injector via a pipe, exactly as the production main loop does.
void feed(
    kib::EisInjector& inj,
    kib::Role role,
    const std::vector<input_event>& evs,
    bool has_abs = false,
    int mx = 0,
    int my = 0) {
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  for (const auto& e : evs) ASSERT_EQ(::write(fds[1], &e, sizeof(e)), static_cast<ssize_t>(sizeof(e)));
  ::close(fds[1]);
  kib::Source src;
  src.fd = kib::unique_fd(fds[0]);
  src.role = role;
  src.has_abs = has_abs;
  src.abs_max_x = mx;
  src.abs_max_y = my;
  kib::forward(src, inj);
}

// Establishes a fresh connection per test: server creates+resumes the devices, the bridge adopts and arms them.
class EisIntegration : public ::testing::Test {
 protected:
  FakeEis server;
  kib::EisInjector inj;

  void SetUp() override {
    ASSERT_TRUE(server.valid());
    int cfd = server.add_client();
    ASSERT_GE(cfd, 0);
    inj.attach(kib::unique_fd(cfd));
    ASSERT_TRUE(pump_until(server, inj, [&] { return inj.ready(); }));
    ASSERT_TRUE(pump_until(server, inj, [&] { return server.start_emulating >= 3; }));
  }
};

// pointer + abs + keyboard each armed via start_emulating on RESUMED.
TEST_F(EisIntegration, HandshakeArmsAllDevices) { EXPECT_GE(server.start_emulating, 3); }

TEST_F(EisIntegration, ForwardsRelativeMotionAndButton) {
  feed(inj, kib::Role::relative, {mk(EV_REL, REL_X, 7), mk(EV_REL, REL_Y, -3), syn(), mk(EV_KEY, BTN_LEFT, 1), syn()});
  ASSERT_TRUE(pump_until(server, inj, [&] { return count(server.events, "frame") >= 2; }));
  EXPECT_DOUBLE_EQ(server.rel_dx, 7);
  EXPECT_DOUBLE_EQ(server.rel_dy, -3);
  EXPECT_THAT(server.events, Contains("button 272 1"));
}

TEST_F(EisIntegration, ForwardsKeyboard) {
  feed(inj, kib::Role::keyboard, {mk(EV_KEY, KEY_A, 1), syn()});
  ASSERT_TRUE(pump_until(server, inj, [&] { return contains(server.events, "key 30 1"); }));
  EXPECT_THAT(server.events, Contains("key 30 1"));
}

TEST_F(EisIntegration, ForwardsWheel) {
  feed(inj, kib::Role::relative, {mk(EV_REL, REL_WHEEL_HI_RES, 120), syn()});
  ASSERT_TRUE(pump_until(server, inj, [&] { return contains(server.events, "scroll 0 -120"); }));
  EXPECT_THAT(server.events, Contains("scroll 0 -120"));
}

// Absolute motion is scaled to the device region (2560×1440): norm 0.5 → 1280×720.
TEST_F(EisIntegration, AbsoluteMotionUsesDeviceRegion) {
  feed(inj, kib::Role::absolute, {mk(EV_ABS, ABS_X, 50), mk(EV_ABS, ABS_Y, 50), syn()}, true, 100, 100);
  ASSERT_TRUE(pump_until(server, inj, [&] { return contains(server.events, "abs 1280 720"); }));
  EXPECT_THAT(server.events, Contains("abs 1280 720"));
}

// Chord keys press in order and release in reverse, each in its own frame.
TEST_F(EisIntegration, TapSendsChordPressAndReverseRelease) {
  kib::tap_send(inj, {KEY_LEFTCTRL, KEY_C});
  ASSERT_TRUE(pump_until(server, inj, [&] { return count(server.events, "frame") >= 4; }));
  std::vector<std::string> keys;
  for (const auto& e : server.events)
    if (e.rfind("key ", 0) == 0)
      keys.push_back(e);
  EXPECT_THAT(keys, ::testing::ElementsAre("key 29 1", "key 46 1", "key 46 0", "key 29 0"));
}

// Regression guard: a virtual-output resize recreates the absolute device (REMOVE+ADD+RESUME on a still-connected context — KWin's changeDevice). The bridge must re-arm the new device and adopt its new region. Eager-start-once would leave it resumed-but-not-emulating and silently drop absolute motion forever.
TEST_F(EisIntegration, ResizeRecreatesAndReArmsAbsolute) {
  int before = server.start_emulating;
  server.resize_abs(1920, 1080);
  ASSERT_TRUE(pump_until(server, inj, [&] { return server.start_emulating > before; }));
  EXPECT_GT(server.start_emulating, before);  // recreated abs device was re-armed

  server.events.clear();
  feed(inj, kib::Role::absolute, {mk(EV_ABS, ABS_X, 50), mk(EV_ABS, ABS_Y, 50), syn()}, true, 100, 100);
  ASSERT_TRUE(pump_until(server, inj, [&] { return contains(server.events, "abs 960 540"); }));
  EXPECT_THAT(server.events, Contains("abs 960 540"));  // 0.5×1920, 0.5×1080 — new region, emulation alive after resize
}

}  // namespace
