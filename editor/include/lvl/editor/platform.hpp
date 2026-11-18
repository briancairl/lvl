/**
 * @copyright 2026-present Brian Cairl
 *
 * @file platform.hpp
 *
 * Window, OpenGL context and Dear ImGui setup: GLFW + OpenGL 3.3 natively, Emscripten's GLFW port +
 * WebGL2 in the browser. Also owns GPU textures (atlases, layer overlays).
 */
#pragma once

// C++ Standard Library
#include <cstdint>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl::editor
{

/// A GPU texture
struct Texture
{
  std::uint32_t id = 0;
  int width = 0;
  int height = 0;

  [[nodiscard]] bool valid() const { return id != 0; }
};

struct PlatformConfig
{
  const char* title = "lvl";
  int width = 1600;
  int height = 900;
  /// Hidden window (headless runs under a virtual X server)
  bool hidden = false;
};

/// Creates the window, GL context and ImGui context
Status initialize(const PlatformConfig& config);

/// Destroys everything created by <code>initialize</code>
void shutdown();

/// True once the user asked to close the window
[[nodiscard]] bool should_close();

/// Requests the window to close
void request_close();

/// Withdraws a close request (e.g. the user cancelled at the unsaved-changes prompt)
void cancel_close();

/// Polls input and starts an ImGui frame
void begin_frame();

/// Renders the ImGui frame and presents it
void end_frame();

/// Creates (or re-creates, when sizes differ) a texture and uploads <code>img</code>
void upload(Texture& texture, const image::Image& img, bool smooth = false);

/// Frees a texture
void release(Texture& texture);

/// Reads back the last presented frame into <code>out</code> (allocated from <code>arena</code>)
void read_framebuffer(memory::Arena& arena, image::Image& out);

/// Sets the window title
void set_title(const char* title);

}  // namespace lvl::editor
