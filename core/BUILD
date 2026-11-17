load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "build",
    hdrs = ["include/lvl/core/build.hpp"],
    strip_include_prefix = "include",
)

cc_test(
    name = "build_test",
    size = "small",
    srcs = ["test/build_test.cpp"],
    copts = LVL_COPTS,
    deps = [":build", "@googletest//:gtest_main"],
)
