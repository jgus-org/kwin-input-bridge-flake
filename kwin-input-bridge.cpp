#include "kwin-input-bridge.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace kib {
namespace {

constexpr int kAllCaps = EI_DEVICE_CAP_POINTER | EI_DEVICE_CAP_POINTER_ABSOLUTE | EI_DEVICE_CAP_KEYBOARD |
                         EI_DEVICE_CAP_BUTTON | EI_DEVICE_CAP_SCROLL;

constexpr const char* kService = "org.kde.KWin";
constexpr const char* kPath = "/org/kde/KWin/EIS/RemoteDesktop";
constexpr const char* kInterface = "org.kde.KWin.EIS.RemoteDesktop";

volatile std::sig_atomic_t g_exit = 0;
void on_term(int) { g_exit = 1; }

std::string errstr() { return std::strerror(errno); }

uint64_t now_us() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000 + ts.tv_nsec / 1000;
}

const char* role_name(Role r) {
  switch (r) {
    case Role::keyboard:
      return "kbd";
    case Role::absolute:
      return "abs";
    case Role::relative:
      return "rel";
  }
  return "?";
}

using ei_event_up = c_unique_ptr<ei_event, ei_event_unref>;

}  // namespace

// ---- EisInjector ----

void EisInjector::attach(unique_fd fd) {
  ei* raw_ei = ei_new_sender(nullptr);
  if (!raw_ei)
    throw std::runtime_error("ei_new_sender failed");
  ei_.reset(raw_ei);
  ei_configure_name(ei_.get(), "sunshine-input-bridge");
  // ei_setup_backend_fd takes ownership of the fd on success.
  if (ei_setup_backend_fd(ei_.get(), fd.get()) != 0)
    throw std::runtime_error("ei_setup_backend_fd failed");
  fd.release();
}

void EisInjector::connect() {
  sd_bus* raw_bus = nullptr;
  if (sd_bus_open_user(&raw_bus) < 0 || !raw_bus)
    throw std::runtime_error("failed to open session D-Bus");
  bus_.reset(raw_bus);

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* raw_reply = nullptr;
  int caps = kAllCaps;
  int r = sd_bus_call_method(bus_.get(), kService, kPath, kInterface, "connectToEIS", &err, &raw_reply, "i", caps);
  sd_msg_up reply(raw_reply);
  if (r < 0) {
    std::string msg = "connectToEIS failed";
    if (sd_bus_error_is_set(&err))
      msg += ": " + std::string(err.message);
    throw std::runtime_error(msg);
  }

  const char* sig = sd_bus_message_get_signature(reply.get(), 1);
  if (!sig || sig[0] != 'h' || (sig[1] != 'i' && sig[1] != 'u'))
    throw std::runtime_error("unexpected connectToEIS reply signature: " + std::string(sig ? sig : "(none)"));

  // read_basic('h') does NOT dup — the fd stays owned by the message and is closed when it's unref'd below. Hand libei its own copy so each owns a distinct descriptor.
  int msg_fd = -1;
  if (sd_bus_message_read_basic(reply.get(), 'h', &msg_fd) < 0)
    throw std::runtime_error("failed to read EIS fd from reply");
  unique_fd backend(fcntl(msg_fd, F_DUPFD_CLOEXEC, 3));
  if (!backend)
    throw std::runtime_error("dup EIS fd failed: " + errstr());
  cookie_type_ = sig[1];
  if (cookie_type_ == 'i') {
    int32_t c = 0;
    if (sd_bus_message_read_basic(reply.get(), 'i', &c) < 0)
      throw std::runtime_error("failed to read EIS cookie");
    cookie_ = static_cast<std::uint32_t>(c);
  } else {
    if (sd_bus_message_read_basic(reply.get(), 'u', &cookie_) < 0)
      throw std::runtime_error("failed to read EIS cookie");
  }
  reply.reset();  // releases the original fd; `backend` is now the sole extra copy

  attach(std::move(backend));
  negotiate();
}

