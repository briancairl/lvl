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
| `jobs` | split per job by `editor::partition()` into one general arena plus a JSON and a scratch arena per worker; cleared after the job's result is applied. The general arena doubles as level I/O's shared arena (`IoContext::shared`), which is safe because a job's I/O and its other uses of that arena are separate phases |

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
| `//level` | `model`: `Level`, `Grid<T>`, `Layer` (`LayerKind` → plane type), tilesets (slicing, `TileInfo`, properties, `SeasonalVariant`), catalogs and seasons, `WfcRules` (side adjacency, plus corner adjacency when `diagonals` is set), `AutoTileRules` (terrains keyed on a neighbor pattern), EnTT objects (portals, markers, path nodes/edges). `io`: level/tileset/rules/path files in JSON and binary |
| `//procgen` | `noise` (heightmaps), `watershed` (erosion, flow routing, rivers/lakes/tunnels), `wfc` (solver), `autotile` (`paint_terrain`: a terrain and its border; `autotile`: the WFC-scored fallback), `fill` (`fill_masked`: a whole-map fill masked by the other layers) |
| `//render` | `preview` (`colorize`: a layer as one RGBA pixel per cell, used by both the editor's overlays and exports), `compose` (full map PNG, mini-preview) |
| `//editor` | `undo`, `jobs` (background jobs), `style` (ImGui style profiles <-> JSON), `editor` (UI: `app.cpp` menus/jobs/dialogs/demo level, `viewport.cpp` canvas and tools, `panels.cpp`, `generate.cpp` the Generate panel's tabs (heightmap, water-shedding, WFC, fill), `adjacency.cpp` the Tile adjacency rules window, `neighbors.cpp` the pop-out drag-and-drop neighbor editor, `platform.cpp` window/GL/ImGui, `file_dialog.cpp` file dialogs, `recent.cpp` the recent levels, `deletion.cpp` delete buttons, `web.cpp` browser file upload/download), `config` (`config_path`/`write_config`: per-user settings files), `lvl` binary, `web` (wasm) |
| `//tools` | `lvl_convert` |
| `//platform/web` | HTML shell, browser smoke test |
| `//third_party` | BUILD files and patches for `http_archive` dependencies |

### Level I/O and parallelism

`level::save_level`/`load_level` take an `IoContext`: workers, each with a JSON and a scratch arena,
an optional `Progress`, and an optional `shared` arena.

Buffers whose size follows the level's — a whole file's bytes, a layer's encoded text — come from
`shared`, not from a worker's arenas, because a worker's share of memory is `jobs / 2 / workers` and
has nothing to do with how big a level is. Only the calling thread carves `shared` up, so bump
allocation stays single-threaded. Without one, worker 0's scratch is used, which caps a level at one
worker's share.

- **JSON, saving**: layers are written last, one object per line, with their data arrays written as
  text directly. The document is written first, then the layers a *wave* at a time: the calling
  thread carves one sub-arena per layer of the wave out of `shared` (as many of the next layers as
  fit, at least one), workers encode into them in parallel, the wave is written in order and the
  memory reclaimed — so peak use is one wave's text, not the whole level's. `encoded_bound` sizes
  each buffer; it must never be short, since the buffer is its sub-arena's whole capacity and so
  cannot grow. A layer whose text will not fit on its own is encoded straight to the file through a
  `StreamSink`'s small fixed buffer instead (serially, since the writer is), so **no level is too
  big to save**; `encode_layer` is a template over the two sinks and writes the same bytes to either.
- **JSON, loading**: a scanner finds each layer's span, and the rest of the document is parsed on the
  calling thread (with `"layers": []`). Then the layer headers are read, also on the calling thread,
  because `add_layer` allocates the plane from the document arena, which is single-threaded. The
  workers then scan the cells straight into those planes — no staging copy, so nothing a worker holds
  is sized by the level. A layer's *data array* never goes through the DOM, as it is never written
  through one: a DOM of the cells costs several times what the cells do. Only the header is parsed,
  from the object with `"data": []` in place of the cells.
- **Binary**: a Bitsery metadata block, then raw per-layer chunks with FNV-1a checksums
  computed/verified in parallel. Loading reads the chunks in place, so the whole file stays resident
  in `shared`.

The one buffer still sized by the level is the file's own text when loading, which must fit `shared`.
Lifting that means a scanner that works from disk plus ranged reads, so for the very largest levels
JSON load, not save, is the wall.

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

Imports and exports of the open level's data (tilesets, WFC rules, path graphs, atlas and seed
images, the PNG exports) open in the **current project folder**: the folder the open level lives in,
or the working directory while it has no path yet. Choosing a file elsewhere in one of them does not
move the remembered folder. Level open/save and style profiles keep using the last folder the dialog
was used in.

### Recent levels

File > Open recent lists the last `RecentFiles::kCapacity` (10) levels opened or saved, newest
first. `editor/src/recent.cpp` keeps the list in `App::recent` (fixed `Path`s, so no heap) and
writes it to `config_path("recent_files.txt")` as one path per line whenever it changes; it is read
back the first time the menu is drawn or an entry is recorded. `finish_job` records a path once a
level load or save succeeds, and drops one whose load failed, so a file that has gone away leaves
the list when it is next tried; until then the menu shows it disabled. "Clear the list" empties it.
Choosing an entry replaces the open document just as "Open..." does.

### Quitting

The main loop runs until `App::exit`. Every way of quitting goes through `request_exit()`: the
window's close button (the GLFW close flag is taken back with `cancel_close()` each frame), File >
Quit, and `--frames`/`--smoke`, which force it. With unsaved changes (`App::modified`) it shows the
"Unsaved changes" prompt instead: Save (`save_and_exit()`: exit once the save job succeeds; with no
path yet, via the save dialog, and cancelling that dialog cancels quitting), Quit without saving,
or Cancel. In the browser, `web::set_unsaved_changes()` arms the page's `beforeunload` "leave site?"
prompt while there are unsaved changes.

### Palette selection and batch tile edits

A plain click or drag in the tileset palette picks one tile or a rectangle of them (the paint stamp,
`App::paint`) as before. Ctrl+click toggles tiles and shift+click extends from the last one clicked,
which builds `App::selection` (a `TileSelection`: one bit per tile of one tileset, plus the anchor).
`editor/src/selection.cpp` reads it: `selected_tiles` (the indices, in an arena), `selection_bounds`
(the palette rectangle they span) and `palette_layout` (the selection as a `Grid<TileRef>` laid out
as in the palette). Every batch context draws the same `deselect_all_button` (the palette header,
Batch edit, and both WFC sections), and Edit > Deselect all tiles / Ctrl+Shift+A empties it from
anywhere.

With two or more tiles selected, the paint tool writes them all at once, in the relative positions
the palette gives them (a tree of 3x3 tiles stays a tree), so the selection is both what a batch
edit applies to and what painting stamps. `paint_stamp` builds that stamp — the selection's layout
when two or more tiles are selected, otherwise `App::paint`'s one tile or palette rectangle — and
`stamp_extents` is its footprint, for the cursor's hover preview. A cell the stamp leaves empty (a
hole in the selection) keeps what is under it. The stamp's top-left corner sits at the cursor, it is
built once per stroke application in the frame arena, and the brush radius applies only when the
stamp is a single tile. The eyedropper empties the selection, so the tile it picks is what paints.

