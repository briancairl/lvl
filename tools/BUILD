load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

# Headless conversion (JSON <-> binary), inspection and PNG/mini-preview export of levels.
cc_binary(
    name = "lvl_convert",
    srcs = ["lvl_convert.cpp"],
    copts = LVL_COPTS,
    deps = [
        "//core:image",
        "//core:jobs",
        "//core:json",
        "//core:logging",
        "//core:memory",
        "//level:io",
        "//render",
    ],
)

sh_test(
    name = "convert_test",
    size = "small",
    srcs = ["convert_test.sh"],
    args = [
        "$(rootpath :lvl_convert)",
        "$(rootpath testdata/small.json)",
    ],
    data = [
        "testdata/small.json",
        ":lvl_convert",
    ],
)
