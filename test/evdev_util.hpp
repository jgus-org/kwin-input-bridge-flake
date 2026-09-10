// Shared test helpers: synthetic evdev event builders and a printf-style formatter for building comparable event records.

#pragma once

#include <linux/input-event-codes.h>
#include <linux/input.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace kibtest {

inline input_event mk(std::uint16_t type, std::uint16_t code, std::int32_t value) {
  input_event e{};
  e.type = type;
  e.code = code;
  e.value = value;
  return e;
}

inline input_event syn() { return mk(EV_SYN, SYN_REPORT, 0); }

template<class... A> std::string fmt(const char* f, A... a) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), f, a...);
  return std::string(buf);
}

}  // namespace kibtest
