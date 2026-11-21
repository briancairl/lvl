load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# Undo/redo by layer snapshots (UI-independent, unit tested).
cc_library(
    name = "undo",
    srcs = ["src/undo.cpp"],
    hdrs = ["include/lvl/editor/undo.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:logging",
        "//core:memory",
        "//level:model",
    ],
)

# UI style profiles (ImGuiStyle <-> JSON), and where per-user settings files live.
cc_library(
    name = "style",
    srcs = [
        "src/config.cpp",
        "src/style.cpp",
    ],
    hdrs = [
        "include/lvl/editor/config.hpp",
        "include/lvl/editor/style.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:build",
        "//core:file",
        "//core:json",
        "//core:types",
        "@imgui",
    ],
)

# Background jobs (UI-independent).
cc_library(
    name = "jobs",
    srcs = ["src/jobs.cpp"],
    hdrs = ["include/lvl/editor/jobs.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:image",
        "//core:jobs",
        "//level:io",
        "//level:model",
        "//procgen",
        "//render",
    ],
)

# The editor UI (Dear ImGui on GLFW + OpenGL3 / WebGL2).
cc_library(
    name = "editor",
    srcs = [
        "src/app.cpp",
        "src/deletion.cpp",
        "src/file_dialog.cpp",
        "src/generate.cpp",
        "src/internal.hpp",
        "src/panels.cpp",
        "src/platform.cpp",
        "src/selection.cpp",
        "src/wfc_neighbors.cpp",
        "src/viewport.cpp",
        "src/web.cpp",
    ],
    hdrs = [
        "include/lvl/editor/app.hpp",
        "include/lvl/editor/platform.hpp",
        "include/lvl/editor/web.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        ":jobs",
        ":style",
        ":undo",
        "//core:json",
        "//core:logging",
        "//core:types",
        "//level:io",
        "//level:model",
        "//procgen",
        "//render",
        "@imgui",
        "@imgui//:backend_glfw_opengl3",
        "@imgui_file_dialog",
    ],
)

cc_binary(
    name = "lvl",
    srcs = ["main.cpp"],
    copts = LVL_COPTS,
    # Web: link with WebAssembly exceptions for ImGuiFileDialog (see //third_party:imgui_file_dialog.BUILD)
    features = ["wasm_exceptions"],
    linkopts = select({
        "@platforms//os:emscripten": [
            "--oformat=html",
            "--shell-file=$(location //platform/web:shell.html)",
            "-sALLOW_MEMORY_GROWTH=1",
            "-sMAXIMUM_MEMORY=4GB",
            "-sINITIAL_MEMORY=64MB",
            "-sSTACK_SIZE=4MB",
            "-sEXPORTED_RUNTIME_METHODS=FS",
            "-sFORCE_FILESYSTEM=1",
        ],
        "//conditions:default": [],
    }),
    additional_linker_inputs = select({
        "@platforms//os:emscripten": ["//platform/web:shell.html"],
        "//conditions:default": [],
    }),
    deps = [
        ":editor",
        "//core:build",
        "//core:logging",
    ],
)

# `bazel build //editor:web` -> bazel-bin/editor/web/lvl.{html,js,wasm}
wasm_cc_binary(
    name = "web",
    cc_target = ":lvl",
    outputs = [
        "web/lvl.html",
        "web/lvl.js",
        "web/lvl.wasm",
    ],
    tags = ["manual"],
)

cc_test(
    name = "jobs_test",
    size = "small",
    srcs = ["test/jobs_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":jobs",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "style_test",
    size = "small",
    srcs = ["test/style_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":style",
        "//core:file",
        "//core:memory",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "undo_test",
    size = "small",
    srcs = ["test/undo_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":undo",
        "@googletest//:gtest_main",
    ],
)

# Boots the editor (under a virtual X server if there is no display), builds the demo level
# (heightmap + WFC + atlas reload job) and exits after a few hundred frames.
sh_test(
    name = "up_down_test",
    size = "medium",
    srcs = ["test/up_down_test.sh"],
    args = [
        "$(rootpath :lvl)",
        "$(rootpath test/tsan.supp)",
    ],
    data = [
        "test/tsan.supp",
        ":lvl",
    ],
    tags = ["display"],
)
