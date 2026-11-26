/**
 * @copyright 2026-present Brian Cairl
 *
 * @file tileset.hpp
 *
 * A tileset is an atlas image sliced into equally-sized tiles (the slicer parameters), plus
 * metadata per tile: semantic type, material, collision mask, free-form properties, and seasonal
 * alternates (a different tile and/or material to use during a named season).
 *
 * A tileset also carries named tile groups: a house, a tree or a rock drawn across several tiles,
 * stamped onto the map as one thing. They belong to the tileset rather than to a level or a rule
 * set because every tile they name is one of that tileset's -- so a group travels with the atlas it
 * was cut from, is written by the standalone tileset file a project saves, and goes away with the
 * tileset without any level or rule set having to be renumbered.
 */
#pragma once

// C++ Standard Library
#include <cstdint>
#include <limits>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/types.hpp"

namespace lvl::level
{

/// How an atlas image is cut into tiles
struct SliceParams
{
  /// Tile size, in pixels
  int tile_width = 16;
  int tile_height = 16;
  /// Pixels skipped at the image's left/top edge
  int margin_x = 0;
  int margin_y = 0;
  /// Pixels between adjacent tiles
  int spacing_x = 0;
  int spacing_y = 0;
  /// Grid of tiles to take (0 = as many as fit the image)
  int columns = 0;
  int rows = 0;
};

/// Pixel rectangle of one tile within its atlas
struct TileRect
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

/// Metadata of one tile
struct TileInfo
{
  /// Optional name ("grass_a", "cliff_nw", ...)
  Name name;
  SemanticId semantic = kNoSemantic;
  MaterialId material = kNoMaterial;
  std::uint8_t collision = collision::kNone;
  /// Excluded from palettes/procedural generation
  bool hidden = false;
  /// How often a scattered fill picks this tile, relative to the others it is scattered with
  /// (see procgen::fill_masked); 0 keeps it out of a scatter without hiding it
  float scatter_weight = 1.0F;
};

/// A free-form key/value property of a tile ("damage" = "5", "animated" = "water", ...)
struct TileProperty
{
  TileIndex tile = 0;
  Name key;
  Name value;
};

/// During season <code>season</code>, tile <code>tile</code> is drawn as <code>alternate</code> (same tileset)
/// and, if set, uses <code>material</code> instead of its own
struct SeasonalVariant
{
  TileIndex tile = 0;
  /// Season name (matched against the project's seasons, so tilesets work across projects)
  Name season;
  TileIndex alternate = 0;
  MaterialId material = kNoMaterial;
};

/// Index of a tile group within a tileset
using GroupId = std::uint16_t;

inline constexpr GroupId kNoGroup = std::numeric_limits<GroupId>::max();

/// A group cell that holds no tile: stamping it leaves whatever is under it alone
inline constexpr TileIndex kNoTileIndex = std::numeric_limits<TileIndex>::max();

/// Widest/tallest a group may be, in cells
inline constexpr int kMaxGroupExtent = 64;

/**
 * @brief A named group of tiles stamped onto the map as one thing: a house, a tree, a rock
 *
 * The footprint is a rectangle, and its cells are <code>size.count()</code> tile indices in
 * row-major order living in the tileset's one flat <code>group_cells</code> table, which this entry
 * spans into -- the same shape the project's level index uses for its object names, so adding a
 * group does not leave the last block behind in a bump arena. A cell holding
 * <code>kNoTileIndex</code> is a hole: a tree's bounding box has corners the tree does not fill,
 * and stamping must leave what is under them alone rather than erase it.
 */
struct TileGroup
{
  /// Unique within the tileset; this is what a scatter profile names
  Name name;
  /**
   * @brief Folder this group is filed under, '/'-separated ("nature/trees"); empty is the top level
   *
   * Organisation only, so that a tileset with a hundred groups can be picked through as a tree. It
   * is deliberately **not** part of the group's identity: a group is found by name
   * (<code>find_group</code>) and a scatter profile names it, so re-filing a group does not break
   * what points at it.
   */
  Name folder;
  /// Footprint, in cells
  Extents size;
  /// Offset of this group's first cell in <code>TileSet::group_cells</code>
  std::uint32_t first = 0;
};

/**
 * @brief A metadata change for one or more tiles: only the fields whose flag is set are applied
 *
 * What the editor's batch edit of a palette selection does to every selected tile.
 */
struct TileEdit
{
  bool set_semantic = false;
  SemanticId semantic = kNoSemantic;
  bool set_material = false;
  MaterialId material = kNoMaterial;
  bool set_collision = false;
  std::uint8_t collision = collision::kNone;
  bool set_hidden = false;
  bool hidden = false;
  bool set_scatter_weight = false;
  float scatter_weight = 1.0F;
  /// Sets property <code>property_key</code> to <code>property_value</code>
  bool set_property = false;
  /// Removes property <code>property_key</code>
  bool clear_property = false;
  Name property_key;
  Name property_value;
};

struct TileSet
{
  explicit TileSet(memory::Arena& arena) :
      tiles{memory::ArenaAllocator<TileInfo>{arena}},
      properties{memory::ArenaAllocator<TileProperty>{arena}},
      seasonal{memory::ArenaAllocator<SeasonalVariant>{arena}},
      groups{memory::ArenaAllocator<TileGroup>{arena}},
      group_cells{memory::ArenaAllocator<TileIndex>{arena}}
  {}