void EisInjector::commit(bool pointer, bool abs_pointer, bool keyboard) {
  if (pointer && pointer_)
    frame(pointer_.get());
  // KWin may hand one device both POINTER and POINTER_ABSOLUTE; don't frame it twice.
  if (abs_pointer && abs_pointer_ && abs_pointer_.get() != pointer_.get())
    frame(abs_pointer_.get());
  if (keyboard && keyboard_)
    frame(keyboard_.get());
  ei_dispatch(ei_.get());
}

void EisInjector::frame(ei_device* d) { ei_device_frame(d, now_us()); }

bool EisInjector::handle_event(ei_event* raw) {
  ei_event_up ev(raw);
  switch (ei_event_get_type(ev.get())) {
    case EI_EVENT_DISCONNECT:
      return true;
    case EI_EVENT_SEAT_ADDED:
      bind_seat(ei_event_get_seat(ev.get()));
      break;
    case EI_EVENT_DEVICE_ADDED:
      adopt_device(ei_event_get_device(ev.get()));
      break;
    case EI_EVENT_DEVICE_RESUMED:
      arm_device(ei_event_get_device(ev.get()));
      break;
    case EI_EVENT_DEVICE_REMOVED:
      drop_device(ei_event_get_device(ev.get()));
      break;
    default:
      break;  // PAUSED/MODIFIERS/PONG: drain. Sends during a server pause are dropped by
  }  // libei; the next RESUMED re-arms via arm_device().
  return false;
}

// start_emulating must follow RESUMED, not ADDED. KWin recreates the absolute device — REMOVE+ADD+RESUME on a connected context — whenever the virtual output is resized (kde-set-mode, fired per Moonlight connect via global_prep_cmd); a device recreated that way reaches RESUMED but would never be armed if we only started at handshake. KWin batches ADDED+RESUMED in one turn, so the initial devices arm here during negotiate with no added latency.
void EisInjector::arm_device(ei_device* device) {
  bool mine = (pointer_ && pointer_.get() == device) || (abs_pointer_ && abs_pointer_.get() == device) ||
              (keyboard_ && keyboard_.get() == device);
  if (mine)
    ei_device_start_emulating(device, emu_seq_++);
}

void EisInjector::negotiate() {
  constexpr uint64_t timeout_us = 5 * 1'000'000ULL;
  uint64_t deadline = now_us() + timeout_us;
  while (now_us() < deadline && !ready()) {
    ei_dispatch(ei_.get());
    while (ei_event* raw = ei_get_event(ei_.get()))
      if (handle_event(raw))
        throw std::runtime_error("EIS disconnected during handshake");
    if (!ready())
      usleep(100'000);
  }
  if (!ready())
    throw std::runtime_error("EIS handshake incomplete");
}

bool EisInjector::process_events() {
  ei_dispatch(ei_.get());
  bool disconnected = false;
  while (ei_event* raw = ei_get_event(ei_.get()))
    if (handle_event(raw))
      disconnected = true;
  return disconnected;
}

void EisInjector::bind_seat(ei_seat* seat) {
  // Binding a cap the seat doesn't advertise is a documented no-op, so bind the full set.
  ei_seat_bind_capabilities(
      seat,
      EI_DEVICE_CAP_POINTER,
      EI_DEVICE_CAP_POINTER_ABSOLUTE,
      EI_DEVICE_CAP_KEYBOARD,
      EI_DEVICE_CAP_BUTTON,
      EI_DEVICE_CAP_SCROLL,
      nullptr);
}

void EisInjector::adopt_device(ei_device* device) {
  if (!pointer_ && ei_device_has_capability(device, EI_DEVICE_CAP_POINTER))
    pointer_.reset(ei_device_ref(device));
  if (!abs_pointer_ && ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
    abs_pointer_.reset(ei_device_ref(device));
    if (ei_region* r = ei_device_get_region(device, 0)) {
      abs_region_w_ = ei_region_get_width(r);
      abs_region_h_ = ei_region_get_height(r);
    }
  }
  if (!keyboard_ && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD))
    keyboard_.reset(ei_device_ref(device));
}