With two or more tiles selected, "Batch edit" at the bottom of the Tile metadata panel writes
metadata to all of them at once: each field (semantic type, material, hidden, collision, a
key/value property to set or a key to remove) is behind a checkbox saying whether it is applied, and
"Take from tile N" fills the fields from the tile shown above. `level::apply_edit(tileset, TileEdit,
tiles, count)` does the writing; like the single-tile fields, it is not undoable.

### Two kinds of tile adjacency rules

A level holds two, and the "Tile adjacency rules" window has a tab for each:

- **WFC** (`level::WfcRules`): a permission relation on pairs, per direction -- what *may* sit next
  to what. The WFC generator solves it over a region.
- **Auto-tiling** (`level::AutoTileRules`, `level/auto_tile_rules.hpp`): terrains, each keying its
  tiles on the pattern of like neighbors around a cell.

They are not interchangeable, and the second exists because the first cannot prescribe inner and
outer corners. A WFC cell's legal tiles are the conjunction of independent per-direction
constraints, while picking a corner needs a predicate over the *joint* pattern of the eight
neighbors; in a real tileset the north-east and north-west outer corners and the north edge permit
the same tiles on every side, so no set of `allowed(a, b, d)` facts separates them. WFC is also
permissive and weight-random, while auto-tiling must be a function: one pattern, one tile.

