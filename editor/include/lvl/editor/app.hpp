/**
 * @copyright 2026-present Brian Cairl
 *
 * @file app.hpp
 *
 * Editor state. All of it lives in one memory block reserved at startup (see Memory): two document
 * arenas (the open level, and a spare that loads go into, swapped on success), two asset arenas for
 * decoded atlases (swapped on reload), the undo ring, a per-frame scratch arena, the main thread's
 * JSON arena and a jobs arena split between background-job workers.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/editor/jobs.hpp"
#include "lvl/editor/platform.hpp"
#include "lvl/editor/style.hpp"
#include "lvl/editor/undo.hpp"
#include "lvl/level/level.hpp"
#include "lvl/procgen/fill.hpp"
#include "lvl/procgen/noise.hpp"
#include "lvl/procgen/scatter.hpp"
#include "lvl/procgen/steps.hpp"
#include "lvl/procgen/watershed.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/render/compose.hpp"

namespace lvl::editor
{

inline constexpr std::size_t kMaxTileSets = 64;
inline constexpr std::size_t kMaxLayers = 64;
/// Tile indices are 16 bits, and slicing caps a tileset at this many tiles
inline constexpr std::size_t kMaxTiles = 0xFFFF;

struct MemoryConfig
{
  /// Total bytes reserved at startup (the editor's one large allocation)
  std::size_t total = 1024UL << 20;
};

struct Memory
{
  memory::Arena root;
  std::array<memory::Arena, 2> documents;
  /**
   * @brief The open project and a spare, paired with the documents
   *
   * A load interns the file's catalogs, seasons, tilesets and rule sets into the project, so a
   * failed load would leave it half-filled. The spare is swapped in on success, exactly as the
   * documents are.
   */
  std::array<memory::Arena, 2> projects;
  std::array<memory::Arena, 2> assets;
  memory::Arena undo;
  memory::Arena frame;
  memory::Arena json;
  memory::Arena jobs;
};

enum class Tool : std::uint8_t
{
  kPan,
  kPaint,
  /// Paint centers and let the tile adjacency rules re-fill around them
  kAutoTile,
  kErase,
  kFill,
  kRect,
  kPick,
  kSelect,
  kPortal,
  kMarker,
  kPathNode,
  kPathEdge,
};

/// How a scalar (height/water) brush changes cells
enum class ScalarMode : std::uint8_t
{
  kSet,
  kRaise,
  kLower,
  kSmooth,
  kFlatten,
};

struct Brush
{
  /// Cells from the center (0 = one cell)
  int radius = 0;
  ScalarMode scalar_mode = ScalarMode::kRaise;
  /// Scalar brushes: amount per application (raise/lower) or blend factor (smooth/flatten)
  float strength = 0.05F;
  /// Scalar brushes: value for kSet
  float value = 0.5F;
};

/// Which kind of tile adjacency rules the rules window is editing
enum class RulesMode : std::uint8_t
{
  /// What may sit next to what, per direction (level::WfcRules), for the WFC generator
  kWfc,
  /// Terrains keyed on the pattern of like neighbors (level::AutoTileRules), for auto-tiled painting
  kAutoTile,
  /// Terrains keyed on the difference in height level (level::StepRules), for terraced ground
  kSteps,
};

/**
 * @brief Settings of auto-tiled painting (the Auto tool)
 *
 * The tool paints the palette tile (or stamp) as the middle of a shape and tiles the border around
 * it from the level's rules: from the terrain the painted tile belongs to
 * (lvl::level::AutoTileRules, which is what prescribes edges and inner/outer corners), or, for a
 * tile with no terrain, by scoring the WFC adjacencies around the stroke. These settings are the
 * fallback's; a terrain carries its own border width. See lvl::procgen::autotile.
 */
struct AutoTile
{
  /// WFC fallback: cells around the painted ones the rules may re-fill
  int radius = 2;
  /// WFC fallback: relaxation sweeps (each carries the rules one cell further outward)
  int iterations = 4;
};

