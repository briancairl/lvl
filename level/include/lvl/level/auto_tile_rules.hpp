/**
 * @copyright 2026-present Brian Cairl
 *
 * @file auto_tile_rules.hpp
 *
 * Auto-tiling rules: which tile a *terrain* uses for each pattern of like neighbors.
 *
 * These are the level's second kind of tile adjacency rules, beside WfcRules, and they answer a
 * different question. WFC adjacency is a permission relation on pairs, per direction: the tiles a
 * cell may hold are the conjunction of independent per-neighbor constraints. Prescribing an inner
 * or outer corner needs a predicate over the *joint* pattern of the eight neighbors ("this tile goes
 * exactly where the terrain is north and east, and nowhere else"), which no product of per-direction
 * relations can express: in a real tileset the north-east and north-west outer corners and the north
 * edge all permit the same tiles on every side. WFC is also deliberately permissive and
 * weight-random, while auto-tiling has to be a function - one pattern, one tile.
 *
 * So a terrain keys its tiles on a NeighborMask: one bit per Direction, set where the neighbor holds
 * the same terrain. That is Wang/blob tiling. <code>Neighborhood</code> says which bits count, and
 * <code>resolve</code> falls back to the nearest mask so a partial set of tiles still fills in.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"

namespace lvl::level
{

/// Which of a cell's neighbors hold the same terrain: one bit per Direction (sides 0..3, corners 4..7)
using NeighborMask = std::uint8_t;

inline constexpr NeighborMask kNoNeighbors = 0x00;
/// The four sides
inline constexpr NeighborMask kSideNeighbors = 0x0F;
/// The four corners
inline constexpr NeighborMask kCornerNeighbors = 0xF0;
/// Every neighbor (a cell well inside the terrain)
inline constexpr NeighborMask kAllNeighbors = 0xFF;

constexpr NeighborMask mask_of(Direction d) { return static_cast<NeighborMask>(1U << static_cast<int>(d)); }

constexpr bool has(NeighborMask mask, Direction d) { return (mask & mask_of(d)) != 0U; }

/// The two sides a corner lies between (north-east: north and east)
constexpr std::array<Direction, 2> corner_sides(Direction corner)
{
  switch (corner)
  {
  case Direction::kNorthEast:
    return {Direction::kNorth, Direction::kEast};
  case Direction::kSouthEast:
    return {Direction::kSouth, Direction::kEast};
  case Direction::kSouthWest:
    return {Direction::kSouth, Direction::kWest};
  case Direction::kNorthWest:
  default:
    return {Direction::kNorth, Direction::kWest};
  }
}

/// Which neighbors a terrain's tiles are keyed on
enum class Neighborhood : std::uint8_t
{
  /// The four sides only: 16 patterns, which a nine-slice set of tiles covers
  kSides,
  /// The sides, plus a corner wherever both of its sides are also terrain: 47 patterns
  kBlob,
};

/**
 * @brief A tile a terrain uses where its neighbors are <code>mask</code>
 *
 * Several variants may share a mask: they are that pattern's pool, and a cell takes one of them by
 * position, so a terrain's middle can be a handful of tiles that scatter over an area.
 */
struct AutoVariant
{
  NeighborMask mask = kNoNeighbors;
  TileRef tile;
};

/**
 * @brief One terrain: a named set of tiles keyed on the pattern of like neighbors
 *
 * <code>expand</code> is how far beyond what the user paints the terrain reaches. At 1 (the default)
 * the painted cells come out as the fully-surrounded tile and the ring around them takes the edges
 * and corners, which is "draw the middle of the path and the sides fill in around it". At 0 the
 * painted cells themselves take the edge tiles, which is how a blob tileset is usually painted.
 */
struct AutoTerrain
{
  explicit AutoTerrain(memory::Arena& arena) : variants{memory::ArenaAllocator<AutoVariant>{arena}} {}

  Name name;
  Neighborhood neighborhood = Neighborhood::kBlob;
  /// Cells beyond the painted ones the terrain also covers (0 or 1; more widens the border)
  int expand = 1;
  memory::Vector<AutoVariant> variants;
};

