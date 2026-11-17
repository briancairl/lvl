load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
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