/// What the last auto-tiled stroke did (the viewport's status line)
struct AutoTileStatus
{
  /// Whether a terrain tiled the stroke, or the WFC fallback scored it
  bool terrain = false;
  FixedString<64> name;
  std::size_t cells = 0;
  std::size_t changed = 0;
  /// Cells the rules could not fill (no tile for the pattern / no tile satisfies the adjacencies)
  std::size_t trouble = 0;
};

/// What painting writes, per layer kind
struct Paint
{
  level::TileRef tile = {0, 0};
  /// Tile stamp: a rectangle of tiles from the palette (width/height 1 = single tile)
  int stamp_width = 1;
  int stamp_height = 1;
  level::SemanticId semantic = 1;
  level::MaterialId material = 1;
  std::uint8_t collision = level::collision::kSolid;
};

/**
 * @brief The tiles selected in the tileset palette: one bit per tile of one tileset
 *
 * A plain click or drag in the palette selects one tile or a rectangle of them (the paint stamp);
 * ctrl+click toggles tiles and shift+click extends from the anchor, which is how several tiles are
 * picked for a batch metadata edit or for WFC rules taken from the palette layout.
 */
struct TileSelection
{
  /// Tileset the selection belongs to
  std::size_t tileset = 0;
  /// Number of selected tiles
  std::size_t count = 0;
  /// Tile a shift+click extends the selection from (the last one clicked)
  level::TileIndex anchor = 0;
  std::bitset<kMaxTiles> tiles;

  [[nodiscard]] bool has(level::TileIndex tile) const { return tile < kMaxTiles and tiles.test(tile); }
  [[nodiscard]] bool has(std::size_t set, level::TileIndex tile) const { return set == tileset and has(tile); }

  void clear()
  {
    tiles.reset();
    count = 0;
  }

  /// Selects (or deselects) one tile
  void set(level::TileIndex tile, bool on)
  {
    if (tile >= kMaxTiles or tiles.test(tile) == on)
    {
      return;
    }
    tiles.set(tile, on);
    count = on ? (count + 1) : (count - 1);
  }

  /// Moves the selection to tileset <code>set</code>, emptying it if it belonged to another one
  void retarget(std::size_t set)
  {
    if (set != tileset)
    {
      clear();
      tileset = set;
    }
  }
};

/**
 * @brief The tile group chosen in the Tilesets panel, which the paint tool stamps as one thing
 *
 * A group is the third thing a stamp can be, after the palette selection and App::paint's tile or
 * rectangle, and it wins over both -- so which of them is stamped is never ambiguous: choosing a
 * group empties the palette selection, and clicking in the palette clears the group.
 */
struct GroupSelection
{
  /// Tileset the group belongs to
  std::size_t tileset = 0;
  /// Group within that tileset, or kNoGroup for none
  level::GroupId group = level::kNoGroup;

  [[nodiscard]] bool active() const { return group != level::kNoGroup; }

  void clear() { group = level::kNoGroup; }
};

struct View
{
  /// Cell at the viewport's top-left corner
  float x = 0.0F;
  float y = 0.0F;
  /// Screen pixels per cell
  float cell_pixels = 32.0F;
  bool grid = true;
  bool objects = true;
  /// Season whose tile variants are shown
  std::size_t season = 0;
  bool tint = false;
  /// Request to fit the map in the viewport on the next frame
  bool fit = true;
};

/// Atlas images (in the current asset arena) and their GPU textures/average colors
struct Assets
{
  std::size_t current = 0;
  std::array<image::Image, kMaxTileSets> images = {};
  std::array<Texture, kMaxTileSets> textures = {};
  std::array<memory::Vector<image::Rgba>, kMaxTileSets> tile_colors = {};
  /// Status of each atlas load (shown in the tileset panel)
  std::array<Status, kMaxTileSets> status = {};

