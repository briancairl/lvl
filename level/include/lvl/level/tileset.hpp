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
  /// Season name (matched against the level's seasons, so tilesets work across levels)
  Name season;
  TileIndex alternate = 0;
  MaterialId material = kNoMaterial;
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