A terrain's `NeighborMask` has one bit per `Direction`, set where the neighbor holds the same
terrain (Wang/blob tiling). `Neighborhood::kSides` counts the four sides (16 patterns, which a
nine-slice covers); `kBlob` also counts a corner wherever both of its sides are terrain (47
patterns), which is what tells inner from outer corners.

Several tiles may share a pattern: they are its *pool*, and a cell picks one by a hash of its
position, so a terrain's middle can be a handful of tiles that scatter over an area and stay put
when it is repainted (`resolve(terrain, mask, key)`, `add_variant`). Where a terrain has no tile for
a pattern, `resolve` falls back to the nearest one it does have, ranking **the four sides above the
corners** — a side counts for more than every corner together, since the sides are what place a
tile. Without that a thirteen-tile set answers an edge with a tile meant for another edge that
happens to share corners, which is the usual way auto-tiling looks subtly wrong.

`learn_from_layout` reads one palette selection: a 3x3 nine-slice, a 2x2 inner-corner block among
the tiles it did not take, and anything left over beside them as further middles — so the whole
thirteen-tile set is one selection (the blocks side by side), and the blocks can still be taken one
at a time. A layout of a single tile is the middle. `expand` is how far past what the user paints
the terrain reaches: 1 (the default) means painting a cell lays its middle tile and borders the ring
around it; 0 means the painted cells take the edge tiles themselves.

Terrains follow tileset removal, and travel in the level files and in the standalone rules file
(which carries both kinds), remapped by tileset name.

### Auto-tiled painting

The Auto tool (toolbar, or T) paints a tile as the middle of a shape and tiles the border around it.
Which rules it uses depends on the tile being painted, and the toolbar says which:

- the tile belongs to a **terrain** (`terrain_of`): `procgen::paint_terrain`. The painted cells,
  plus the cells already holding one of the terrain's fully-surrounded tiles (its every tile when
  `expand` is 0), are the *core*; the terrain covers the core grown by `expand`, and every covered cell near
  the stroke takes the tile for the pattern of covered neighbors around it. So one painted cell lays
  down a whole nine-slice, and painting the next cell of a path turns the old corner into a straight
  edge -- the covered area follows from the core alone, so a stroke never has to remember what the
  last one did. Cells off the grid count as covered, so a terrain running off the map gets no border
  along the map's edge.
- **no terrain**: `procgen::autotile`, the fallback. It scores each WFC rule tile against its
  neighbors' adjacencies over a band of `App::auto_tile.radius` cells (the "Refill" slider), locking
  the painted cells and every cell holding a tile they hold, counting a locked neighbor
  `locked_weight` times an ordinary one and keeping what is there on a tie, over at most
  `iterations` nearest-first sweeps. It tidies a band against hard constraints but cannot prescribe
  corners, so the toolbar points at the Auto-tiling tab.