  [[nodiscard]] render::Atlases atlases(std::size_t count) const
  {
    return render::Atlases{.images = images.data(), .tile_colors = tile_colors.data(), .count = count};
  }
};

/// Per-layer overlay textures (one texel per cell), rebuilt when marked dirty
struct Overlays
{
  std::array<Texture, kMaxLayers> textures = {};
  std::array<bool, kMaxLayers> dirty = {};
  bool all_dirty = true;
};

/// Settings of the procedural generators (persist across runs within a session)
struct Generators
{
  std::size_t height_layer = 0;
  procgen::HeightmapParams heightmap;
  Path seed_image;
  procgen::WatershedParams watershed;
  bool watershed_semantics = true;
  float wfc_water_threshold = 1e-3F;
  int wfc_attempts = 10;
  bool wfc_keep_existing = true;
  bool wfc_use_height = true;
  bool wfc_use_water = true;
  bool wfc_use_semantic = false;
  /// Whether rules taken from the palette layout also forbid what it does not show
  level::LayoutMode wfc_layout = level::LayoutMode::kAllow;
  /// Region for WFC (width 0 = whole map); set with the select tool
  int region_x = 0;
  int region_y = 0;
  int region_w = 0;
  int region_h = 0;

  /// Masked fill (Generate > Fill): the tile layer written, and the layers that mask it
  std::size_t fill_layer = 0;
  std::size_t fill_band_layer = 0;
  std::size_t fill_id_layer = 0;
  bool fill_use_band = true;
  bool fill_use_id = false;
  float fill_lo = 0.0F;
  float fill_hi = 1.0F;
  int fill_id = 0;
  bool fill_only_empty = false;
  /// With two or more tiles selected, scatter them rather than filling with the first
  bool fill_scatter = true;
  /// Whether the fill honours the select tool's region, as WFC does
  bool fill_region = true;
  /// What the last fill did, so the panel can report it
  procgen::FillResult fill_result;

  /// Scattered objects (Generate > Scatter): the profile being used and the layers it reads
  std::size_t scatter_profile = 0;
  std::size_t scatter_layer = 0;
  std::size_t scatter_band_layer = 0;
  std::size_t scatter_id_layer = 0;
  std::size_t scatter_collision_layer = 0;
  /// Name the next profile gets
  Name scatter_name;
  /// What the last scatter did, so the panel can report it
  procgen::ScatterResult scatter_result;

  /// Height steps (Generate > Height steps): the layers it reads and writes
  std::size_t step_height_layer = 0;
  std::size_t step_level_layer = 0;
  std::size_t step_tile_layer = 0;
  std::size_t step_collision_layer = 0;
  procgen::TerraceParams terrace;
  procgen::TidyParams tidy;
  /// Whether cutting the levels tidies them straight after
  bool step_tidy = true;
  procgen::TidyResult tidy_result;
  procgen::StepCollision step_collision = procgen::StepCollision::kBelow;
  bool step_keep_untermed = true;
  /// Whether the tiles are redrawn whenever the levels are recut
  bool step_tile_after_terrace = true;
  procgen::TerraceResult terrace_result;
  procgen::StepFillResult step_result;
};

/// Most layers Edit > Refresh semantics from tiles can choose between
inline constexpr std::size_t kMaxDeriveLayers = 128;

/**
 * @brief What Edit > Refresh semantics from tiles squashes (App::derive)
 *
 * The tile layers are chosen rather than taken as read, because the layers that say what the ground
 * *is* are rarely all of them: the cliff faces of a height-stepped map are drawn over the ground
 * they stand on without replacing what it is. Which of them wins is not a choice -- it follows the
 * level's layer order, so the one on top does (see <code>level::derive_semantics</code>).
 */
