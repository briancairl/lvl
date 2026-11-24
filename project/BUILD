load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# A project's folder tree: where levels, tilesets, rule sets, atlases and exports belong, and how a
# file chosen from outside is copied in so the project stays self-contained.
cc_library(
    name = "paths",
    srcs = ["src/paths.cpp"],
    hdrs = ["include/lvl/project/paths.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:file",
        "//core:logging",
        "//core:memory",
        "//core:types",
    ],
)

# The project model: the data its levels share, and the index that lets a portal link to a level
# without loading it.
cc_library(
    name = "model",
    srcs = ["src/project.cpp"],
    hdrs = ["include/lvl/project/project.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        ":paths",
        "//core:file",
        "//core:logging",
        "//core:memory",
        "//core:types",
        "//level:model",
    ],
)

# The project manifest (project.json).
cc_library(
    name = "io",
    srcs = ["src/io.cpp"],
    hdrs = ["include/lvl/project/io.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        ":model",
        ":paths",
        "//core:build",
        "//core:file",
        "//core:json",
        "//core:logging",
        "//core:types",
        "//level:io",
    ],
)

cc_test(
    name = "project_test",
    size = "small",
    srcs = ["test/project_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":io",
        ":model",
        ":paths",
        "@googletest//:gtest_main",
    ],
)
