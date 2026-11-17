# lvl

A 2D tile-based game level editor, written in C++20 and built with Bazel. The UI is Dear ImGui
(GLFW + OpenGL3 natively, Emscripten's GLFW port + WebGL2 in the browser). Level objects live in an
EnTT registry. This file gives context to anyone, human or AI, working on the code. Read it before
making architectural decisions.

## Core tenets

These follow the conventions of `briancairl/gft` (the game this editor's levels are meant for).

1. **Memory is reserved up front, in arenas.** The editor makes one `std::malloc` for its whole
   working set (`lvl::memory::Arena`, sized from the command line / defaults). Subsystems carve
   fixed-capacity sub-arenas out of it. Level documents, tilesets, scratch space, JSON parsing and
   binary I/O buffers each get their own arena. Exhausting an arena is an assert failure, not a
   silent fallback to the heap.
2. **Human-readable formats are the source of truth.** Every asset (levels, tilesets, WFC rules,
   path graphs) round-trips through JSON. The compact binary format (Bitsery) is always a
   *translation* of the same data, and `//tools:lvl_convert` converts either way.
3. **No virtual dispatch.** No `virtual`/`override`/`std::function`. Closed sets of "shapes" are
   `std::variant` plus `lvl::overloaded{...}`, a closed `enum` plus `switch`, or templates.
4. **No exceptions.** First-party code is compiled with `-fno-exceptions` (`//bazel:copts.bzl`).
   Invariant violations go through `LVL_ASSERT*`/`LVL_FAIL`. These log the condition, the operand
   values and an explanation, then `std::abort()`. Recoverable failures, such as a missing or
   malformed file chosen by the user, return a `lvl::Result`/`bool` and are logged and shown in the
   UI.
5. **Data-oriented.** Grid layers are flat structure-of-arrays planes. Algorithms (noise,
   watershed, WFC, export) are free functions over those planes, not methods on smart objects.

## Style

`.clang-format` (2-space indent, 120 columns, braces on their own line for functions, classes and
namespaces) is enforced by pre-commit and by CI's `clang-format` job. Types are `PascalCase`,
constants and enum values `kPascalCase`, fields `snake_case`, macros `LVL_SCREAMING_SNAKE_CASE`.
Namespaces mirror directories (`lvl::core`, `lvl::level`, `lvl::procgen`, `lvl::editor`). Every
source file starts with the `@copyright`/`@file` header, and includes are grouped as
`// C++ Standard Library`, then third-party, then `// LVL`.

## Build

Bazel 8 (`.bazelversion`; use bazelisk) with Bzlmod (`MODULE.bazel`). `.bazelrc` sets C++20 and
provides the `asan`, `ubsan`, `tsan`, `release` and `ci` configs. Third-party libraries come from
BCR (`nlohmann_json`, `googletest`, `emsdk`) or from pinned `http_archive`s with BUILD files in
`//third_party` (`imgui`, `glfw`, `entt`, `bitsery`, `stb`).

- `bazel test //...` runs every test.
- The web build is a `wasm_cc_binary` (tagged `manual`) built by CI's `web` job.

CI (`.github/workflows/ci.yml`) runs: native build and test under `default`/`asan`/`ubsan`/`tsan`;
a `clang-format` check; and the web build. `CI passed` aggregates these and is the one check to mark
required.
