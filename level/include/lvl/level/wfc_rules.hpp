/**
 * @copyright 2026-present Brian Cairl
 *
 * @file wfc_rules.hpp
 *
 * Constraints for wave-function-collapse generation (lvl::procgen::wfc):
 *
 * - which tiles take part, with relative weights;
 * - which tiles may sit next to which, per direction (adjacency);
 * - per-tile constraints against other layers: a height band, a water requirement and an optional
 *   semantic type, so generation can be seeded/steered by a heightmap, rivers, or hand-painted
 *   semantics.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"

namespace lvl::level
{

/// How a WFC tile relates to the water layer
enum class WaterRule : std::uint8_t
{
  /// No constraint
  kAny,
  /// Only where there is no water
  kDry,
  /// Only where there is water
  kWet,
  /// Only on cells next to (but not in) water
  kShore,
};

/// Index of a tile within WfcRules::tiles
using WfcTileId = std::uint16_t;

struct WfcTile
{
  TileRef tile;
  /// Relative frequency
  float weight = 1.0F;
  /// Allowed band of the (normalized, 0..1) height layer
  float height_min = 0.0F;
  float height_max = 1.0F;
  WaterRule water = WaterRule::kAny;
  /// Only where the semantic layer has this id (kNoSemantic = no constraint)
  SemanticId semantic = kNoSemantic;
};

/// "Tile <code>b</code> may be the <code>direction</code> neighbor of tile <code>a</code>"
/// (the reverse relation, a as b's opposite(direction) neighbor, is implied)
struct WfcAdjacency
{
  WfcTileId a = 0;
  WfcTileId b = 0;
  Direction direction = Direction::kEast;

  friend constexpr bool operator==(const WfcAdjacency& lhs, const WfcAdjacency& rhs)
  {
    return lhs.a == rhs.a and lhs.b == rhs.b and lhs.direction == rhs.direction;
  }
};

struct WfcRules
{
  explicit WfcRules(memory::Arena& arena) :
      tiles{memory::ArenaAllocator<WfcTile>{arena}}, adjacency{memory::ArenaAllocator<WfcAdjacency>{arena}}
  {}

  Name name;
  memory::Vector<WfcTile> tiles;
  memory::Vector<WfcAdjacency> adjacency;
};

/// Index of <code>tile</code> in <code>rules</code>, or <code>rules.tiles.size()</code>
WfcTileId find_tile(const WfcRules& rules, TileRef tile);

/// Adds <code>tile</code> (if absent); returns its index
WfcTileId add_tile(WfcRules& rules, TileRef tile);

/// Removes a tile and every adjacency involving it (renumbering later tiles)
void remove_tile(WfcRules& rules, WfcTileId id);

/// Normalizes an adjacency to its canonical stored form (direction east or south)
WfcAdjacency canonical(WfcAdjacency adjacency);

/// True if <code>b</code> may be <code>direction</code> of <code>a</code>
bool allowed(const WfcRules& rules, WfcTileId a, WfcTileId b, Direction direction);

/// Allows or forbids <code>b</code> as <code>direction</code> neighbor of <code>a</code>
void set_allowed(WfcRules& rules, WfcTileId a, WfcTileId b, Direction direction, bool allow);

/**
 * @brief Learns tiles and adjacencies from an example tile plane
 *
 * Every distinct non-empty tile found is added (weight = occurrence count), and every observed
 * neighboring pair is allowed. Existing rules are kept (merged).
 */
void learn(WfcRules& rules, const Grid<TileRef>& example);

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::WaterRule>
{
  static constexpr std::array<std::string_view, 4> kNames = {"any", "dry", "wet", "shore"};
};