Both are local, greedy and deterministic rather than solvers: they never fail and are cheap enough
to run on every mouse-move of a stroke. Where the rules cannot fill a cell they say so (the status
line) rather than backtracking. Whole regions are still generated with `procgen::solve`.

### Masked fills

The Generate panel's **Fill** tab writes tiles over the whole map, choosing the cells from the
layers the level already has rather than from a brush stroke: a band of a scalar layer (a heightmap,
or water depth) and a ground type on a semantic layer, intersected, optionally narrowed to the
Select tool's region and to the cells that have no tile yet. `procgen::fill_masked`
(`procgen/fill.hpp`) does the writing; `editor::apply_fill` builds the mask from `App::generators`,
takes the undo snapshot and is what the Fill button calls.

The tiles it writes are the palette selection, the same set the paint tool stamps. With one tile
every matched cell takes it; with several it **scatters** them, each cell picking one by a hash of
its position and the level's seed. So the scatter is a function of the cell, not of the order cells
are visited: filling twice writes the same tiles, narrowing the mask and filling again leaves the
cells it still covers exactly as they were, and only the seed moves them. "Scatter the selection"
off fills with the first selected tile instead, which is the plain whole-map fill.

A fill is instant even on a large map, so it is not a job; it is one undoable layer edit.

### Tile adjacency rules

Both kinds of rules live in one window, `adjacency_window` (View > Tile adjacency rules,
`editor/src/adjacency.cpp`), above a tab bar; the rule set's name and the import/export of the file
that carries both are shared, and `App::rules_mode` is the tab (it is the source of truth, so
another panel or the scripted test can switch tabs).

The **WFC** tab holds "Add palette tile(s)", the rules from the palette layout, the rule-tile strip,
the selected tile's constraints, the batch constraints and the allowed neighbors per direction. The
generators read `level::WfcRules`, so this is not a tab of the Generate panel; the Generate panel's
WFC tab shows how many tiles and adjacencies it holds and has an "Edit rules..." button that opens
and focuses the window.

