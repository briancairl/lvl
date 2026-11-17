load("@rules_cc//cc:cc_library.bzl", "cc_library")

# GLFW 3.4 built from source for the Linux X11 backend: the shared sources, the posix_* sources
# shared across backends, and the X11 source list from upstream's src/CMakeLists.txt. Wayland is not
# built (it needs wayland-scanner protocol codegen). X11/GL headers and libraries come from the host
# (libx11-dev, libxrandr-dev, libxinerama-dev, libxcursor-dev, libxi-dev, libxext-dev, libgl-dev).
cc_library(
    name = "glfw",
    srcs = [
        "src/context.c",
        "src/egl_context.c",
        "src/glx_context.c",
        "src/init.c",
        "src/input.c",
        "src/linux_joystick.c",
        "src/monitor.c",
        "src/null_init.c",
        "src/null_joystick.c",
        "src/null_monitor.c",
        "src/null_window.c",
        "src/osmesa_context.c",
        "src/platform.c",
        "src/posix_module.c",
        "src/posix_poll.c",
        "src/posix_thread.c",
        "src/posix_time.c",
        "src/vulkan.c",
        "src/window.c",
        "src/x11_init.c",
        "src/x11_monitor.c",
        "src/x11_window.c",
        "src/xkb_unicode.c",
    ],
    hdrs = glob([
        "include/GLFW/*.h",
        "src/*.h",
    ]),
    copts = ["-w"],
    includes = ["include"],
    local_defines = ["_GLFW_X11"],
    linkopts = [
        "-lX11",
        "-lXrandr",
        "-lXinerama",
        "-lXcursor",
        "-lXi",
        "-lXext",
        "-ldl",
        "-lpthread",
    ],
    visibility = ["//visibility:public"],
)
