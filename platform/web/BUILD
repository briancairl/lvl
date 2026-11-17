load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("//bazel:copts.bzl", "LVL_COPTS")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "hello",
    srcs = ["hello.cpp"],
    copts = LVL_COPTS,
    deps = ["//core:build"],
)

# `bazel build //platform/web:hello_web` -- toolchain smoke test (run with node).
wasm_cc_binary(
    name = "hello_web",
    cc_target = ":hello",
    outputs = [
        "hello_web/hello.js",
        "hello_web/hello.wasm",
    ],
    tags = ["manual"],
)