void EisInjector::drop_device(ei_device* device) {
  if (pointer_ && pointer_.get() == device)
    pointer_.reset();
  if (abs_pointer_ && abs_pointer_.get() == device) {
    abs_pointer_.reset();
    abs_region_w_ = abs_region_h_ = 0;
  }
  if (keyboard_ && keyboard_.get() == device)
    keyboard_.reset();
}

void EisInjector::close() {
  if (bus_ && cookie_) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (cookie_type_ == 'u')
      sd_bus_call_method(bus_.get(), kService, kPath, kInterface, "disconnect", &err, nullptr, "u", cookie_);
    else
      sd_bus_call_method(
          bus_.get(),
          kService,
          kPath,
          kInterface,
          "disconnect",
          &err,
          nullptr,
          "i",
          static_cast<int32_t>(cookie_));
  }
  pointer_.reset();
  abs_pointer_.reset();
  keyboard_.reset();
  ei_.reset();
  bus_.reset();
}

// ---- evdev translation ----

bool forward(Source& src, EisSink& sink, const std::vector<std::uint32_t>& swallow_keys) {
  bool pointer = false, abs_pointer = false, keyboard = false;
  double rel_dx = 0.0, rel_dy = 0.0;
  auto flush_motion = [&] {
    if (rel_dx != 0.0 || rel_dy != 0.0) {
      sink.motion_rel(rel_dx, rel_dy);
      rel_dx = rel_dy = 0.0;
    }
  };
  std::array<input_event, 64> ev;
  for (;;) {
    ssize_t n = ::read(src.fd.get(), ev.data(), ev.size() * sizeof(input_event));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return false;  // device gone
    }
    if (n == 0)
      break;
    for (std::size_t i = 0, count = static_cast<std::size_t>(n) / sizeof(input_event); i < count; ++i) {
      const auto& e = ev[i];
      if (e.type != EV_REL || (e.code != REL_X && e.code != REL_Y))
        flush_motion();  // keeps every other event in its original position relative to the motion it followed
      if (e.type == EV_SYN) {
        if (e.code == SYN_REPORT) {
          sink.commit(pointer, abs_pointer, keyboard);
          pointer = abs_pointer = keyboard = false;
        }
        continue;
      }
      if (e.type == EV_KEY) {
        if (e.value == 2)
          continue;  // autorepeat; KWin generates its own
        // Route by source role, not key-code threshold: multimedia KEY_* codes (≥ 0x160) on the keyboard device would otherwise misroute to button().
        if (src.role == Role::keyboard) {
          if (std::find(swallow_keys.begin(), swallow_keys.end(), e.code) != swallow_keys.end())
            continue;
          sink.key(e.code, e.value != 0);
          keyboard = true;
        } else {
          sink.button(e.code, e.value != 0);
          pointer = true;
        }
      } else if (e.type == EV_REL) {
        switch (e.code) {
          case REL_X:
            rel_dx += e.value;
            pointer = true;
            break;
          case REL_Y:
            rel_dy += e.value;
            pointer = true;
            break;
          case REL_WHEEL_HI_RES:
            sink.scroll(
                0,
                -e.value);  // evdev +up → libei −y; hi-res is already in libei's 1/120-detent units, so no scaling
            pointer = true;
            break;
          case REL_HWHEEL_HI_RES:
            sink.scroll(e.value, 0);
            pointer = true;
            break;
          case REL_WHEEL:
          case REL_HWHEEL:
            break;  // ignored: inputtino emits these as the /120-truncated echo of the *_HI_RES channel, so honouring both double-counts and quantises touchpad scroll to whole detents
          default:
            break;
        }
      } else if (e.type == EV_ABS && src.has_abs) {
        if (e.code == ABS_X) {
          src.last_x = e.value;
          sink.motion_abs(
              src.last_x / static_cast<double>(src.abs_max_x),
              src.last_y / static_cast<double>(src.abs_max_y));
          abs_pointer = true;
        } else if (e.code == ABS_Y) {
          src.last_y = e.value;
          sink.motion_abs(
              src.last_x / static_cast<double>(src.abs_max_x),
              src.last_y / static_cast<double>(src.abs_max_y));
          abs_pointer = true;
        }
      }
    }
  }
  flush_motion();
  sink.commit(pointer, abs_pointer, keyboard);  // trailing events not terminated by SYN_REPORT
  return true;
}

