load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# Header-only: the STB_*_IMPLEMENTATION translation unit lives in //core (core/src/image.cpp).
cc_library(name = "image", hdrs = ["stb_image.h"], includes = ["."])
cc_library(name = "image_write", hdrs = ["stb_image_write.h"], includes = ["."])
cc_library(name = "image_resize", hdrs = ["stb_image_resize2.h"], includes = ["."])