/// Index of a terrain within AutoTileRules::terrains
using TerrainId = std::uint16_t;

inline constexpr TerrainId kNoTerrain = std::numeric_limits<TerrainId>::max();

struct AutoTileRules
{
  explicit AutoTileRules(memory::Arena& arena) : terrains{memory::ArenaAllocator<AutoTerrain>{arena}} {}

  memory::Vector<AutoTerrain> terrains;
};

/// Index of the terrain named <code>name</code>, or <code>kNoTerrain</code>
TerrainId find_terrain(const AutoTileRules& rules, std::string_view name);

/// Adds a terrain (or returns the one already named <code>name</code>)
TerrainId add_terrain(AutoTileRules& rules, memory::Arena& arena, std::string_view name);

/// Removes a terrain
void remove_terrain(AutoTileRules& rules, TerrainId id);

/// The terrain <code>tile</code> is one of the tiles of, or <code>kNoTerrain</code>
TerrainId terrain_of(const AutoTileRules& rules, TileRef tile);

/// Drops the bits <code>neighborhood</code> does not distinguish (corners whose sides are not both set)
NeighborMask reduce(NeighborMask mask, Neighborhood neighborhood);

/// The first variant for exactly <code>mask</code>, or null
const AutoVariant* find_variant(const AutoTerrain& terrain, NeighborMask mask);

/// How many tiles <code>mask</code>'s pool holds
std::size_t variant_count(const AutoTerrain& terrain, NeighborMask mask);

/// Makes <code>tile</code> the only tile for <code>mask</code> (an empty tile clears the pattern)
void set_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile);

/// Adds another tile to <code>mask</code>'s pool (ignoring one it already holds)
void add_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile);

/**
 * @brief The tile a terrain uses where its neighbors are <code>mask</code>
 *
 * The pattern itself if the terrain has it, else the nearest one: the pattern agreeing with the
 * most of the four sides, and among those the most corners, so an incomplete set of tiles still
 * fills every cell and never answers an edge with a tile meant for another edge. A pattern whose
 * pool holds several tiles takes one of them by <code>key</code> (the painter passes a hash of the
 * cell, so a cell keeps its tile however often the area is repainted). Empty if the terrain has no
 * tiles at all.
 */
TileRef resolve(const AutoTerrain& terrain, NeighborMask mask, std::uint64_t key = 0);

/// The patterns <code>neighborhood</code> distinguishes, in ascending order, in <code>arena</code>
memory::Vector<NeighborMask> patterns(Neighborhood neighborhood, memory::Arena& arena);

/**
 * @brief Takes a terrain's tiles from tiles laid out as they appear on the map
 *
 * The layout is read for the shapes tilesets ship these tiles in, in this order:
 *
 * - a 3x3 block: a nine-slice, read as the corner, edge and middle tiles of a large patch of
 *   terrain, which gives the four sides, the four *outer* corners and the fully-surrounded tile;
 * - a 2x2 block among the tiles the nine-slice did not take: the four *inner* corners, where the
 *   terrain surrounds the cell on every side but one corner (top-left is the one missing its
 *   north-west neighbor);
 * - anything still left over beside a nine-slice: more tiles for the fully-surrounded pattern's
 *   pool, which a painted area scatters between. A layout of one tile is that pattern too; a
 *   layout holding neither block nor a single tile is no shape this understands.
 *
 * So the whole thirteen-tile set is one selection -- a 3x3 block and a 2x2 block side by side --
 * and any further tiles selected with them become alternative middles. Each block replaces the
 * patterns it covers and leaves the others alone, so the blocks can also be taken one at a time.
 *
 * @return how many tiles were taken (0 if the layout holds none of those shapes, or is improbably
 *         large: a layout is a handful of palette tiles, and above 1024 cells it is refused)
 */
std::size_t learn_from_layout(AutoTerrain& terrain, const Grid<TileRef>& layout);

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::Neighborhood>
{
  static constexpr std::array<std::string_view, 2> kNames = {"sides", "blob"};
};