// ---- one-shot key-chord injection ----

void tap_send(EisSink& sink, const std::vector<std::uint32_t>& codes, unsigned pace_us) {
  for (auto code : codes) {
    sink.key(code, true);
    sink.commit(false, false, true);
    if (pace_us)
      ::usleep(pace_us);
  }
  for (auto it = codes.rbegin(); it != codes.rend(); ++it) {
    sink.key(*it, false);
    sink.commit(false, false, true);
    if (pace_us)
      ::usleep(pace_us);
  }
}

int tap(const std::vector<std::uint32_t>& codes) {
  EisInjector eis;
  eis.connect();
  tap_send(eis, codes, 20'000);
  // The D-Bus disconnect in close() races frames still buffered in the EIS socket; give KWin a beat to drain them first.
  ::usleep(150'000);
  return 0;
}

// ---- Sunshine evdev source discovery + main loop ----

namespace {

// Newest device path per role (higher event number ≈ newer generation). Older devices from prior
// connects stay registered but go silent; only the newest is forwarded.
std::unordered_map<Role, std::string> discover() {
  static const std::unordered_map<std::string, Role> role_by_name = {
      {"Keyboard passthrough", Role::keyboard},
      {"Mouse passthrough (absolute)", Role::absolute},
      {"Mouse passthrough", Role::relative},
  };
  std::unordered_map<Role, std::pair<int, std::string>> best;
  DIR* dir = opendir("/dev/input");
  if (!dir)
    return {};
  while (struct dirent* ent = readdir(dir)) {
    const char* name = ent->d_name;
    if (std::strncmp(name, "event", 5) != 0 || name[5] == '\0')
      continue;
    bool digits = true;
    for (const char* p = name + 5; *p; ++p)
      if (!std::isdigit(static_cast<unsigned char>(*p))) {
        digits = false;
        break;
      }
    if (!digits)
      continue;

    std::string path = std::string("/dev/input/") + name;
    unique_fd dev(::open(path.c_str(), O_RDONLY));
    if (!dev)
      continue;
    char buf[256] = {};
    if (ioctl(dev.get(), EVIOCGNAME(sizeof(buf) - 1), buf) < 0)
      continue;
    auto it = role_by_name.find(buf);
    if (it == role_by_name.end())
      continue;
    int rank = std::atoi(name + 5);
    auto bi = best.find(it->second);
    if (bi == best.end() || rank > bi->second.first)
      best[it->second] = {rank, path};
  }
  closedir(dir);

  std::unordered_map<Role, std::string> out;
  for (auto& [role, ranked] : best) out[role] = std::move(ranked.second);
  return out;
}

void open_source(int epfd, std::unordered_map<Role, Source>& sources, Role role, const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "kwin-input-bridge: open %s failed: %s\n", path.c_str(), errstr().c_str());
    return;
  }
  auto [it, inserted] = sources.try_emplace(role);
  if (!inserted) {
    ::close(fd);
    return;
  }  // lost a race with another rescan
  Source& src = it->second;
  src.fd = unique_fd(fd);
  src.role = role;
  src.path = path;
  if (role == Role::absolute) {
    input_absinfo ax{}, ay{};
    // inputtino leaves the abs mouse at 0..0 unless used; only scale when the range is real.
    if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 && ax.maximum > 0 &&
        ay.maximum > 0) {
      src.has_abs = true;
      src.abs_max_x = ax.maximum;
      src.abs_max_y = ay.maximum;
      src.last_x = ax.value;
      src.last_y = ay.value;
    }
  }
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.ptr = &src;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) < 0) {
    std::fprintf(stderr, "kwin-input-bridge: epoll_ctl %s failed: %s\n", path.c_str(), errstr().c_str());
    sources.erase(it);
    return;
  }
  std::fprintf(stderr, "kwin-input-bridge: forwarding %s (%s)\n", path.c_str(), role_name(role));
}

