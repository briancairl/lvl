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
| `projects[2]` | the `project::Project` the open level draws on (catalogs, seasons, tilesets, rule sets) and a spare. A load interns the file's entries into the spare, which is swapped in with the document on success |
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
- **That version is the (de)serializer's context** (`binary::Version`), so a `serialize` overload can
  ask `binary::has_version(s, 300)` whether the file it is reading has a field that was appended
  later. Binary files are positional, so appending a field to a type moves no bytes but is simply
  not there in an older file; without the version the read would run off the end. Writing is always
  the newest shape, since it writes `kVersion`. That is what keeps every file an earlier editor
  wrote readable, and `//core:serialization_test` checks it against bytes with an older header.

## Layout

| package | contents |
|---|---|
| `//core` | `logging` (`LVL_LOG_*`, `LVL_ASSERT*`, thread-safe line buffering, log history for the UI), `types` (`FixedString`, `overloaded`), `memory`, `file` (atomic `file::Writer`), `json`, `binary`, `jobs` (`parallel_for`, `Background<Job>`, `Progress`, seeded `random`), `image` |
| `//level` | `model`: `Level`, `Grid<T>`, `Layer` (`LayerKind` → plane type), tileset slicing and metadata (`TileInfo`, properties, `SeasonalVariant`, `TileGroup` multi-tile stamps), `WfcRules` (side adjacency, plus corner adjacency when `diagonals` is set), `AutoTileRules` (terrains keyed on a neighbor pattern), `StepRules` (terrains keyed on the difference in height level), EnTT objects (portals, markers, path nodes/edges). A `Level` is now its settings, layers, path graphs and objects: the tilesets its cells name, the catalogs they index, the seasons it follows and all three kinds of tile rules belong to its `//project`. `for_each_id_cell`/`count_id_cells` and the four `clear_tileset_refs` overloads (one per `Level`/`WfcRules`/`AutoTileRules`/`StepRules`) are the primitives the project drives them through, and `derive_collision`/`derive_materials` take the `TileSets` they read. `io`: level/tileset/rules/path files in JSON and binary |
| `//project` | `paths`: the folder tree a project owns (`Folder`, `subfolder`/`resolve`/`relativize`, `ensure_tree`, `plan_copy`/`apply_copy` for copying an asset in). `model`: `Project` (the tilesets its levels draw from, the `RuleSet`s they auto-tile with (`find_rules`/`add_rules`/`intern_rules`/`rules_of`/`remove_rules`), the seasons they follow and the semantic/material catalogs their cells index, the tileset/rule-set files it owns, and a `LevelEntry` index of its member levels), `portal_link`. `io`: the `project.json` manifest |
| `//procgen` | `noise` (heightmaps), `watershed` (erosion, flow routing, rivers/lakes/tunnels), `wfc` (solver), `autotile` (`paint_terrain`: a terrain and its border; `autotile`: the WFC-scored fallback), `fill` (`fill_masked`: a whole-map fill masked by the other layers), `steps` (`terrace`: a heightmap cut into whole levels; `tidy_levels`: squaring them into shapes the tiles can draw; `tile_steps`: the cliffs between them) |
| `//render` | `preview` (`colorize`: a layer as one RGBA pixel per cell, used by the editor's overlays, the layer export and the mini-preview), `compose` (full map PNG, mini-preview, `export_layer_png`) |
| `//editor` | `undo`, `jobs` (background jobs, incl. `RescanProjectJob` and `CompactCatalogsJob`), `style` (ImGui style profiles <-> JSON), `editor` (UI: `app.cpp` menus/jobs/dialogs/demo level, `viewport.cpp` canvas and tools, `panels.cpp`, `generate.cpp` the Generate panel's tabs (heightmap, water-shedding, WFC, fill, height steps), `panels.cpp` (including the Project panel: members, tilesets, rule sets), `adjacency.cpp` the Tile adjacency rules window, `neighbors.cpp` the pop-out drag-and-drop neighbor editor, `platform.cpp` window/GL/ImGui, `file_dialog.cpp` file dialogs, `recent.cpp` the recent levels, `deletion.cpp` delete buttons, `web.cpp` browser file upload/download), `config` (`config_path`/`write_config`: per-user settings files), `lvl` binary, `web` (wasm) |
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

**`level::LevelScope` says how much of what a level draws on goes into its own file.** A level
references its project's catalogs, seasons, tilesets and rule sets by id, so the file has to say
which of them it carries.

- `kStandalone` writes them all, so the file reads back on its own into a project it fills in as it
  goes. It is the default, it is what `//tools:lvl_convert` emits and what exporting a level writes,
  and it is byte-for-byte what a level file has always been -- a file handed to someone else should
  work.
- `kProject` writes only what the level owns: its settings, path graphs, objects and layers. The
  manifest carries the rest, so the file is smaller and a project's levels cannot drift apart.
  Loading one needs a project that already holds the shared data, and its cells are read as the
  project ids they are.

Loading accepts either: a file that carries the shared sections **replaces** the project's with them
(each section, not just some -- that is why the JSON loader clears the tilesets as the seasons and
the three rule kinds already did), and one that does not leaves the project untouched. In binary the
two are different file kinds, `binary::Kind::kLevel` and `kProjectLevel`, so a reader knows which
shape the metadata block has before decoding it; `Meta::shared` carries that and is itself never
serialized.

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

**Each action says which of the project's folders its files belong in** (`Setup::folder`), so the
table in `file_dialog.cpp` is where "where does this kind of file live?" is answered: a level opens
in `levels/`, an atlas in `assets/atlases/`, a seed image in `assets/images/`, a tileset in
`tilesets/`, rules in `rules/`, path graphs in `paths/`, every PNG export in `artifacts/`. The
folder is created on demand, so one the user deleted does not make the dialog open nowhere. Saving a
level offers `levels/` even when the open level's file is somewhere else, because a save into the
tree is what makes it a member; the name it already has is still what is suggested.

Only two actions belong to no folder of the open project: a **project manifest**, since putting one
somewhere is what *makes* a project there, so it cannot open inside one; and a **style profile**,
which is a per-user setting rather than part of any project. Those open where the dialog was last
used, and choosing a file elsewhere in one of the project actions does not move that remembered
folder. With no project folder yet every action falls back to the folder the open level lives in and
then to the working directory -- the behaviour before projects, and still what a bare level file
gets -- and every action updates the remembered folder again, since they all share it.

### Projects

A project is a **folder** with a fixed sub-folder tree, and everything it needs lives inside it, so
the whole folder can be moved, copied or committed as one thing. Its manifest is always
`project.json` in the root, which makes "is this a project folder?" one existence check
(`project::is_project_root`).

```
shoreline/                 the project root; its name is the project's default name
  project.json             the manifest
  levels/                  one file per level ("lvl-level")
  tilesets/                one file per tileset ("lvl-tileset")
  rules/                   one file per rule set ("lvl-wfc": WFC + auto-tiling + height steps)
  paths/                   exported/imported path graphs ("lvl-paths")
  assets/
    atlases/               tileset atlas images, copied in on import
    images/                heightmap seed images and layer image imports
  artifacts/               PNG exports: the full map, the mini-preview, layer images
```

`project::Folder` names those folders and `subfolder`/`resolve`/`relativize` compose paths against
the root. Every path a project file stores is **relative to the root** with `/` separators, whatever
the platform; a path that stays absolute is one that points outside the tree, which is how a
deliberately external reference is expressed. Composing a path can overflow a `Path`, and the inputs
are the user's, so those functions return a `Status` rather than truncating: a truncated path would
name a different file. `ensure_tree` creates the whole tree and is idempotent.

**Assets are copied in, not linked.** Choosing an atlas or a seed image from outside the project
goes through `plan_copy`, which says where the copy would land (`assets/atlases/` or
`assets/images/`), whether it would be renamed around a different file of that name (`forest.png` ->
`forest-2.png`), and whether the identical bytes are already there — so the confirmation dialog can
say exactly what will happen before `apply_copy` does it.

`plan_copy` makes **both** the source and the root absolute against the working directory before it
compares them (`absolute_path`). Either can be relative -- a source is whatever the user gave, a
root is wherever the manifest was written -- and neither can be compared with the other until they
are in the same frame of reference. Resolving the source against the *root* instead reads
`forest.png` as `<root>/forest.png`, so an outside file looks like one already inside and is stored
under a path that names nothing.

**Every read of a stored asset path goes through `project::resolve` first**, because the path is
relative to the project *root* and the process working directory is wherever the editor was started
-- so the two are not the same folder and resolving against the wrong one simply fails to find the
file. `editor::load_atlases` resolves each tileset's atlas (for both the probe and the decode),
`//tools:lvl_convert` resolves it the same way, and the heightmap's seed image is resolved on the UI
thread before `HeightmapJob` starts, since a job has no project to resolve it against. `resolve`
passes an absolute path through unchanged, which is what a deliberately external reference is, and
with an empty root it leaves the path alone -- so a bare level file behaves as it always did. The
working directory is deliberately **not** changed to the root: a `chdir` would silently re-point the
recent-files list, the dialog's remembered folder and every relative path on the command line.

An atlas that will not load is not logged -- it draws red in the Tilesets panel -- so the scripted
session checks `App::assets::status` itself after the atlas it copied in is reloaded (smoke step
38), which is the one thing that would otherwise let this break again unnoticed.

`editor::request_import` is the gate: it plans the copy and, for a file outside the tree, records an
`App::pending_import` that `import_prompt` draws as a modal beside the other prompts — **Copy into
the project** (what Enter takes), **Link in place** or **Cancel**. Three cases never reach it: a
file already inside the tree is relativized and stored, a project with no folder yet has nowhere to
copy to so the path is stored as it is, and nothing is stored at all until the user answers.

What the prompt says comes from the plan rather than from a guess: "it will be copied to X", "a
different file of that name is already there, so the copy will be X-2", or "the same bytes are
already at X, which will be used". **Link in place** is deliberately offered, because a reference
may point outside on purpose — and the Project panel then lists every such reference in amber with a
**Copy in** button (`copy_asset_into_project`, the same prompt), which is how a project built with
outside references is made self-contained afterwards.

**The manifest is always JSON**, the one deliberate exception to "binary is a translation of the
same data": it is a few kilobytes of names, never on a hot path, and a fixed name is what makes the
existence check work. Levels, tilesets, rule sets and path graphs keep both formats. The manifest
never stores the project's own location — `Project::root` comes from where the file was read, and
`save_project` sets it from where the manifest is written, so writing one somewhere else moves the
project there and a moved folder still works.

**The tilesets and rule sets are not in the manifest; it names their files.** `save_project` writes
each one through the standalone `lvl-tileset` and `lvl-wfc` formats the editor already imports and
exports, and `load_project` reads them back. That is what those formats existing buys:
per-tileset and per-rule-set diffs stay small, and a project is a folder of files a person can read
rather than one large document. `Project::tileset_files`/`rule_files` are parallel to `tilesets` and
to `rules` — where each one is written and was read from. A part with no file yet gets one
from its name (`project_file`: anything a file name should not hold becomes `-`, so
`cliffs / rock!` gives `tilesets/cliffs-rock.tileset.json`), and that choice is recorded, so
renaming a tileset does not move its file or orphan the one the manifest already names.

They are read **in the order the manifest lists them**, because each one appends and so takes the
next id — the ids the member levels' cells hold. A tileset file that will not load therefore fails
the whole project instead of being skipped, since skipping it would shift every id after it.

**Only one level is ever resident**, because a level carries whole grid planes and a project may
hold many. The others are an index: a `LevelEntry` each, holding the name, the file, the grid size
and the names of the level's portals and markers. That is enough to list the project, to offer a
portal a destination, and to say whether a link still resolves (`portal_link` ->
`kResolved`/`kSelf`/`kUnknownLevel`/`kUnknownPortal`) without loading one sibling. Opening a project
reads one small file. The portal/marker names live in one flat `object_names` table that each entry
spans into; it is reserved to `kMaxObjectNames` up front and never grown, so the rebuild every level
save does reuses the same block instead of leaving the old one behind in the bump arena.
`set_level_objects` is that rebuild, and `remove_level` compacts the table the same way.

Levels address each other **by name**, not by path, so moving a level's file within `levels/` does
not break the portals pointing at it. `Portal::target_level` is a `Name` into the index.

**A file still carries that field `Path`-wide**, because it held a path before projects and a path
does not fit a `Name`. `PortalRecord::target_level_field` is the wire form, and
`level::migrate_portal_targets` (called by both loaders, before `restore`) turns it into a member's
name: a string that already names a member is kept, a path is matched to the member whose file has
that name (`find_level_by_file_name`), and anything else is reduced to the file name's stem -- what
the level would have been called -- and logged as a **warning**. A portal that does not resolve is
something to fix in the editor, not a reason to refuse the file.

**The open level is a member, and the index follows it.** `editor::refresh_level_entry` runs
whenever a level is saved or loaded: it finds the member by name (how levels address each other),
falling back to the file so a level saved under a new name takes the new name rather than becoming a
second member, adds it if it is new, and rebuilds what the index says -- the grid size and the names
of its portals and markers, read from the live registry. It marks the project modified only when
the index now says something *different*, compared before the name table is rebuilt: it runs on
every load as well as every save, and loading a member re-reads what the manifest already said, so
a project must not report unsaved changes merely for having been opened. That is also what makes a bare level file
open as a single-member project, with no "no project" branch anywhere.

In the editor the two destination fields are **combos, not text boxes**, which is the point: a typed
path was indistinguishable from a working link. The target level is picked from the project's
members (plus "(this level)"), and the target portal from the live registry when the destination is
this level -- so a portal made this session is offered before any save -- or from that member's name
span otherwise, which is what a project knows about a level it has not loaded. `portal_link_of`
answers whether a link can be followed, `link_message` says what is missing, and a dangling portal
draws red in the objects list and in the viewport with that message as its tooltip. A value naming
nothing is still listed, red and disabled, so it can be seen and replaced rather than silently
dropped.

**The Project panel** (View > Project) lists the members -- each with its grid size, portal count
and file, red when the file is not where the manifest says -- with Open, rename, and a delete button
that removes the member from the index and leaves its file on disk. The open level is shown as
"(open)" and renamed through its own settings rather than here, since it is the document. Below it
are the tilesets and the rule sets, each rule set with a delete button and the one this level uses
marked. "Add a level..." records an existing file as a member; "Rescan" runs `RescanProjectJob`.

**A member level is saved in project scope**, so the manifest stays the one authority for the
catalogs, seasons, tilesets and rules its cells name and the project's levels cannot drift apart. A
level with no project folder is saved standalone, as a level file has always been. That is also why
Ctrl+S writes the **manifest first and synchronously**, then starts the level save: a member's file
means nothing without the manifest, and writing it first keeps the two out of each other's way.

**Switching level** goes through `request_switch`, which uses the same prompt quitting does, since
the open document is about to be replaced either way (Save and switch / Discard and switch /
Cancel). The load itself is `LoadLevelJob` with `keep_project` set: a member carries none of the
shared sections, so it is read into the open project rather than the spare and `finish_job` does not
swap the project. `decode_atlases` is a separate flag because the two are separate questions: a
switch leaves the tilesets as the same objects, so **the atlases are not decoded again** -- not
re-decoding them on every switch is most of the point of the project owning them -- while opening a
project has just read new tilesets in and needs them. Both are one job, because only one job runs at
a time: opening a project reads the manifest synchronously and then loads its first member with
`decode_atlases` on, rather than starting an atlas job the level load would have displaced.

**New project / Open project / Save project** are synchronous rather than jobs: a manifest and the
files it names are kilobytes and never on a hot path. What needs a job is the level that follows,
which is `LoadLevelJob` as it stands. A new project is made by choosing where `project.json` goes,
rather than by picking a folder: that needs no directory-picking mode and matches `save_project`,
which takes the project to be wherever its manifest is written.

**The dialog suggests a folder of its own**, `<level name>/project.json` rather than a bare
`project.json` (`project::project_file` slug-ifies the level's name, so any name the user gave it
makes a file name). The tree is created beside the manifest, so accepting a bare `project.json`
scatters `levels/`, `tilesets/`, `assets/` and the rest into whatever folder the dialog happened to
be in, which is recoverable only by hand. It is only a suggestion -- the folder can be renamed or
dropped in the name box like any other part of a save name.

**New project keeps the project the open level draws on** and gives it a home on disk -- it creates
the tree, names the project after the folder if it has no name yet, and writes the manifest. It
deliberately does *not* replace it with an empty one (which is what `project::create_project` does,
for a caller that wants that): making a project out of the level you have open must not throw away
the tilesets, catalogs and rules its cells name. The level is not a member until it is saved into
the tree.

`App::project_path` and `App::project_modified` sit beside `App::path` and `App::modified`. The two
pairs are separate because the two things are saved separately -- editing a tileset or a rule set
changes the project, not the level that happens to be open -- and the title's star, the quit prompt
and the browser's "leave site?" prompt all mean "either has changes".

### Starting up

Started with nothing named on the command line, the editor raises a **start-up prompt**
(`editor::welcome_prompt`, `App::welcome`): the recent *projects* that are still on disk, each named
by its folder, plus **New project...** and **Open project...**. That is the one question start-up
has an answer to, and the manifest is always called the same thing, so reaching a project through
File > Open would otherwise have the user navigating to a folder to pick a file whose name they
already know.

It is deliberately an **escape rather than a gate**: **Not now** (and Escape) leaves the blank level
the editor has always started with, and New project turns that into a project at any time. It never
appears when a level, a manifest or a project folder was named, nor after `--demo`, because those
have already said what to work on. Choosing something clears `App::welcome` *before* the action
runs, so the file dialog does not open underneath the modal; a job in flight or an open dialog
clears it too, so the prompt cannot sit over the progress dialog.

The scripted session (`--smoke`) is **not** exempt: step 0 checks the prompt came up and that making
a level puts it away, so the one path a user takes is the path the test takes.

### Recent levels and projects

File > Open recent lists the last `RecentFiles::kCapacity` (10) levels **and projects** opened or
saved, newest first. A project is what you reopen, so opening or creating one records its manifest
beside the levels; the stored format is unchanged (one path per line), because a manifest is told
from a level by the same check the command line uses -- its file name is `project.json`. Such an
entry is listed by its *folder* (every manifest is called the same thing) and marked "(project)",
and choosing it opens the project, which goes on to load its first member. `editor/src/recent.cpp` keeps the list in `App::recent` (fixed `Path`s, so no heap) and
writes it to `config_path("recent_files.txt")` as one path per line whenever it changes; it is read
back the first time the menu is drawn or an entry is recorded. `finish_job` records a path once a
level load or save succeeds, and drops one whose load failed, so a file that has gone away leaves
the list when it is next tried; until then the menu shows it disabled. "Clear the list" empties it.
Choosing a level entry replaces the open document just as "Open..." does.

The positional command-line argument takes a level, a project manifest or a project **folder**, told
apart by whether there is a `project.json` to be found -- one existence check, because a project is
where its manifest is. `--project=DIR` is the same thing said explicitly, through the same code.

### Quitting

The main loop runs until `App::exit`. Every way of quitting goes through `request_exit()`: the
window's close button (the GLFW close flag is taken back with `cancel_close()` each frame), File >
Quit, and `--frames`/`--smoke`, which force it. With unsaved changes (`App::modified`) it shows the
"Unsaved changes" prompt instead: Save (`save_and_exit()`: exit once the save job succeeds; with no
path yet, via the save dialog, and cancelling that dialog cancels quitting), Quit without saving,
or Cancel. In the browser, `web::set_unsaved_changes()` arms the page's `beforeunload` "leave site?"
prompt while there are unsaved changes.

### Tile groups

A **tile group** is a named thing drawn across several tiles -- a house, a tree, a rock -- stamped
onto the map as one thing. A group lives **in the tileset** (`level::TileGroup`,
`level/tileset.hpp`), not in a level or a rule set, because every tile it names is one of that
tileset's: so it travels with the atlas it was cut from, it is written by the standalone
`lvl-tileset` file a project already saves each tileset through, and removing the tileset takes its
groups with it without any level or rule set having to be renumbered.

The footprint is a rectangle and its cells are `size.count()` tile indices in **row-major order in
the tileset's one flat `group_cells` table**, which each entry spans into -- the same shape the
project's level index uses for its object names, so adding a group does not leave the last block
behind in a bump arena. A cell holding `kNoTileIndex` is a **hole**: a tree's bounding box has
corners the tree does not fill, and stamping must leave what is under them alone rather than erase
it. `set_group` adds one (replacing the group of that name, so a footprint that changed size does
not strand its old cells), `remove_group` compacts the table the later groups span into, and
`validate_groups` is what a file goes through -- the table is always written packed and in the order
the groups are listed, so a file whose groups do not span it exactly cannot say which cells are
whose, and a group reading another group's cells is worse than a tileset with no groups, so all of
them are dropped. `reslice` keeps a group's shape and holes the cells whose tiles the atlas no
longer has, since re-slicing is how a tile size is corrected and the correction usually brings them
back.

In the editor they are **Tilesets > Tile groups**: the list, a delete button per row, the chosen
group drawn as it will be stamped, and "From the palette selection", which is how one is made --
the palette is already how several tiles are picked and already how they are laid out, so the house
in the atlas is the house on the map, holes included. Choosing a group sets `App::group` (a
`GroupSelection`: a tileset and a `GroupId`), and **`paint_stamp` returns it before anything else**,
which is the whole of the stamping work: the paint tool, the hover footprint and the hover outline
of the cells a stamp writes all go through that one function. A group and the palette selection are
two answers to "what does a stamp write?", so choosing a group empties the selection, clicking in
the palette clears the group, and Ctrl+Shift+A clears both; the toolbar names the chosen group while
the Paint tool is active, since a stamp is not visible on the canvas until it is painted.

Groups are tileset data, so adding, renaming or removing one marks the **project** modified rather
than the level, and like the other tile metadata it is not undoable. Stamping is an ordinary layer
edit and is.

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

### Three kinds of tile adjacency rules

The **project** holds all three, as one `project::RuleSet`, and the "Tile adjacency rules" window has a tab
for each. They are one set because they are used as one: a terrain's border tiles, a cliff's step
tiles and the WFC tiles beside them all come from the same tileset. The set's name is `wfc.name` --
the one the rules file already carries -- so there is no second name to keep in step.

**A project holds a list of sets, and a level names the one it uses** (`Level::rules`), because its
levels differ by biome: a cave tileset and an overworld tileset want separate adjacency graphs,
separate terrains and separate cliff art. One global set would force every tile of every tileset
into one WFC graph, and "only what the layout shows" would then forbid cross-biome neighbours
between levels that never touch. A project always holds at least one set, as it always holds at
least one season, so `project::rules_of(project, level)` always resolves -- to the set the level
names, or the first when it names none or names one that has gone, which is what a level written
before the sets were plural gets. `editor::rule_set(app)` is that lookup for the open level, and is
what every panel reads.

`Level::rules` is deliberately **not** part of `Settings`: it is a reference to the project rather
than a setting of the level, and the two formats carry it differently. A standalone file carries its
rules, which name themselves, so the loader takes the level's set name from what it read and no new
field is needed; only a project-scope file has to say which set it means, and it does so at the
document's top level in JSON and in the `kProjectLevel`-only branch of the binary metadata block.
Both of those are places no file written before now has bytes in, so `Settings` keeps its layout and
every existing `.lvlb` still reads.

Reading a rule-set file interns the set **by name** (`project::intern_rules`), so reading the same
file twice does not leave two. The project's blank starting set is deliberately *not* taken over by
the first set read in: "the project has one empty set" cannot tell that set apart from one the user
has just added and not filled yet, so the second rules file read into a project would silently
replace the first. A project that has only ever had rules imported therefore keeps an unused set,
which the user can remove -- the cost of the other way round is losing a rule set without saying
so. For the same reason the manifest rebuilds `rule_files` from where the sets actually landed
rather than assuming they come back in order, and refuses two files holding a set of the same name.

- **WFC** (`level::WfcRules`): a permission relation on pairs, per direction -- what *may* sit next
  to what. The WFC generator solves it over a region.
- **Auto-tiling** (`level::AutoTileRules`, `level/auto_tile_rules.hpp`): terrains, each keying its
  tiles on the pattern of like neighbors around a cell.

- **Height steps** (`level::StepRules`, `level/step_rules.hpp`): terrains keyed on how a cell's
  height *level* compares with its neighbors', which is what draws a cliff.

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

### Height steps

Ground cut into whole levels, with a cliff wherever two levels meet. This mirrors how gft tiles its
worlds (`getSlopeFromBumpCode` in gft's `game/src/data/world.cpp`), so what the editor draws is what
the game will draw -- and since gft works the tiling out from the per-cell height it reads, what a
level really owes the game is the **levels**, not the tiles.

- **`LayerKind::kLevel`** is the integer height level per cell, an `IdGrid` plane like the semantic
  and material layers. `procgen::terrace` cuts a heightmap into it, either into a given number of
  bands or into bands of a given height (`TerraceMode`), over the heightmap's own range unless one
  is given. `skew` makes the bands a geometric series rather than equal ones: below 1 each band is
  thinner than the one below it, so the ground is cut finely towards the top and coarsely at the
  bottom, which is what gives a skyline more steps as it rises. By count the bands still divide the
  range exactly however they are skewed; by rate a skew below 1 makes them converge, so a tall
  enough map runs into `max_level`. It is an ordinary layer, so the levels can be touched up by
  hand and are saved.
- **`level::StepType`** is the vocabulary: 18 tiles, in either of two block shapes
  (`level::StepLayout`). Three across and six down is how gft's own sheets hold them, an *outward*
  row and an *inward* row alternating; six across and three down is the same eighteen as two 3x3
  blocks side by side, the step-ups on the left and the step-downs on the right, which is easier to
  read off a tileset. Outward is the rim of ground standing above its neighbors (its edges, its
  convex corners, and the flat middle); inward is where that rim turns back on itself around a
  hole. The rules window draws its slots in whichever shape the palette selection is.
- **`level::step_type_of`** is gft's table, ported. Each neighbor is *level with*, *below* or *above*
  this cell, and the tile follows from the whole eight-neighbor pattern. Every one of gft's cases
  leaves the "a neighbor is above me" group empty, so the table reduces to a map from "which
  neighbors are below me" to a tile, and a cell with anything above it is `kFlat`: **only the higher
  side of a step is drawn**, and the ground at the foot of a cliff stays plain. Patterns the table
  does not name are `kFlat` too, as they are in gft.
- **`procgen::tidy_levels`** squares the terraces off, and is what makes the rest work. The table
  names 20 of the 255 ways a cell's neighbors can stand below it, so a heightmap cut straight into
  levels is full of shapes with no tile: one-cell spurs and notches, and -- the larger half of it --
  45-degree boundaries, which a staircase with one-cell treads is. Morphology alone cannot remove
  those, since a diagonal edge is two cells wide everywhere.

  The pass works on the **upper sets** (the cells at level k or above), built from the top down and
  unioned with the terrace above *before* being cleaned, so they stay nested and their unions are
  squared off too. Each one gets an opening (spurs narrower than `min_feature` go), a closing
  (notches fill), a **snap** to a lattice of `min_feature` blocks where each block goes the way its
  cells vote, and a pass that fills blocks meeting only at a corner. A table-driven **repair**
  (`square_corners`) then backstops it, growing a terrace a cell at a time until `step_type_of` has
  an answer; `tile_steps` counts whatever is still left as `unexpressed`.

  **Voting a block at a time is the point.** The repair on its own cascades: raising a cell to fix
  a 45-degree boundary makes offenders of its neighbors, and a diagonal carries that along its
  whole length, so valleys flood -- on a 96x96 map of five terraces it moved half the cells and
  raised some by four levels. A block is decided once, by what was already there, so it cannot
  cascade: the same map moves 6% of its cells, none by more than one level.

  Two things it is honest about rather than promising. The lattice is anchored at the grid's origin
  (choosing a phase per terrace was tried and is worse, because terraces on different phases do not
  nest squarely), so a feature exactly `min_feature` wide lying across the blocks can still be
  voted away -- `min_feature` is a preference, not a guarantee. And the four inward corner tiles
  draw a single-cell notch perfectly well, so the repair may leave one that a two-cell smoothing
  would fill again: the repair alone is a fixed point, the whole pass with smoothing on is not.
- **`procgen::tile_steps`** draws a level layer with those tiles, **one tile layer per height
  level**. They have to be separate: a cliff tile is transparent where the terrace below shows
  through it, which is how these tilesets are cut, so the terraces are composited rather than
  flattened into one plane. Stacked in order, level 0 at the bottom, they come out right.

  Layer k draws the cells at level k or above: the middle of that ground flat, its rim edges and
  corners, and everything below it cleared, so re-running after the levels change leaves nothing
  behind. Level 0 therefore covers the whole map and is flat throughout, which is the opaque base
  the rest composite over. Within one layer the only question is whether a neighbor is part of the
  same ground, so no cell is ever "above" another: that is what the stacking expresses. Cells off
  the grid count as part of it, so the map's edge is not a cliff.

  **The layers follow the levels, not the terrains.** A terrain only says which tiles a level is
  drawn in (`level::step_terrain_at`), so one terrain covering every level is the ordinary case and
  several let the art change with height -- grass cliffs low down, rock higher up. Cells at a level
  no terrain covers are counted in `untermed` and left alone.

  In the editor a level's layer is the tile layer named `step <k>` (`editor::step_layer_of`), and
  `editor::add_step_layers` adds the missing ones lowest first. Cutting the heightmap into levels
  adds them and draws them in the one call, since it is the levels that say how many are wanted.

- **Only the tile visual is split.** The collision mask -- and any other per-cell data a level
  keeps, semantics and materials among them -- stays a single flat layer, because it follows from
  the levels rather than from any one terrace. It is optional and re-runnable: `StepCollision::kBelow` marks the cells
  at the foot of every cliff solid (leaving the ground on top walkable), `kAbove` marks its edge
  instead. Either
  way it sets `collision::kSolid` where a step is and clears it where there is none, leaving the
  other bits of the mask alone, so recutting the levels never leaves a wall where the cliff has
  gone.

Generate > Height steps runs the passes (`editor::apply_terrace`, which tidies as well unless that
is turned off, and `editor::apply_steps`), and recutting the levels redraws the tiles unless that is
turned off. The panel reports what the tidy moved and how many cells the tiles still cannot express.

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
its position and the level's seed, weighted by `TileInfo::scatter_weight` -- how often a tile is
picked against the others it is scattered with (0 keeps it out; all-zero or absent weights stand
for 1 each, so an untouched tileset scatters evenly). The weight is ordinary tile metadata: it is
edited in the Tile metadata panel, in its batch edit, and in the Fill tab's own rows, which also
show what each weight comes to as a share, and it travels in the tileset. So the scatter is a function of the cell, not of the order cells
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

Rows of layers, semantic types, materials, seasons, path graphs, portals/markers, tile groups and
tile properties start with a delete button (`delete_button`). `request_delete` records a `PendingDelete`, and
`process_deletes` (once per frame, after every panel has drawn and never while a job runs) shows a
confirmation dialog listing what else changes, then applies it; Shift+click skips the dialog.
**Deleting a catalog entry retires it; it does not renumber.** Catalog ids are stored in millions of
cells across a project's levels, only one of which is resident, so erasing an entry and shifting the
ids after it would silently turn every sibling level's cells into something else.
`project::remove_semantic`/`remove_material` therefore set `retired` on the entry and leave its id
where it is. Everything the project itself holds is resident, so tile metadata, seasonal variants
and the rule sets' WFC tiles are cleared outright, as are the open level's cells; a sibling's cells
keep the id, and a retired entry reads as "none" everywhere (`render::colorize` draws it
transparent), which is what makes leaving them safe.

`project::intern_semantic`/`intern_material` give a retired entry's name its id back, so a file
naming it does not append a duplicate -- but they deliberately do **not** un-retire it. Interning is
what reading a file does, and a file must not quietly undo the project's decision to remove an
entry.

Retirement is the **manifest's** to carry, so it does not survive opening a *standalone* level:
such a file carries the catalogs itself and is the whole source of the project it is read into, so
the project that comes back has nothing retired. That is the right answer rather than a gap -- there
is no manifest in the picture to have recorded the decision -- but it is worth knowing, because it
means "I removed an entry" is a fact about a project on disk and not about a level file. The catalogs panel lists a retired entry greyed as "(removed)" with a **Restore** button
beside it, which is the one way back; hiding it would leave the user wondering why the ids below it
never close up. Every picker skips retired entries, since nothing new should be given one.

**Compaction is the only thing that renumbers**, and it is a deliberate act. It is planned once
(`project::plan_compaction` -> a `CatalogPlan`: the new id of every entry, or `kRetiredEntry`),
applied to every level of the project (`apply_compaction`), and only then are the entries dropped
and the project's own references rewritten (`finish_compaction`) -- in that order, because until
the last step the plan's old ids are what the project's vectors are indexed by. The catalogs panel
offers it when `needs_compaction` says there is something to do, and clears the undo history, since
every id below a dropped entry has moved out from under the snapshots in the ring. `editor::CompactCatalogsJob` is what drives it across a
project: it plans once, then loads, renumbers and saves every member but the open one, which the UI
thread does in `finish_job` since a job may not touch the document. A member is written back in
**project scope** whatever it was read in, because the plan is the project's: a file carrying its own
copy of the catalogs would be left naming the old ids beside the new cells. If a member cannot be
read or written the job stops, says which, and the entries are **not** dropped, so the ids on disk
and the ids in the project still agree.

`editor::RescanProjectJob` is the other half: it reads every member and rebuilds what the index says
about it, for files changed outside the editor. Both jobs stage through the **spare** document and
project -- the pair a load already uses -- rather than carving a document out of the job arena,
which is the right size by construction where a fraction of the job arena is not: exhausting an
arena is an assert, not a `Status`. Only one job runs at a time, so nothing else is staging into
them. Reading a whole level to learn its grid size and object names is wasteful, and both formats
could read "everything but the cells" cheaply, but that is new I/O surface and is left until a
rescan is slow enough to matter.

`project::remove_tileset` keeps erasing and renumbering -- it drops the tileset and calls all four
`clear_tileset_refs` overloads, for the open level and the three rule kinds -- but it returns a
`Status` and **refuses outright** once the project has more than one member level, since the others
are not resident to renumber. `project::semantic_uses`/`material_uses` only count the resident
level, so the confirmation says "in this level" rather than claiming to know the project.

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
  serialization and file errors (including a file written before a field existed still reading, via
  the header version in the serializer's context), JSON zero-heap check, jobs/random, images.
- `//level`: model operations (resize, derivation, clearing a tileset's references, portals/paths/routes, WFC
  rule learning, rules from a palette layout, a step terrain read from either block shape, batch
  tile and constraint edits (scatter weight included), tile groups spanning one packed cell table
  (replaced by name, removed with the table compacted, holes and cells outside the footprint, a
  group refused for having no name, no footprint or nothing but holes, a cell past the last tile
  taken as a hole, a re-slice keeping the shape, and groups that do not pack the table all dropped),
  terrains from a
  nine-slice or an inner-corner block, the whole thirteen-tile set plus spare middles in one
  selection, the sides-first fallback, what each neighborhood distinguishes); full JSON and binary round trips with 4 workers and with 1 (tile groups and their holes among what they carry, and a standalone tileset file taking its groups to another project), JSON→binary→JSON
  stability, a project-scope save leaving the shared sections out and round-tripping against a
  project that already holds them while leaving it untouched, a project-scope level naming the rule
  set it uses and resolving to it in a project that holds the sets in another order, a standalone
  level carrying only the set it uses and taking its name from it, a portal target written as a path
  becoming the member whose file it names, one that names no member keeping a stem and warning
  rather than failing, the standalone scope being the
  default and still reading into an empty project, standalone files moving between levels,
  malformed/truncated files, a level whose layers
  and file are bigger than one worker's share, saves split across waves and saves streamed layer by
  layer (both byte-identical to an unsplit one), a load with worker arenas far too small to stage a
  layer, headers at their widest.
- `//project`: path composition (the folder names, joins that refuse to truncate, resolve/relativize
  round trips, a path outside the root staying absolute, and a part's file name made from any name
  the user might give it), `ensure_tree` being idempotent, copying
  an asset in (a plain copy, a rename around a name clash, identical bytes reused, a file already
  inside, and a file beside the project staying outside it when the root and the source are both
  relative), the level index (members added/found/renamed, each member's object names and their spans
  surviving a re-save, a portal link validated without loading its target, removal compacting the
  name table), seasons (added, found, removed, the cycle never empty), catalogs interned by name and
  removing an entry remapping the resident level's cells, tile metadata, seasonal variants and WFC
  rule tiles, tilesets (added, found, removed with the level's references following, and refused
  once the project holds several levels),
  rule sets (found, added, interned by name, a level resolving to the set it names and falling back
  when it names none or one that has gone, removal keeping at least one), catalog removal retiring
  an entry rather than moving the ids after it (what the project holds and the open level's cells
  cleared, a sibling's left, interning the name again giving its id back without bringing it back),
  compaction planned once and applied to every level together,
  and the manifest (round trip carrying the tilesets' and rule sets' own files and a tileset's tile
  groups with them, several rule sets
  each getting their own file and coming back in the order the manifest lists them, a part keeping
  the file it was given across a rename and a new one getting its own, saving elsewhere moving the
  project there, a named tileset file that has gone refusing the project, stable when written twice,
  missing level files flagged, create/open, named after its folder, which entries are retired, and
  every malformed shape refused).
- `//procgen`: masked fills (a height band, a ground type, the two together, the scatter being even
  and staying put under a different mask, scatter weights deciding the shares (a weight of zero,
  negative weights, all-zero and equal weights coming to the same thing), regions and clipping,
  sparing what is drawn, no tiles); height steps (every case of gft's table and that anything else
  is flat, a plateau's rim, inward corners, the map edge, terrains picked by level, a layer per level holding only its own
  ground and being cleared where that ground has gone, one terrain covering every level and several
  changing the art with height, a level with no terrain left undrawn and counted, a null layer
  skipped, collision staying one flat mask (set and cleared on either side of a cliff, and filled
  even with no tile layers at all)); terracing (by count and by rate, a flat map,
  a given range, clamping and the ceiling, and the skew -- equal bands unchanged, each band a
  fixed fraction of the last in either direction, and converging bands held at the ceiling); tidying (spurs and notches, the map edge kept, a lone
  high corner squared off rather than shaved, every pattern drawable afterwards across several
  seeds and band counts, the repair being a fixed point, and the terrain surviving it -- nine cells
  in ten unmoved, none by more than one level);
  noise determinism, parallel == serial heightmaps, seed images, erosion,
  rivers/lakes, breach tunnels, WFC with adjacency, layer constraints, kept tiles/regions, impossible
  constraints; terrain painting (a nine-slice from one painted cell, the border re-tiling as the
  shape grows, inner corners, no border, the map edge, middles scattering over an area and staying
  put when it is repainted, a terrain with no tiles); the WFC-scored
  fallback (edges around painted centers, a path drawn one cell at a time, stability once the rules
  hold, corner fills with `diagonals`, tiles the rules do not know, rules that cannot express the
  shape).
- `//render`: tile colors, compose with seasons and overlays, size refusal, mini-preview, PNG export,
  one layer exported as a pixel per cell, a retired catalog entry drawing as nothing.
- `//editor:jobs_test` (atlas loading: a large atlas with small worker arenas; too little job or asset
  memory reported per atlas; a level saved and loaded through `JobMemory` whose layers exceed a
  worker's scratch; a rescan reading every member with only one resident and reporting one it cannot
  read; a compaction rewriting every member but the open one, in project scope, with the entries
  still in place), `//editor:undo_test`, `//editor:style_test` (style profile round trip, partial and bad files).
- `//editor:up_down_test` (tag `display`): `lvl --smoke` drives a scripted session through the UI code
  paths: the start-up prompt coming up with nothing named and going away once a level is made, demo
  level, generators (incl. WFC with corner rules, the adjacency rules window and the neighbor
  editor), undo/redo, save/load
  in both formats, all exports, a style profile save/load, deletes, a batch tile edit, adjacency rules and
  batch constraints from a palette selection, the file dialog, reopening the newest recent level, a
  multi-tile paint stamp, a tile group made out of the palette selection, chosen, stamped holes and
  all, and deleted (which stops the stamping), a thirteen-tile terrain taken from one palette selection and painted (the
  auto-tiling tab), a masked fill (scattered over a height band, stable when re-run, weighted shares and a
  weight of zero, and with the scatter off), a heightmap terraced, tidied and its steps drawn onto a layer per level (the
  layers added by the cut itself, each holding its own ground and nothing below it, every cell's tile the one its level's shape calls for, nothing left that the tiles cannot express, the blocked cells the ones at the foot of
  a step, and redrawing changing nothing), saving making the open level a member of its project and
  a portal resolving against it (its own portals from the live registry, a level no member is named
  after and a portal a member does not have both reported), deleting a catalog entry retiring it and
  the project then saying it has something to compact, the panel's own compaction dropping exactly
  the entries retired for it,
  a project written to disk (its tree made and its manifest written, the open level saved into
  `levels/` becoming its first member and its file carrying none of the shared sections, a second
  member from a save under another name, a portal into that sibling resolving from the index alone,
  switching to it without replacing the project or decoding the atlases again, the compaction job
  rewriting the other member, a rescan leaving every member with a size, Ctrl+S writing the manifest
  and the level, and the whole project read back with both levels, its tilesets and itself in the
  recent list); a layer written to the project's artifacts folder as a PNG; an atlas chosen from
  outside the project asking first, with the plan naming where it would land, and once copied in
  being stored relative to the root and asking nothing; saving a level and exporting a tileset each
  offering the project folder that kind of file belongs in;
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