struct SemanticDerive
{
  /// The semantic layer written
  std::size_t layer = 0;
  /// Which tile layers are ticked, by layer index
  std::bitset<kMaxDeriveLayers> layers;
  /// Leave the cells no ticked layer gives a semantic type for, rather than clearing them
  bool keep_unmatched = false;
  /// Cells the last run changed, so the menu can report it
  std::size_t changed = 0;
  /// Whether a run has happened, so "0 cells changed" can be told from "not run yet"
  bool ran = false;

  [[nodiscard]] bool has(std::size_t index) const { return index < kMaxDeriveLayers and layers.test(index); }

  [[nodiscard]] std::size_t count() const { return layers.count(); }
};

/// What a delete button removes
enum class DeleteKind : std::uint8_t
{
  kNone,
  kLayer,
  kSemantic,
  kMaterial,
  kSeason,
  kGraph,
  /// A portal, marker or path node (by entity)
  kObject,
  /// A tile property (index within tileset <code>owner</code>)
  kProperty,
  /// An auto-tiling terrain
  kTerrain,
  /// A height-step terrain
  kStepTerrain,
  /// A member level, which leaves its file on disk
  kLevelMember,
  /// A rule set of the project
  kRuleSet,
  /// A tile group (index within tileset <code>owner</code>)
  kTileGroup,
  /// A scatter profile of the project
  kScatterProfile,
  /// One object of scatter profile <code>owner</code>
  kScatterObject,
};

/// A delete waiting for confirmation (or, when confirmed, for the end of the frame)
struct PendingDelete
{
  DeleteKind kind = DeleteKind::kNone;
  std::size_t index = 0;
  std::size_t owner = 0;
  level::Entity entity = level::kNullEntity;
  /// e.g. "material 'snow'"
  FixedString<128> what;
  /// What else changes, e.g. "312 cells and 4 tiles use it; they become 'none'."
  FixedString<256> detail;
  bool confirmed = false;
};

/// Which panels are open
struct Panels
{
  bool layers = true;
  bool tilesets = true;
  bool tile_meta = true;
  bool catalogs = true;
  bool generate = true;
  bool objects = true;
  bool minimap = true;
  bool log = true;
  bool style = false;
  bool demo = false;
  /// The tile adjacency rules window (the rule tiles and their allowed neighbors)
  bool adjacency = false;
  /// The pop-out neighbor editor
  bool neighbors = false;
  /// The project: its member levels, its tilesets and its rule sets
  bool project = true;
};

/// What the modal file dialog is for
enum class FileAction : std::uint8_t
{
  kNone,
  /// A new project: the manifest is saved where the user puts it, and that folder is the root
  kNewProject,
  kOpenProject,
  kSaveProjectAs,
  kOpenLevel,
  kSaveLevel,
  /// A level file brought into the open project as a member (copied into levels/ by the save)
  kAddLevel,
  kLoadTileSet,
  kSaveTileSet,
  kAtlasImage,
  kSeedImage,
  kLoadWfc,
  kSaveWfc,
  kLoadScatter,
  kSaveScatter,
  kLoadPaths,
  kSavePaths,
  kExportPng,
  kExportMinimap,
  /// The selected layer as a PNG, one pixel per cell (what its overlay draws)
  kExportLayerImage,
  kSaveStyle,
  kLoadStyle,
};

/**
 * @brief A file chosen from outside the project, waiting for the user to say what to do with it
 *
 * Everything a project needs should live inside its folder, so the whole folder can be moved,
 * copied or committed as one thing. Choosing an atlas or a seed image from elsewhere therefore asks
 * first rather than storing the outside path quietly. <code>project::plan_copy</code> has already
 * worked out exactly what copying would do, so the prompt can say it before anything is written.
 *
 * A file already inside the tree never gets here: it is simply relativized.
 */
struct PendingImport
{
  /// What wanted the file; kNone when nothing is pending
  FileAction action = FileAction::kNone;
  /// What the user picked, as the dialog gave it
  Path source;
  /// Where a copy would land and what copying would do (see project::plan_copy)
  project::CopyPlan plan;
};

