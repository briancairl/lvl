load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# CPU rendering: layer previews (shared by the editor's overlays), full-map PNG export, mini-preview.
cc_library(
    name = "render",
    srcs = [
        "src/compose.cpp",
        "src/preview.cpp",
    ],
    hdrs = [
        "include/lvl/render/compose.hpp",
        "include/lvl/render/preview.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [
        "//core:image",
        "//core:jobs",
        "//core:memory",
        "//level:model",
    ],
)

cc_test(
    name = "render_test",
    size = "small",
    srcs = ["test/render_test.cpp"],
    copts = LVL_COPTS,
    deps = [
        ":render",
        "@googletest//:gtest_main",
    ],
)
