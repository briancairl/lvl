// Minimal WebAssembly smoke target: proves the Emscripten toolchain builds first-party code.
// C++ Standard Library
#include <cstdio>

// LVL
#include "lvl/core/build.hpp"

int main()
{
  std::printf("lvl %d.%d.%d (web=%d)\n", lvl::kVersionMajor, lvl::kVersionMinor, lvl::kVersionPatch, lvl::kIsWeb);
  return 0;
}