/// The most recently opened or saved levels (File > Open recent), newest first
struct RecentFiles
{
  /// How many paths are kept; the oldest falls off the list
  static constexpr std::size_t kCapacity = 10;
  Path paths[kCapacity];
  std::size_t count = 0;
  /// The list has been read from the user's config
  bool loaded = false;
};

/// State of the file dialog (ImGuiFileDialog; see editor/src/file_dialog.cpp)
struct FileDialog
{
  FileAction action = FileAction::kNone;
  /// Folder the dialog opens in (the last one used)
  Path directory = ".";
  /// Bookmarks have been loaded from the user's config
  bool places_loaded = false;
};

struct App
{
  MemoryConfig config;
  Memory memory;

  /// The open document and the spare one (loads go into the spare and are swapped in on success)
  level::Level* level = nullptr;
  level::Level* spare = nullptr;
  alignas(level::Level) unsigned char level_storage[2][sizeof(level::Level)];

  /// The open level's project, and its spare; swapped together with the document
  project::Project* project = nullptr;
  project::Project* spare_project = nullptr;
  alignas(project::Project) unsigned char project_storage[2][sizeof(project::Project)];

  /// The open level's file, and whether it has unsaved changes
  Path path;
  bool modified = false;

  /**
   * @brief The open project's manifest, and whether the project has unsaved changes
   *
   * Empty while the project is one synthesized around a bare level file, which is what opening a
   * level with no project beside it gives. The two dirty flags are separate because the two things
   * are saved separately: editing a tileset or a rule set is a change to the project, not to the
   * level that happens to be open.
   */
  Path project_path;
  bool project_modified = false;

  /// An asset chosen from outside the project, waiting on the copy-in prompt
  PendingImport pending_import;

  Assets assets;
  Overlays overlays;
  UndoStack undo;

  View view;
  Tool tool = Tool::kPaint;
  Brush brush;
  Paint paint;
  AutoTile auto_tile;
  std::size_t layer = 1;
  std::size_t tileset = 0;
  /// Tile selected in the tileset panel (for metadata editing)
  level::TileIndex tile = 0;
  /// Tiles selected in the palette (batch metadata edits, WFC rules from the palette layout)
  TileSelection selection;
  /// What the batch metadata edit applies to the selected tiles
  level::TileEdit batch;
  /// What Edit > Refresh semantics from tiles reads and writes
  SemanticDerive derive;
  /// The tile group the paint tool stamps, if one is chosen
  GroupSelection group;
  /// Name the next group taken from the palette selection gets
  FixedString<64> group_name;
  /// Folder the next group is filed under ('/'-separated; empty is the top level)
  FixedString<64> group_folder;
  /// Selected object (portal, marker or path node)
  level::Entity entity = level::kNullEntity;
  level::GraphId graph = 0;
  /// First node of an edge being drawn
  level::Entity edge_start = level::kNullEntity;
  /// Path preview: the route from this node to the selected one is highlighted
  level::Entity route_from = level::kNullEntity;

  Generators generators;
  /// Adjacency rules: the tile whose constraints and neighbors are being edited
  level::WfcTileId wfc_tile = 0;
  /// Adjacency rules: hovering a neighbor previews how the two tiles would line up
  bool wfc_preview = true;
  /// What the batch edit applies to the constraints of the selected rule tiles
  level::WfcTileEdit wfc_batch;
  /// Which kind of rules the rules window is editing
  RulesMode rules_mode = RulesMode::kWfc;
  /// Auto-tiling rules: the terrain being edited
  level::TerrainId terrain = 0;
  /// Height-step rules: the terrain being edited
  level::StepTerrainId step_terrain = 0;
  /// What the last auto-tiled stroke did
  AutoTileStatus auto_tile_status;

