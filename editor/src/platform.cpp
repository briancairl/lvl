/**
 * @copyright 2026-present Brian Cairl
 *
 * @file platform.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <utility>

// Dear ImGui
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// OpenGL
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#else
#include <imgui_impl_opengl3_loader.h>
#endif  // defined(__EMSCRIPTEN__)

// Enums the imgui GL loader doesn't declare (values from the OpenGL spec)
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#endif
#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT 0x0CF5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif

// GLFW
#include <GLFW/glfw3.h>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/editor/platform.hpp"

namespace lvl::editor
{
namespace
{

GLFWwindow* window = nullptr;

void on_glfw_error(int error, const char* description)
{
  LVL_LOG_ERROR() << "GLFW error " << error << ": " << description;
}

}  // namespace

Status initialize(const PlatformConfig& config)
{
  glfwSetErrorCallback(on_glfw_error);
  if (glfwInit() == GLFW_FALSE)
  {
    return Status::error("failed to initialize GLFW (is a display available?)");
  }
#if defined(__EMSCRIPTEN__)
  const char* glsl_version = "#version 300 es";
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
  const char* glsl_version = "#version 330";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif  // defined(__EMSCRIPTEN__)
  if (config.hidden)
  {
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  }
  window = glfwCreateWindow(config.width, config.height, config.title, nullptr, nullptr);
  if (window == nullptr)
  {
    glfwTerminate();
    return Status::error("failed to create a window with an OpenGL 3.3 / WebGL2 context");
  }
  glfwMakeContextCurrent(window);
#if !defined(__EMSCRIPTEN__)
  // The browser paces frames itself (requestAnimationFrame)
  glfwSwapInterval(1);
#endif  // !defined(__EMSCRIPTEN__)

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // The style profile is applied by the editor (see lvl/editor/style.hpp)
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(__EMSCRIPTEN__)
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
  // No persistent layout file in the browser
  io.IniFilename = nullptr;
#endif  // defined(__EMSCRIPTEN__)
  ImGui_ImplOpenGL3_Init(glsl_version);
  LVL_LOG_INFO() << "Platform initialized (ImGui " << ImGui::GetVersion() << ")";
  return Status::okay();
}

void shutdown()
{
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  window = nullptr;
}

bool should_close() { return glfwWindowShouldClose(window) != 0; }

void request_close() { glfwSetWindowShouldClose(window, GLFW_TRUE); }

void begin_frame()
{
  glfwPollEvents();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void end_frame()
{
  ImGui::Render();
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  glViewport(0, 0, width, height);
  glClearColor(0.10F, 0.10F, 0.11F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window);
}

void upload(Texture& texture, const image::Image& img, bool smooth)
{
  if (img.empty())
  {
    release(texture);
    return;
  }
  if (texture.id == 0)
  {
    GLuint id = 0;
    glGenTextures(1, &id);
    texture.id = id;
  }
  glBindTexture(GL_TEXTURE_2D, texture.id);
  const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.pixels);
  texture.width = img.width;
  texture.height = img.height;
}

void release(Texture& texture)
{
  if (texture.id != 0)
  {
    const GLuint id = texture.id;
    glDeleteTextures(1, &id);
  }
  texture = Texture{};
}

void read_framebuffer(memory::Arena& arena, image::Image& out)
{
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  out = image::create(arena, std::max(width, 1), std::max(height, 1));
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, out.width, out.height, GL_RGBA, GL_UNSIGNED_BYTE, out.pixels);
  // GL rows are bottom-up
  for (int y = 0; y < out.height / 2; ++y)
  {
    for (int x = 0; x < out.width; ++x)
    {
      std::swap(out.at(x, y), out.at(x, out.height - 1 - y));
    }
  }
}

void set_title(const char* title) { glfwSetWindowTitle(window, title); }

}  // namespace lvl::editor
