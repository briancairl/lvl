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

/// Tile a terrain uses where its neighbors are <code>mask</code>
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

/// The variant for exactly <code>mask</code>, or null
const AutoVariant* find_variant(const AutoTerrain& terrain, NeighborMask mask);

/// Sets the tile for <code>mask</code> (an empty tile removes the variant)
void set_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile);

/**
 * @brief The tile a terrain uses where its neighbors are <code>mask</code>
 *
 * The exact variant if there is one, else the variant whose mask agrees with the most neighbors (so
 * an incomplete set of tiles still fills every cell), else an empty tile.
 */
TileRef resolve(const AutoTerrain& terrain, NeighborMask mask);

/// The patterns <code>neighborhood</code> distinguishes, in ascending order, in <code>arena</code>
memory::Vector<NeighborMask> patterns(Neighborhood neighborhood, memory::Arena& arena);

/**
 * @brief Takes a terrain's tiles from a block of tiles laid out as they appear on the map
 *
 * Three shapes are understood, which between them are how tilesets ship these tiles:
 *
 * - 3x3: a nine-slice, read as the corner, edge and middle tiles of a large patch of terrain, which
 *   gives the four sides, the four *outer* corners and the fully-surrounded tile;
 * - 2x2: the four *inner* corners, where the terrain surrounds the cell on every side but one
 *   corner (top-left is the one missing its north-west neighbor);
 * - 1x1: the fully-surrounded tile alone.
 *
 * Existing variants for the patterns the layout covers are replaced; the others are kept, so a 3x3
 * block and then a 2x2 block build up the usual thirteen-tile set.
 *
 * @return how many variants were set (0 if the layout is not one of those shapes)
 */
std::size_t learn_from_layout(AutoTerrain& terrain, const Grid<TileRef>& layout);

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::Neighborhood>
{
  static constexpr std::array<std::string_view, 2> kNames = {"sides", "blob"};
};