  render::ComposeOptions export_options;
  int minimap_size = 256;
  image::Image minimap;
  Texture minimap_texture;
  bool minimap_dirty = true;

  Panels panels;
  /// The style as last loaded/saved/applied from a preset ("Revert" in the style editor)
  style::Profile style_reference;
  FileDialog dialog;
  RecentFiles recent;
  JobSlot job;
  JobMemory job_memory;
  PendingDelete pending_delete;
  /// Last error shown in a popup
  FixedString<512> error;
  bool show_error = false;

  /// Exit after this many frames (headless tests); 0 = run until closed
  std::size_t max_frames = 0;
  /// The main loop ends after the current frame
  bool exit = false;
  /// The unsaved-changes prompt is up
  /**
   * @brief What one scripted step records for the next one to check (see app.cpp's smoke())
   *
   * The steps run one per idle frame, with a job in between, so a step that wants to compare
   * against what things were before it has to leave the value somewhere.
   */
  struct SmokeCheck
  {
    std::size_t tilesets = 0;
    std::size_t semantics = 0;
    std::size_t assets = 0;
  } smoke_check;

  /**
   * @brief The start-up prompt is up: open a recent project, or make one
   *
   * Set when the editor starts with nothing named on the command line, which is the one moment the
   * user has not yet said what they are working on. Dismissing it leaves the blank level the editor
   * has always started with, so the prompt is a shortcut and never a gate.
   */
  bool welcome = false;

  bool confirm_exit = false;
  /**
   * @brief The member level a switch is waiting on, while the open one has unsaved changes
   *
   * <code>project::kNoLevel</code> when nothing is pending. Switching goes through the same prompt
   * quitting does, for the same reason: the open document is about to be replaced.
   */
  std::size_t switch_to = static_cast<std::size_t>(-1);
  /// Switch once the save in flight succeeds (as exit_after_save does for quitting)
  bool switch_after_save = false;
  /// Exit once the level save in progress (or about to be chosen in the save dialog) succeeds
  bool exit_after_save = false;
  /// Scripted smoke test: drives generators, undo, save/load and exports through the UI code paths
  bool smoke = false;
  int smoke_step = 0;
  std::size_t frame_count = 0;
};

/// Reserves memory, creates an empty document; optionally opens <code>path</code>
Status initialize(App& app, const MemoryConfig& config, const char* path);

/// Releases everything (GPU textures; memory)
void shutdown(App& app);

/// Runs one frame of UI
void frame(App& app);

/// Shows an error popup (and logs it)
void report(App& app, const Status& status);

/**
 * @brief Asks to quit: exits right away if there are no unsaved changes (or <code>force</code>);
 *        otherwise shows the unsaved-changes prompt (Save / Don't save / Cancel)
 */
void request_exit(App& app, bool force = false);

/// Saves the level (asking where if it has no path yet), then exits if that succeeds
void save_and_exit(App& app);

/**
 * @brief Opens member level <code>index</code> of the open project in place of the open one
 *
 * Immediately when the open level has no unsaved changes; otherwise through the same prompt
 * quitting uses, since the open document is about to be replaced either way.
 */
void request_switch(App& app, std::size_t index);

/// Saves the level (asking where if it has no path yet), then switches if that succeeds
void save_and_switch(App& app);

/// Marks every overlay (and the minimap) as needing an update
void invalidate_view(App& app);

/// Marks one layer's overlay (and the minimap) as needing an update
void invalidate_layer(App& app, std::size_t layer);

/// Starts a new empty level
void new_level(App& app, const level::Settings& settings);

/// Reloads every tileset's atlas image into a fresh asset arena (in parallel)
void reload_atlases(App& app);

/// Season name shown in the view
std::string_view view_season(const App& app);

/// Creates a small demo level: a code-generated atlas (written next to the working directory),
/// catalogs, two seasons, WFC rules and a generated terrain
void demo_level(App& app);

}  // namespace lvl::editor
