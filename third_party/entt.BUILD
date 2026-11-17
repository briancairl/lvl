load("@rules_cc//cc:cc_library.bzl", "cc_library")

# EnTT ships its own src/BUILD.bazel (making src/ a separate package), so use the amalgamated header.
cc_library(
    name = "entt",
    hdrs = ["single_include/entt/entt.hpp"],
    # -isystem: EnTT's own warnings are not ours to fix
    includes = ["single_include"],
    visibility = ["//visibility:public"],
)