// Adopt the newest device per role. Cheap but not per-event (only on a timer) — per-event rescans
// would throttle motion.
void sync_sources(int epfd, std::unordered_map<Role, Source>& sources) {
  auto found = discover();
  for (auto it = sources.begin(); it != sources.end();) {
    auto fi = found.find(it->first);
    if (fi != found.end() && fi->second == it->second.path) {
      ++it;
      continue;
    }  // unchanged
    epoll_ctl(epfd, EPOLL_CTL_DEL, it->second.fd.get(), nullptr);
    it = sources.erase(it);
  }
  for (auto& [role, path] : found)
    if (!sources.count(role) || sources.at(role).path != path)
      open_source(epfd, sources, role, path);
}

// Runs one EIS connection's lifetime — returns when the server disconnects or the process is asked to stop (g_exit).
void event_loop(EisInjector& eis, const std::vector<std::uint32_t>& swallow_keys) {
  unique_fd epfd(epoll_create1(EPOLL_CLOEXEC));
  if (!epfd)
    throw std::runtime_error("epoll_create1: " + errstr());

  epoll_event ee{};
  ee.events = EPOLLIN;
  ee.data.ptr = nullptr;  // sentinel: the libei event channel
  if (epoll_ctl(epfd.get(), EPOLL_CTL_ADD, eis.fd(), &ee) < 0)
    throw std::runtime_error("epoll_ctl ei fd: " + errstr());

  std::unordered_map<Role, Source> sources;
  sync_sources(epfd.get(), sources);
  uint64_t last_scan = now_us();

  std::array<epoll_event, 16> events;
  for (;;) {
    int n = epoll_wait(epfd.get(), events.data(), events.size(), 1000);
    if (n < 0) {
      if (errno == EINTR) {
        if (g_exit)
          return;
        continue;
      }
      throw std::runtime_error("epoll_wait: " + errstr());
    }
    for (int i = 0; i < n; ++i) {
      if (events[i].data.ptr == nullptr) {
        if (eis.process_events())
          return;  // server gone → let run() reconnect
        continue;
      }
      auto* src = static_cast<Source*>(events[i].data.ptr);
      if (!forward(*src, eis, swallow_keys)) {
        epoll_ctl(epfd.get(), EPOLL_CTL_DEL, src->fd.get(), nullptr);
        sources.erase(src->role);  // src is now invalid; do not touch after
      }
    }
    uint64_t now = now_us();
    if (now - last_scan >= 2'000'000) {
      sync_sources(epfd.get(), sources);  // after processing events: avoids erasing a Source still in events[]
      last_scan = now;
    }
    if (g_exit)
      return;
  }
}

}  // namespace

void run(const std::vector<std::uint32_t>& swallow_keys) {
  // Best-effort jitter hardening: lock pages and raise priority. Both can fail unprivileged.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    std::fprintf(stderr, "kwin-input-bridge: mlockall failed (%s); continuing\n", errstr().c_str());
  if (setpriority(PRIO_PROCESS, 0, -5) != 0)
    std::fprintf(stderr, "kwin-input-bridge: setpriority failed (%s); continuing\n", errstr().c_str());

  std::signal(SIGTERM, on_term);
  std::signal(SIGINT, on_term);
  std::signal(SIGPIPE, SIG_IGN);

  for (;;) {  // reconnect on EIS disconnect
    EisInjector eis;
    for (;;) {  // KWin's EIS interface appears once the compositor is up
      if (g_exit)
        return;
      try {
        eis.connect();
        break;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "kwin-input-bridge: waiting for KWin EIS: %s\n", e.what());
        ::sleep(1);
      }
    }
    std::fprintf(stderr, "kwin-input-bridge: connected to KWin EIS\n");
    event_loop(eis, swallow_keys);  // runs until the connection ends or we're asked to stop
    if (g_exit)
      return;  // SIGTERM
    std::fprintf(stderr, "kwin-input-bridge: EIS disconnected, reconnecting\n");
  }
}

}  // namespace kib
