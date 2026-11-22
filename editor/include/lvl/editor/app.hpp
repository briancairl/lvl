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
#include "lvl/procgen/noise.hpp"
#include "lvl/procgen/watershed.hpp"
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

/**
 * @brief Settings of auto-tiled painting (the Auto tool)
 *
 * The tool paints the palette tile (or stamp) as the "centers" of a shape, then re-chooses the band
 * of cells around them from the level's tile adjacency rules, which is how a path's edges and
 * corners or a lake's shore appear by themselves. See lvl::procgen::autotile.
 */
struct AutoTile
{
  /// Cells around the painted ones the rules may re-fill
  int radius = 2;
  /// Relaxation sweeps (each carries the rules one cell further outward)
  int iterations = 4;
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
};

/// What the modal file dialog is for
enum class FileAction : std::uint8_t
{
  kNone,
  kOpenLevel,
  kSaveLevel,
  kLoadTileSet,
  kSaveTileSet,
  kAtlasImage,
  kSeedImage,
  kLoadWfc,
  kSaveWfc,
  kLoadPaths,
  kSavePaths,
  kExportPng,
  kExportMinimap,
  kSaveStyle,
  kLoadStyle,
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

  Path path;
  bool modified = false;

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
  bool confirm_exit = false;
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
