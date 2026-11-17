load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "build",
    hdrs = ["include/lvl/core/build.hpp"],
    strip_include_prefix = "include",
)

cc_library(
    name = "types",
    hdrs = [
        "include/lvl/core/fixed_string.hpp",
        "include/lvl/core/visitor.hpp",
    ],
    strip_include_prefix = "include",
)

cc_library(
    name = "logging",
    srcs = ["src/logging.cpp"],
    hdrs = [
        "include/lvl/core/format.hpp",
        "include/lvl/core/logging.hpp",
        "include/lvl/core/status.hpp",
    ],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [":types"],
)

cc_test(
    name = "build_test",
    size = "small",
    srcs = ["test/build_test.cpp"],
    copts = LVL_COPTS,
    deps = [":build", "@googletest//:gtest_main"],
)

cc_test(
    name = "logging_test",
    size = "small",
    srcs = ["test/logging_test.cpp"],
    copts = LVL_COPTS,
    deps = [":logging", ":types", "@googletest//:gtest_main"],
)

cc_library(
    name = "memory",
    srcs = ["src/memory.cpp"],
    hdrs = ["include/lvl/core/memory.hpp"],
    copts = LVL_COPTS,
    strip_include_prefix = "include",
    deps = [":logging"],
)

cc_test(
    name = "memory_test",
    size = "small",
    srcs = ["test/memory_test.cpp"],
    copts = LVL_COPTS,
    deps = [":memory", "@googletest//:gtest_main"],
)
