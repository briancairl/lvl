load("@rules_cc//cc:cc_library.bzl", "cc_library")

# aiekick/ImGuiFileDialog: file browser with places/bookmarks, a clickable path bar, search, sorting,
# keyboard exploration and overwrite confirmation. Features are switched on with defines (they change
# struct layouts, so they are `defines`, seen by every user of the header).
cc_library(
    name = "imgui_file_dialog",
    srcs = ["ImGuiFileDialog.cpp"],
    hdrs = [
        "ImGuiFileDialog.h",
        "ImGuiFileDialogConfig.h",
    ],
    copts = ["-w"],
    defines = [
        "USE_STD_FILESYSTEM",
        "USE_PLACES_FEATURE",
        "USE_PLACES_BOOKMARKS",
        "USE_PLACES_DEVICES",
        "PLACES_PANE_DEFAULT_SHOWN=true",
        "USE_EXPLORATION_BY_KEYS",
        "USE_DIALOG_EXIT_WITH_KEY",
        "IGFD_EXIT_KEY=ImGuiKey_Escape",
        "USE_QUICK_PATH_SELECT",
    ],
    includes = ["."],
    # The editor's own "Places" group (fixed places) goes first, before the built-in Bookmarks
    local_defines = ["placesBookmarksDisplayOrder=1"],
    # It uses try/catch (regex filters, std::filesystem errors), and the Emscripten toolchain builds
    # with -fno-exceptions by default: this one library gets WebAssembly-native exceptions there
    # (//editor:lvl links with the same feature). Native toolchains ignore the feature; first-party
    # code stays -fno-exceptions everywhere.
    features = ["wasm_exceptions"],
    visibility = ["//visibility:public"],
    deps = ["@imgui"],
)
