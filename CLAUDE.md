# lvl

A 2D tile-based game level editor, written in C++20 and built with Bazel. The UI is Dear ImGui:
GLFW + OpenGL 3.3 natively, and Emscripten's GLFW port + WebGL2 in the browser. Level objects live in
an EnTT registry. This file gives context to anyone, human or AI, working on the code. Read it before
making architectural decisions.

## Core tenets

These follow the conventions of `briancairl/gft`, the game these levels are made for. New code must
keep them.

1. **Memory is reserved up front, in arenas.** The editor makes one large allocation (`--memory`,
   default 1 GiB natively / 512 MiB on the web) and carves fixed sub-arenas out of it. Exhausting an
   arena is an assert failure, never a silent heap fallback; operations sized by user input (exports,
   file reads) check first and return a `Status`.
2. **Human-readable formats are the source of truth.** Everything round-trips through JSON. Binary
   is always a *translation* of the same data, and `//tools:lvl_convert` converts either way (the
   JSON → binary → JSON round trip is byte-for-byte stable and tested).
3. **No virtual dispatch.** No `virtual`/`override`/`std::function` in first-party code. Closed sets
   of shapes use `std::variant` + `lvl::overloaded{...}`, a closed `enum` + `switch`, or templates.
   The only function pointers are ones third-party APIs demand (`glfwSetErrorCallback`).
4. **No exceptions.** First-party targets build with `-fno-exceptions` (`LVL_COPTS`,
   `//bazel:copts.bzl`). Invariant violations use `LVL_ASSERT*`/`LVL_FAIL`, which log the condition,
   the operand values and an explanation, then `std::abort()` (they are not compiled out). Failures
   caused by the user, such as missing files, malformed JSON or images that won't fit, return a
   `lvl::Status`, which is logged and shown in an error popup.
5. **Data-oriented.** Layers are flat planes (`Grid<T>`). Algorithms (noise, watershed, WFC,
   rendering, I/O) are free functions over plain data, and editor systems are free functions over
   `App`.

## How the tenets are upheld

### Memory

`lvl::memory::Arena` (`core/include/lvl/core/memory.hpp`) is gft's bump allocator, with these
additions:

- `push`/`pop` carve 64-byte-aligned sub-arenas.
- `Scope` marks an arena and rewinds it on destruction.
- The newest block can grow or be released in place (`try_resize_bytes`/`release_bytes`).
- `ArenaAllocator<T>` is a stateful std allocator; `memory::Vector<T>` is a `std::vector` that uses
  it.
- Peak usage and a label are tracked for diagnostics.

The editor's layout (`editor/src/app.cpp`, `reserve_memory`):

| arena | use |
|---|---|
| `documents[2]` | the open `level::Level` and a spare. Loads go into the spare and are swapped in on success; `level::reset()` rewinds a document wholesale |
| `assets[2]` | decoded atlas images and average tile colors. Reloads decode into the other arena, then swap |
| `undo` | the undo ring (`editor::UndoStack`) |
| `frame` | per-frame scratch, cleared every frame (overlay rebuilds, file listings, mini-preview) |
| `json` | the UI thread's JSON arena |
| `jobs` | split per job by `editor::partition()` into one general arena plus a JSON and a scratch arena per worker; cleared after the job's result is applied |

The EnTT registry uses `ArenaAllocator` (`level::Registry`). Names and paths are inline
`FixedString`s (`Name`, `Path`), so documents never touch the heap.

**JSON** works as in gft. `lvl::Json` is an `nlohmann::basic_json` whose allocator draws from the
thread's installed JSON arena, inside a `json::Scope`. Two patches in `//third_party/patches` route
nlohmann's parser/serializer buffers and its value-destruction stack through that allocator. The
first patch is gft's; the second is new, because without it destroying nested values allocated on
the heap. `//core:json_memory_test` checks with a counting `operator new` that parse/edit/dump makes
zero heap allocations. The state is thread-local, so every I/O worker installs its own arena
(`level::io::with_json`).

**Images**: `core/src/image.cpp` compiles stb_image, stb_image_write and stb_image_resize2 with
`STBI_MALLOC` & co. routed into a thread-local scratch arena. Results are copied to a destination
arena, and the two must differ.

**Bitsery** (binary) writes into arena-backed vectors and reads in place from the arena the file was
read into.

Known heap users, which are deliberately out of scope: Dear ImGui's internal allocations, GL/GLFW,
`std::thread` bookkeeping, and `std::filesystem` in the file dialog.

### Formats

- JSON loaders go through `json::Reader`. It type- and range-checks every value and records the first
  failure with the offending key, instead of letting nlohmann abort under `-fno-exceptions`.
- Floats are written as their shortest round-trip decimal (`json::shortest`).
- Each binary file starts with a header: magic `LVLB`, a kind, and the editor version
  (`core/include/lvl/core/binary.hpp`).

## Layout

