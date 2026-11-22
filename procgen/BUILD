load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# Procedural generation: noise heightmaps, iterative water-shedding, wave function collapse,
# auto-tiling.
cc_library(
    name = "procgen",
    srcs = [
        "src/autotile.cpp",
        "src/fill.cpp",
        "src/noise.cpp",
        "src/steps.cpp",
        "src/watershed.cpp",
        "src/wfc.cpp",
    ],
    hdrs = [
        "include/lvl/procgen/autotile.hpp",
        "include/lvl/procgen/fill.hpp",
        "include/lvl/procgen/noise.hpp",
        "include/lvl/procgen/steps.hpp",
        "include/lvl/procgen/watershed.hpp",
        "include/lvl/procgen/wfc.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:image",
        "//core:jobs",
        "//core:json",
        "//core:memory",
        "//level:model",
    ],
)

cc_test(
    name = "procgen_test",
    size = "small",
    srcs = ["test/procgen_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":procgen",
        "@googletest//:gtest_main",
    ],
)
