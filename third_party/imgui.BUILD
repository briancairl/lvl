load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Dear ImGui core.
cc_library(
    name = "imgui",
    srcs = [
        "imgui.cpp",
        "imgui_demo.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
    ],
    hdrs = [
        "imconfig.h",
        "imgui.h",
        "imgui_internal.h",
        "imstb_rectpack.h",
        "imstb_textedit.h",
        "imstb_truetype.h",
    ],
    copts = ["-w"],
    includes = ["."],
)

# ImGui::InputText overloads for std::string.
cc_library(
    name = "stdlib",
    srcs = ["misc/cpp/imgui_stdlib.cpp"],
    copts = ["-w"],
    hdrs = ["misc/cpp/imgui_stdlib.h"],
    strip_include_prefix = "misc/cpp",
    deps = [":imgui"],
)

# GLFW + OpenGL3 platform/renderer backends. Natively these use @glfw and imgui's own GL loader;
# under Emscripten, GLFW is Emscripten's built-in port and GL is WebGL2 (GLES3).
cc_library(
    name = "backend_glfw_opengl3",
    srcs = [
        "backends/imgui_impl_glfw.cpp",
        "backends/imgui_impl_opengl3.cpp",
    ],
    hdrs = [
        "backends/imgui_impl_glfw.h",
        "backends/imgui_impl_opengl3.h",
        "backends/imgui_impl_opengl3_loader.h",
    ],
    copts = ["-w"],
    includes = ["backends"],
    linkopts = select({
        "@platforms//os:emscripten": [
            "-sUSE_GLFW=3",
            "-sMIN_WEBGL_VERSION=2",
            "-sMAX_WEBGL_VERSION=2",
        ],
        "//conditions:default": [
            "-lGL",
            "-ldl",
        ],
    }),
    deps = [":imgui"] + select({
        "@platforms//os:emscripten": [],
        "//conditions:default": ["@glfw"],
    }),
)
