/**
 * @copyright 2026-present Brian Cairl
 *
 * @file tileset.hpp
 *
 * A tileset is an atlas image sliced into equally-sized tiles (the slicer parameters), plus
 * metadata per tile: semantic type, material, collision mask, free-form properties, and seasonal
 * alternates (a different tile and/or material to use during a named season).
 */
#pragma once

// C++ Standard Library
#include <cstdint>

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
      seasonal{memory::ArenaAllocator<SeasonalVariant>{arena}}
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
