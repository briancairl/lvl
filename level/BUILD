load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# The level document model: grid layers, tilesets, catalogs, seasons, WFC and auto-tiling rules,
# objects (EnTT).
cc_library(
    name = "model",
    srcs = [
        "src/auto_tile_rules.cpp",
        "src/step_rules.cpp",
        "src/layer.cpp",
        "src/level.cpp",
        "src/objects.cpp",
        "src/tileset.cpp",
        "src/types.cpp",
        "src/wfc_rules.cpp",
    ],
    hdrs = [
        "include/lvl/level/auto_tile_rules.hpp",
        "include/lvl/level/step_rules.hpp",
        "include/lvl/level/grid.hpp",
        "include/lvl/level/layer.hpp",
        "include/lvl/level/level.hpp",
        "include/lvl/level/objects.hpp",
        "include/lvl/level/tileset.hpp",
        "include/lvl/level/types.hpp",
        "include/lvl/level/wfc_rules.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:image",
        "//core:json",
        "//core:logging",
        "//core:memory",
        "//core:types",
        "@entt",
    ],
)

# Level/tileset/WFC/path files, JSON and binary, with parallel layer encode/decode.
cc_library(
    name = "io",
    srcs = [
        "src/io.cpp",
        "src/io_binary.cpp",
        "src/io_common.hpp",
        "src/io_json.cpp",
    ],
    hdrs = ["include/lvl/level/io.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        ":model",
        "//core:binary",
        "//core:build",
        "//core:file",
        "//core:jobs",
        "//core:json",
        "//core:types",
        "//project:model",
        "//project:paths",
    ],
)

cc_test(
    name = "model_test",
    size = "small",
    srcs = ["test/model_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":model",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "io_test",
    size = "small",
    srcs = ["test/io_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":io",
        ":model",
        "//core:file",
        "//project:model",
        "//project:paths",
        "@googletest//:gtest_main",
    ],
)
