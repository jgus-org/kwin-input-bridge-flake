#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "kwin-input-bridge.hpp"

int main(int argc, char** argv) {
  setvbuf(stderr, nullptr, _IOLBF, 0);
  try {
    if (argc == 3 && std::strcmp(argv[1], "--tap") == 0) {
      // Chord spec: evdev key codes joined with '+', e.g. "29+46" for ctrl+c. A code must follow every '+', so the empty spec and trailing '+' are rejected.
      std::vector<std::uint32_t> codes;
      for (const char* p = argv[2];;) {
        char* end = nullptr;
        unsigned long code = std::strtoul(p, &end, 10);
        if (end == p || code == 0 || code > KEY_MAX || (*end && *end != '+')) {
          std::fprintf(stderr, "kwin-input-bridge: bad chord spec: %s\n", argv[2]);
          return 2;
        }
        codes.push_back(static_cast<std::uint32_t>(code));
        if (!*end)
          break;
        p = end + 1;
      }
      return kib::tap(codes);
    }
    std::vector<std::uint32_t> swallow_keys;
    for (int i = 1; i < argc; i += 2) {
      bool ok = std::strcmp(argv[i], "--swallow-key") == 0 && i + 1 < argc;
      char* end = nullptr;
      unsigned long code = ok ? std::strtoul(argv[i + 1], &end, 10) : 0;
      if (!ok || end == argv[i + 1] || *end || code == 0 || code > KEY_MAX) {
        std::fprintf(stderr, "usage: kwin-input-bridge [--tap CODE[+CODE...]] [--swallow-key CODE]...\n");
        return 2;
      }
      swallow_keys.push_back(static_cast<std::uint32_t>(code));
    }
    kib::run(swallow_keys);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "kwin-input-bridge: %s\n", e.what());
    return 1;
  }
  return 0;
}
