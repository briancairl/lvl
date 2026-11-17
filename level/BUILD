load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# The level document model: grid layers, tilesets, catalogs, seasons, WFC rules, objects (EnTT).
cc_library(
    name = "model",
    srcs = [
        "src/layer.cpp",
        "src/level.cpp",
        "src/objects.cpp",
        "src/tileset.cpp",
        "src/types.cpp",
        "src/wfc_rules.cpp",
    ],
    hdrs = [
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
