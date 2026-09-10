// Forwards Sunshine's uinput devices into a headless kwin_wayland --virtual session. The virtual
// backend wires up no libinput, so the evdev devices Sunshine creates are replayed over KWin's EIS
// RemoteDesktop (libei) — the only input channel the virtual backend honours. KWin exposes relative
// and absolute pointer as separate EIS devices; relative motion sent to the absolute one is dropped.
//
// Declares the units exercised by the tests: the EisSink the evdev translation writes to, the
// EisInjector that implements it over libei, and forward(). EisInjector::attach() is the seam the
// integration test uses to drive a libeis server over a socketpair instead of KWin's D-Bus.

#pragma once

#include <libei-1.0/libei.h>
#include <linux/input.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kib {

class unique_fd {
 public:
  unique_fd() = default;
  explicit unique_fd(int fd) : fd_(fd) {}
  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  unique_fd(unique_fd&& o) noexcept : fd_(o.release()) {}
  unique_fd& operator=(unique_fd&& o) noexcept {
    reset(o.release());
    return *this;
  }
  ~unique_fd() { reset(); }
  int get() const { return fd_; }
  int release() {
    int f = fd_;
    fd_ = -1;
    return f;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = fd;
  }
  explicit operator bool() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

// unique_ptr whose deleter is a free function named in the type — stateless and zero-size, so it needs no per-type closer struct.
template<auto Free> struct c_deleter {
  template<class T> void operator()(T* p) const noexcept { Free(p); }
};
template<class T, auto Free> using c_unique_ptr = std::unique_ptr<T, c_deleter<Free>>;

using sd_bus_up = c_unique_ptr<sd_bus, sd_bus_flush_close_unref>;
using sd_msg_up = c_unique_ptr<sd_bus_message, sd_bus_message_unref>;
using ei_up = c_unique_ptr<ei, ei_unref>;
using ei_device_up = c_unique_ptr<ei_device, ei_device_unref>;

// Destination for translated evdev input. EisInjector is the production impl; tests record calls. Absolute coordinates are normalised 0..1; the sink applies any device-region scaling.
struct EisSink {
  virtual ~EisSink() = default;
  virtual void motion_rel(double dx, double dy) = 0;
  virtual void motion_abs(double x_norm, double y_norm) = 0;
  virtual void button(std::uint32_t code, bool press) = 0;
  virtual void scroll(std::int32_t dx, std::int32_t dy) = 0;
  virtual void key(std::uint32_t code, bool press) = 0;
  virtual void commit(bool pointer, bool abs_pointer, bool keyboard) = 0;
};

enum class Role { keyboard, absolute, relative };

struct Source {
  unique_fd fd;
  Role role = Role::relative;
  std::string path;
  bool has_abs = false;
  int abs_max_x = 0, abs_max_y = 0;
  int last_x = 0, last_y = 0;  // tracked to avoid an ioctl per ABS event
};

// EIS sender bound to KWin's EIS RemoteDesktop.
class EisInjector : public EisSink {
 public:
  EisInjector() = default;
  EisInjector(const EisInjector&) = delete;
  EisInjector& operator=(const EisInjector&) = delete;
  ~EisInjector() override { close(); }

  void connect();             // production: D-Bus connectToEIS → attach → negotiate
  void attach(unique_fd fd);  // test seam: set up the libei sender on a ready EIS fd (no D-Bus, no negotiate)
  bool process_events();      // drain the libei event queue; true if the server disconnected
  bool ready() const { return pointer_ && keyboard_; }
  int fd() const { return ei_get_fd(ei_.get()); }

  void motion_rel(double dx, double dy) override { ei_device_pointer_motion(pointer_.get(), dx, dy); }
  void motion_abs(double x_norm, double y_norm) override {
    if (abs_pointer_ && abs_region_w_ > 0)
      ei_device_pointer_motion_absolute(abs_pointer_.get(), x_norm * abs_region_w_, y_norm * abs_region_h_);
  }
  void button(std::uint32_t code, bool press) override { ei_device_button_button(pointer_.get(), code, press); }
  void scroll(std::int32_t dx, std::int32_t dy) override { ei_device_scroll_discrete(pointer_.get(), dx, dy); }
  void key(std::uint32_t code, bool press) override { ei_device_keyboard_key(keyboard_.get(), code, press); }
  void commit(bool pointer, bool abs_pointer, bool keyboard) override;

 private:
  void close();
  void frame(ei_device* d);
  bool handle_event(ei_event* raw);  // returns true on DISCONNECT
  void negotiate();
  void bind_seat(ei_seat* seat);
  void adopt_device(ei_device* device);
  void arm_device(ei_device* device);
  void drop_device(ei_device* device);

  sd_bus_up bus_;
  ei_up ei_;
  ei_device_up pointer_;      // relative pointer: motion + buttons + scroll
  ei_device_up abs_pointer_;  // absolute pointer (may be null; equal to pointer_ if KWin combines)
  ei_device_up keyboard_;
  std::uint32_t abs_region_w_ = 0;  // 0 until the abs device's region is adopted
  std::uint32_t abs_region_h_ = 0;
  std::uint32_t emu_seq_ = 0;  // start_emulating sequence; must rise on every call
  std::uint32_t cookie_ = 0;
  char cookie_type_ = 'i';  // wire type of the cookie ('i'/'u') — mirrored from connectToEIS reply
};

// Translate one evdev device's pending frames onto the sink, committing once per SYN_REPORT. Keyboard keys in swallow_keys are dropped. Returns false when the device has vanished and should be dropped for the next rescan to replace.
bool forward(Source& src, EisSink& sink, const std::vector<std::uint32_t>& swallow_keys = {});

// Press the chord's evdev key codes in order and release them in reverse, one frame per event, pausing pace_us between frames.
void tap_send(EisSink& sink, const std::vector<std::uint32_t>& codes, unsigned pace_us = 0);

// One-shot --tap mode: connect to KWin's EIS, send the chord, disconnect.
int tap(const std::vector<std::uint32_t>& codes);

void run(const std::vector<std::uint32_t>& swallow_keys = {});

}  // namespace kib