| package | contents |
|---|---|
| `//core` | `logging` (`LVL_LOG_*`, `LVL_ASSERT*`, thread-safe line buffering, log history for the UI), `types` (`FixedString`, `overloaded`), `memory`, `file` (atomic `file::Writer`), `json`, `binary`, `jobs` (`parallel_for`, `Background<Job>`, `Progress`, seeded `random`), `image` |
| `//level` | `model`: `Level`, `Grid<T>`, `Layer` (`LayerKind` → plane type), tilesets (slicing, `TileInfo`, properties, `SeasonalVariant`), catalogs and seasons, `WfcRules`, EnTT objects (portals, markers, path nodes/edges). `io`: level/tileset/WFC/path files in JSON and binary |
| `//procgen` | `noise` (heightmaps), `watershed` (erosion, flow routing, rivers/lakes/tunnels), `wfc` (solver) |
| `//render` | `preview` (`colorize`: a layer as one RGBA pixel per cell, used by both the editor's overlays and exports), `compose` (full map PNG, mini-preview) |
| `//editor` | `undo`, `jobs` (background jobs), `editor` (UI: `app.cpp` menus/jobs/dialogs/demo level, `viewport.cpp` canvas and tools, `panels.cpp`, `generate.cpp` generators and the WFC rule editor, `platform.cpp` window/GL/ImGui, `web.cpp` browser file upload/download), `lvl` binary, `web` (wasm) |
| `//tools` | `lvl_convert` |
| `//platform/web` | HTML shell, browser smoke test |
| `//third_party` | BUILD files and patches for `http_archive` dependencies |

### Level I/O and parallelism

`level::save_level`/`load_level` take an `IoContext`: workers, each with a JSON and a scratch arena,
plus an optional `Progress`.

- **JSON**: layers are written last, one object per line, with their data arrays written as text
  directly. On load, a scanner finds each layer's span. The rest of the document is parsed on the
  calling thread (with `"layers": []`), while workers parse the layers into their scratch arenas.
  Results are then copied into the document on the calling thread, because document arenas are
  single-threaded.
- **Binary**: a Bitsery metadata block, then raw per-layer chunks with FNV-1a checksums
  computed/verified in parallel.

Catalogs, seasons and tilesets are referenced by name in JSON. Standalone tileset/WFC/path files carry
the catalog entries they use and are remapped by name on load.

### Editor jobs

Long operations are concrete job types (`editor/include/lvl/editor/jobs.hpp`) with `run(Progress&)`,
held in `JobSlot` (a `std::variant` of `jobs::Background<...>`). `prepare_job<T>()` partitions the
jobs arena; `start_job()` launches the job. While it runs, a modal progress dialog with Cancel blocks
editing, so jobs may read the document without locks. `finish_job()` applies results on the UI thread
(with undo snapshots). Generators copy inputs into the job's arena and the UI thread copies results
back. On the web there are no pthreads, so jobs run synchronously.

### Undo

`UndoStack` snapshots whole layer planes into the undo ring before an edit. Undo swaps the snapshot
with the layer, so the entry becomes its own redo. Object (entity) edits, layer add/remove/reorder and
resizes are not undoable; structural changes clear the history.

## Build

Bazel 8 (`.bazelversion`; use bazelisk) with Bzlmod (`MODULE.bazel`). `.bazelrc` sets C++20 and the
`asan`, `ubsan`, `tsan`, `release` and `ci` configs; `user.bazelrc` is imported if present (ignored by
git). Third-party libraries:

- From BCR: `nlohmann_json` (patched via `single_version_override`), `googletest`, `emsdk`.
- Pinned `http_archive`s with BUILD files in `//third_party`: Dear ImGui (docking), GLFW, EnTT,
  Bitsery, stb.

Third-party headers are system includes, so their warnings stay out of our builds.

- `bazel test //...` builds and runs everything except the wasm target.
- `bazel build //editor:web` builds the web version (tagged `manual`).

CI (`.github/workflows/ci.yml`, ubuntu-24.04) runs these jobs; `CI passed` aggregates them and is the
one check to mark required:

- **Native build & test** under `default`/`asan`/`ubsan`/`tsan`. Display tests run under the host's
  Xvfb.
- **clang-format** (version 18).
- **Web build & browser test**: builds `//editor:web` and runs `platform/web/smoke_test.mjs` in
  headless Chromium; the build and a screenshot are uploaded.

## Tests

- `//core`: logging (incl. no interleaving across threads, death tests on `cond:`), memory, JSON/binary
  serialization and file errors, JSON zero-heap check, jobs/random, images.
- `//level`: model operations (resize, seasons, derivation, tileset removal, portals/paths/routes, WFC
  rule learning); full JSON and binary round trips with 4 workers and with 1, JSON→binary→JSON
  stability, standalone files moving between levels, malformed/truncated files.
- `//procgen`: noise determinism, parallel == serial heightmaps, seed images, erosion,
  rivers/lakes, breach tunnels, WFC with adjacency, layer constraints, kept tiles/regions, impossible
  constraints.
- `//render`: tile colors, compose with seasons and overlays, size refusal, mini-preview, PNG export.
- `//editor:undo_test`.
- `//editor:up_down_test` (tag `display`): `lvl --smoke` drives a scripted session through the UI code
  paths: demo level, generators, undo/redo, save/load in both formats, all exports. It requires no
  errors logged; the final frame and exports are kept as test outputs.
- `//tools:convert_test`.

## Style

`.clang-format` (2-space indent, 120 columns, braces on their own line for functions, classes and
namespaces) is enforced by pre-commit and CI. Types are `PascalCase`, constants and enum values
`kPascalCase`, fields `snake_case`, macros `LVL_SCREAMING_SNAKE_CASE`. Namespaces mirror directories
(`lvl::core`, `lvl::level`, `lvl::procgen`, `lvl::render`, `lvl::editor`). Every source file starts
with the `@copyright`/`@file` header, and includes are grouped as `// C++ Standard Library`, then
third-party, then `// LVL`. Enums that are (de)serialized specialize `lvl::EnumNames<E>`.

## Gotchas

- `memory::Arena` capacities multiply out of 32 bits on wasm32: divide before multiplying when
  splitting sizes (`total / 100 * 18`).
- Never keep a `lvl::Json` past its `json::Scope`; deallocation asserts that a scope is open.
- `binary::read_file`'s scratch arena is rewound on return, so values must not allocate from it.
- `image::load`/`decode`/`resize` need distinct destination and scratch arenas.
- Mutating the document from a job is not allowed: copy inputs into the job's arena, and apply
  results in `finish_job()`.