The **Auto-tiling** tab holds the terrain list (each row with a `delete_button`), the selected
terrain's name, neighborhood and border, "From the palette layout", and a slot per pattern the
neighborhood distinguishes. A slot draws the tile it holds with a line along every side the terrain
does *not* continue through and a notch at every corner it does not reach, so the slot looks like
the situation it covers; clicking one puts the palette tile in it, ctrl+clicking adds another tile
to that pattern's pool (the slot then shows "xN"), right-clicking clears it, and a slot with no tile
is dark (`resolve` will use the nearest pattern's tile).

Under "Allowed neighbors", clicking a thumbnail toggles that neighbor; pressing the left button and
dragging across several sets all of them to what the first one became (allowed if it was forbidden,
and the other way round), so a whole row can be swept in one motion. The drag is kept in one
direction's row and ends when the button comes up.

### Batch WFC constraints

A rule tile counts as selected when its tile is in the palette selection; ctrl+clicking the rule
tile strip in the adjacency rules window toggles it there too (`selected_rule_tiles`). With two or more
selected, "Batch constraints" writes the ticked fields (weight, height band, water rule, semantic
type) to all of them through `level::apply_edit(WfcRules&, WfcTileEdit, ...)`, the counterpart of
the tile-metadata batch edit; `batch_field` draws one row of either.

### WFC rules from the palette layout

"From the palette layout" in the adjacency rules window turns the palette selection into adjacency rules:
every pair of selected tiles that sit next to each other in the palette may sit next to each other
on the map, in that direction (corners too when `WfcRules::diagonals` is on). "Allow what it shows"
merges those pairs into the rules; "Only what it shows"
(`level::LayoutMode::kExclusive`) makes them a hard constraint, forbidding every other tile on the
sides where the layout puts a neighbor — sides the layout says nothing about, and tiles it does not
contain, keep the rules they have. `level::constrain_from_layout` does both; its scratch arena must
not be the one the rules live in. "Add palette tile(s)" adds the selection, or the paint stamp when
nothing is selected.

### Neighbor editor

`neighbors_window` (View > Tile neighbors, or "Neighbor editor..." in the adjacency rules window) is a
floating window: a palette of the rule tiles, and a 3x3 grid with the edited tile in the middle and one
drop slot per `Direction`. Drag a tile from the palette into a slot to allow it there, between slots to
move it (Ctrl: copy), onto the palette or right-click it to forbid it, onto the middle to edit it next.
Each slot's context menu allows/clears everything or copies it to the other sides/corners. Corner slots
only count when `WfcRules::diagonals` is on; turning it on with no corner rules derives them from the
side rules (`level::derive_diagonals`), so an existing rule set stays solvable.

`Direction` has the four sides (values 0..3, also used by portals) then the four corners (4..7);
`kDirections` is the sides, `kDiagonals` the corners and `kNeighbors` all eight.

Wherever a neighbor is picked for a known direction — a tile in one of the slots, or one of the
thumbnails under "Allowed neighbors" in the adjacency rules window — hovering it shows `wfc_pair_tooltip`: the two
tiles drawn in the positions the rule would put them in (the edited one outlined yellow), and whether
the pair is allowed. The "Preview pairs" checkbox in either window turns it off (`App::wfc_preview`),
and the neighbor editor falls back to its plain text tooltip.

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
  rule learning, rules from a palette layout, batch tile and constraint edits, terrains from a
  nine-slice or an inner-corner block, the whole thirteen-tile set plus spare middles in one
  selection, the sides-first fallback, what each neighborhood distinguishes); full JSON and binary round trips with 4 workers and with 1, JSON→binary→JSON
  stability, standalone files moving between levels, malformed/truncated files, a level whose layers
  and file are bigger than one worker's share, saves split across waves and saves streamed layer by
  layer (both byte-identical to an unsplit one), a load with worker arenas far too small to stage a
  layer, headers at their widest.
- `//procgen`: masked fills (a height band, a ground type, the two together, the scatter being even
  and staying put under a different mask, regions and clipping, sparing what is drawn, no tiles);
  noise determinism, parallel == serial heightmaps, seed images, erosion,
  rivers/lakes, breach tunnels, WFC with adjacency, layer constraints, kept tiles/regions, impossible
  constraints; terrain painting (a nine-slice from one painted cell, the border re-tiling as the
  shape grows, inner corners, no border, the map edge, middles scattering over an area and staying
  put when it is repainted, a terrain with no tiles); the WFC-scored
  fallback (edges around painted centers, a path drawn one cell at a time, stability once the rules
  hold, corner fills with `diagonals`, tiles the rules do not know, rules that cannot express the
  shape).
- `//render`: tile colors, compose with seasons and overlays, size refusal, mini-preview, PNG export.
- `//editor:jobs_test` (atlas loading: a large atlas with small worker arenas; too little job or asset
  memory reported per atlas; a level saved and loaded through `JobMemory` whose layers exceed a
  worker's scratch), `//editor:undo_test`, `//editor:style_test` (style profile round trip, partial and bad files).
- `//editor:up_down_test` (tag `display`): `lvl --smoke` drives a scripted session through the UI code
  paths: demo level, generators (incl. WFC with corner rules, the adjacency rules window and the neighbor
  editor), undo/redo, save/load
  in both formats, all exports, a style profile save/load, deletes, a batch tile edit, adjacency rules and
  batch constraints from a palette selection, the file dialog, reopening the newest recent level, a
  multi-tile paint stamp, a thirteen-tile terrain taken from one palette selection and painted (the
  auto-tiling tab), a masked fill (scattered over a height band, stable when re-run, and with the
  scatter off),
  and the unsaved-changes prompt (cancelled, then "Save and quit" ends the session). It requires no
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