  Name name;
  /// Atlas image path (relative paths are resolved against the working directory)
  Path image;
  SliceParams slice;
  /// Atlas size in pixels (known once the image has been loaded; used to slice)
  int image_width = 0;
  int image_height = 0;
  /// One entry per sliced tile
  memory::Vector<TileInfo> tiles;
  memory::Vector<TileProperty> properties;
  memory::Vector<SeasonalVariant> seasonal;
  /// Named multi-tile stamps; each one spans into <code>group_cells</code>
  memory::Vector<TileGroup> groups;
  /// Every group's cells, back to back in the order the groups are listed
  memory::Vector<TileIndex> group_cells;
};

/// A project's tilesets, indexed by <code>TileSetId</code>; a level names them, the project owns them
using TileSets = memory::Vector<TileSet>;

/// Number of tile columns/rows <code>params</code> yields for an image of the given size
Extents slice_grid(const SliceParams& params, int image_width, int image_height);

/// Pixel rectangle of tile <code>index</code> under <code>params</code>
TileRect tile_rect(const SliceParams& params, int image_width, int image_height, TileIndex index);

/// Re-slices <code>tileset</code> for its current image size, keeping metadata of tiles that remain
void reslice(TileSet& tileset);

/// Tile <code>tile</code> as drawn during <code>season</code> (itself if it has no variant then)
TileIndex seasonal_tile(const TileSet& tileset, TileIndex tile, std::string_view season);

/// Material of <code>tile</code> during <code>season</code>
MaterialId seasonal_material(const TileSet& tileset, TileIndex tile, std::string_view season);

/**
 * @brief Precomputes the drawn tile for every tile of <code>tileset</code> during <code>season</code>
 *
 * Fills <code>out</code> (size = tile count) so renderers do one lookup per cell.
 */
void resolve_season(const TileSet& tileset, std::string_view season, memory::Vector<TileIndex>& out);

/// Sets (or replaces) a seasonal variant; <code>alternate == tile</code> and no material removes it
void set_seasonal_variant(TileSet& tileset, const SeasonalVariant& variant);

/// Value of property <code>key</code> of <code>tile</code>, or nullptr
const Name* find_property(const TileSet& tileset, TileIndex tile, std::string_view key);

/// Sets (replacing it, if there is one) property <code>key</code> of <code>tile</code>
void set_property(TileSet& tileset, TileIndex tile, std::string_view key, std::string_view value);

/// Removes property <code>key</code> of <code>tile</code>, if there is one
void remove_property(TileSet& tileset, TileIndex tile, std::string_view key);

/// Index of the group named <code>name</code>, or <code>kNoGroup</code>
GroupId find_group(const TileSet& tileset, std::string_view name);

/// First of group <code>group</code>'s cells, or nullptr if its span does not fit the cell table
const TileIndex* group_cells_of(const TileSet& tileset, const TileGroup& group);

/// Tile at <code>(x, y)</code> of <code>group</code>, or <code>kNoTileIndex</code> for a hole or an outside cell
TileIndex group_cell(const TileSet& tileset, const TileGroup& group, int x, int y);

/**
 * @brief Adds the group <code>name</code>, or replaces the one of that name, from row-major <code>cells</code>
 *
 * <code>cells</code> holds <code>size.count()</code> entries; <code>kNoTileIndex</code>, and any
 * index past the tileset's last tile, is a hole. Returns the group's index, or
 * <code>kNoGroup</code> if the name is empty, the footprint is empty or larger than
 * <code>kMaxGroupExtent</code>, every cell is a hole, or the tileset already holds
 * <code>kNoGroup</code> groups.
 *
 * Replacing a group keeps the folder it was filed under: re-taking it from the palette is a change
 * to its tiles and should not un-file it.
 */
GroupId set_group(TileSet& tileset, std::string_view name, Extents size, const TileIndex* cells);

/// Removes group <code>index</code>, compacting the cell table the later groups span into
void remove_group(TileSet& tileset, GroupId index);

/**
 * @brief Makes <code>tileset</code>'s groups consistent with its cell table and its tile count
 *
 * A cell naming a tile the tileset does not have becomes a hole. The cell table is always written
 * packed and in the order the groups are listed, so a file whose groups do not span it exactly
 * cannot say which cells are whose -- and a group reading another group's cells is worse than a
 * tileset with no groups, so all of them are dropped. Returns how many.
 */
std::size_t validate_groups(TileSet& tileset);

/// Applies <code>edit</code> to <code>count</code> tiles, skipping indices past the end; returns how many were edited
std::size_t apply_edit(TileSet& tileset, const TileEdit& edit, const TileIndex* tiles, std::size_t count);

template <typename S> void serialize(S& s, SliceParams& p)
{
  s.value4b(p.tile_width);
  s.value4b(p.tile_height);
  s.value4b(p.margin_x);
  s.value4b(p.margin_y);
  s.value4b(p.spacing_x);
  s.value4b(p.spacing_y);
  s.value4b(p.columns);
  s.value4b(p.rows);
}

bool load(json::Reader& r, const Json& j, SliceParams& p);
void save(Json& j, const SliceParams& p);

}  // namespace lvl::level
