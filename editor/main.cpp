/**
 * @copyright 2026-present Brian Cairl
 *
 * @file main.cpp
 *
 * lvl editor entry point.
 *
 *   lvl [level.json|level.lvlb] [--memory=MiB] [--frames=N] [--hidden] [--demo] [--smoke]
 *       [--screenshot=out.png]
 *
 * --frames=N exits after N frames (headless up/down tests); --hidden keeps the window invisible;
 * --smoke runs a scripted session (demo level, generators, undo, save/load, exports) and exits.
 */

// C++ Standard Library
#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif  // defined(__EMSCRIPTEN__)

// LVL
#include "lvl/core/build.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/editor/app.hpp"
#include "lvl/editor/platform.hpp"

namespace
{

/// The editor state (large; static storage rather than the stack)
lvl::editor::App app;

struct Options
{
  const char* path = nullptr;
  std::size_t memory_mib = lvl::kIsWeb ? 512 : 1024;
  std::size_t frames = 0;
  bool hidden = false;
  bool demo = false;
  bool smoke = false;
  /// Saves the final frame as PNG before exiting (with --frames)
  const char* screenshot = nullptr;
};

Options parse(int argc, char** argv)
{
  Options o;
  for (int i = 1; i < argc; ++i)
  {
    const char* arg = argv[i];
    if (std::strncmp(arg, "--memory=", 9) == 0)
    {
      o.memory_mib = static_cast<std::size_t>(std::strtoull(arg + 9, nullptr, 10));
    }
    else if (std::strncmp(arg, "--frames=", 9) == 0)
    {
      o.frames = static_cast<std::size_t>(std::strtoull(arg + 9, nullptr, 10));
    }
    else if (std::strncmp(arg, "--screenshot=", 13) == 0)
    {
      o.screenshot = arg + 13;
    }
    else if (std::strcmp(arg, "--hidden") == 0)
    {
      o.hidden = true;
    }
    else if (std::strcmp(arg, "--smoke") == 0)
    {
      o.smoke = true;
    }
    else if (std::strcmp(arg, "--demo") == 0)
    {
      o.demo = true;
    }
    else if (arg[0] != '-')
    {
      o.path = arg;
    }
    else
    {
      LVL_LOG_WARN() << "Ignoring unknown option '" << arg << "'";
    }
  }
  return o;
}

void run_frame()
{
  lvl::editor::begin_frame();
  lvl::editor::frame(app);
  lvl::editor::end_frame();
}

}  // namespace

int main(int argc, char** argv)
{
  const Options options = parse(argc, argv);
  if (const auto status = lvl::editor::initialize(lvl::editor::PlatformConfig{.hidden = options.hidden}); !status.ok)
  {
    LVL_LOG_ERROR() << status.message;
    return EXIT_FAILURE;
  }
  app.max_frames = options.frames;
  app.smoke = options.smoke;
  const lvl::editor::MemoryConfig memory{.total = std::max<std::size_t>(options.memory_mib, 64) << 20};
  if (const auto status = lvl::editor::initialize(app, memory, options.path); !status.ok)
  {
    LVL_LOG_ERROR() << status.message;
    lvl::editor::shutdown();
    return EXIT_FAILURE;
  }
  if (options.demo)
  {
    lvl::editor::demo_level(app);
  }
#if defined(__EMSCRIPTEN__)
  // The browser drives frames; main() returns while the page keeps running
  emscripten_set_main_loop(run_frame, 0, false);
#else
  while (!app.exit)
  {
    run_frame();
  }
  if (options.screenshot != nullptr)
  {
    lvl::memory::Arena& scratch = app.memory.jobs;
    lvl::memory::clear(scratch);
    lvl::memory::Arena encode;
    lvl::image::Image img;
    lvl::editor::read_framebuffer(scratch, img);
    lvl::memory::push(scratch, encode, "screenshot");
    if (const auto status = lvl::image::save_png(options.screenshot, img, encode); !status.ok)
    {
      LVL_LOG_ERROR() << status.message;
    }
    lvl::memory::pop(scratch, encode);
  }
  lvl::editor::shutdown(app);
  lvl::editor::shutdown();
  LVL_LOG_INFO() << "Exited cleanly after " << app.frame_count << " frames";
#endif  // defined(__EMSCRIPTEN__)
  return EXIT_SUCCESS;
}
