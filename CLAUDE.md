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
   The only function pointers are ones third-party APIs demand (`glfwSetErrorCallback`). Third-party
   libraries may have their own virtual internals (ImGuiFileDialog does); first-party code only calls
   into them.
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
arena, and the two must differ. Image sizes come from user files, so running out of either arena is
a `Status`, not an assert. `image::probe` reads a file's header and estimates the scratch a decode
needs (`Info::scratch_bytes`, checked against stb's real peak in `//core:image_test`). The stb
allocator refuses rather than asserts when scratch runs out. The one-arena `image::load(path, arena,
out)` reserves the pixels, then lends the rest of the arena to the decoder.

Atlas loading (`editor::load_atlases`) probes every atlas and reserves its pixels in the asset arena
first. It then decodes, in parallel, the atlases whose scratch fits side by side in the job's general
arena, each in its own sub-arena. Larger ones decode one at a time with the whole general arena.

**Bitsery** (binary) writes into arena-backed vectors and reads in place from the arena the file was
read into.

Known heap users, which are deliberately out of scope: Dear ImGui's and ImGuiFileDialog's internal
allocations, GL/GLFW, `std::thread` bookkeeping, and `std::filesystem` in the file dialog.

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
| `//editor` | `undo`, `jobs` (background jobs), `style` (ImGui style profiles <-> JSON), `editor` (UI: `app.cpp` menus/jobs/dialogs/demo level, `viewport.cpp` canvas and tools, `panels.cpp`, `generate.cpp` the Generate panel's tabs (heightmap, water-shedding, WFC, WFC rules), `platform.cpp` window/GL/ImGui, `file_dialog.cpp` file dialogs, `deletion.cpp` delete buttons, `web.cpp` browser file upload/download), `config` (`config_path`: per-user settings files), `lvl` binary, `web` (wasm) |
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

### File dialogs

`editor/src/file_dialog.cpp` wraps aiekick/ImGuiFileDialog (features chosen with defines in
`//third_party:imgui_file_dialog.BUILD`). Each `FileAction` gets a title, file-type filters and, for
saves, a suggested name and overwrite confirmation. The places pane has three groups:
- **Places** (fixed, rebuilt on each open): Home (or `/work` in the browser), the working directory,
  the open level's folder and the settings folder.
- **Bookmarks**: user-editable, serialized to `config_path("file_dialog_places.txt")`.
- **Devices**.

The dialog remembers the last folder it was used in.

### Quitting

The main loop runs until `App::exit`. Every way of quitting goes through `request_exit()`: the
window's close button (the GLFW close flag is taken back with `cancel_close()` each frame), File >
Quit, and `--frames`/`--smoke`, which force it. With unsaved changes (`App::modified`) it shows the
"Unsaved changes" prompt instead: Save (`save_and_exit()`: exit once the save job succeeds; with no
path yet, via the save dialog, and cancelling that dialog cancels quitting), Quit without saving,
or Cancel. In the browser, `web::set_unsaved_changes()` arms the page's `beforeunload` "leave site?"
prompt while there are unsaved changes.

### Deleting

Rows of layers, semantic types, materials, seasons, path graphs, portals/markers and tile properties
start with a delete button (`delete_button`). `request_delete` records a `PendingDelete`, and
`process_deletes` (once per frame, after every panel has drawn and never while a job runs) shows a
confirmation dialog listing what else changes, then applies it; Shift+click skips the dialog.
Deleting a catalog entry (`level::remove_semantic`/`remove_material`) turns everything that used it
into "none" and renumbers later ids.

### Undo

`UndoStack` snapshots whole layer planes into the undo ring before an edit. Undo swaps the snapshot
with the layer, so the entry becomes its own redo. Object (entity) edits, layer add/remove/reorder and
resizes are not undoable; structural changes (including deleting a layer or a catalog entry) clear the
history.

## Build

Bazel 8 (`.bazelversion`; use bazelisk) with Bzlmod (`MODULE.bazel`). `.bazelrc` sets C++20 and the
`asan`, `ubsan`, `tsan`, `release` and `ci` configs; `user.bazelrc` is imported if present (ignored by
git). Third-party libraries:

- From BCR: `nlohmann_json` (patched via `single_version_override`), `googletest`, `emsdk`.
- Pinned `http_archive`s with BUILD files in `//third_party`: Dear ImGui (docking), ImGuiFileDialog,
  GLFW, EnTT, Bitsery, stb.

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
- `//editor:jobs_test` (atlas loading: a large atlas with small worker arenas; too little job or asset
  memory reported per atlas), `//editor:undo_test`, `//editor:style_test` (style profile round trip, partial and bad files).
- `//editor:up_down_test` (tag `display`): `lvl --smoke` drives a scripted session through the UI code
  paths: demo level, generators, undo/redo, save/load in both formats, all exports, a style profile
  save/load, deletes, the file dialog, and the unsaved-changes prompt (cancelled, then "Save and
  quit" ends the session). It requires no
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
- ImGuiFileDialog uses `try`/`catch`. Natively, third-party code keeps the toolchain's exceptions. On
  Emscripten (whose toolchain disables exceptions), that library and `//editor:lvl` request the emsdk
  toolchain's `wasm_exceptions` feature; native toolchains ignore it.
- `docs/editor.png` (README) is `lvl --demo --frames=40 --screenshot=docs/editor.png` under
  `xvfb-run -s "-screen 0 1600x900x24"`, with `HOME` pointed at an empty directory (so no personal
  style profile applies) and no `imgui.ini` in the working directory. Regenerate it when the UI
  changes noticeably.
