/**
 * @copyright 2026-present Brian Cairl
 *
 * @file step_rules.hpp
 *
 * The third kind of tile adjacency rules: terrains keyed on the *difference* in height level
 * between a cell and its neighbors, which is what draws a step up and the hole it steps into.
 *
 * The other two kinds (lvl::level::WfcRules, lvl::level::AutoTileRules) ask only whether a neighbor
 * is the same as this cell. A step cannot be drawn from that: the rim of a plateau and the inside
 * of a pit have the same pattern of "not me" neighbors and must take different tiles, so the rule
 * has to know which side is higher. Each neighbor is therefore one of three states -- level with
 * this cell, below it, or above it -- and the tile follows from the whole eight-neighbor pattern.
 *
 * This mirrors how gft tiles its worlds (<code>getSlopeFromBumpCode</code> in gft's
 * <code>game/src/data/world.cpp</code>), so a level drawn here shows what the game will draw. gft
 * derives the same types from the per-cell height it reads, so what a level really owes the game is
 * the height level of each cell; the tiles are this editor showing its work.
 *
 * A terrain is a set of 18 tiles (lvl::level::StepType) covering one range of height levels, so a
 * map can step from water through sand to grass to rock and each band bring its own cliff.
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

/**
 * @brief The eighteen tiles a step terrain is drawn from, in the order gft's sheets hold them
 *
 * The sheet is three tiles wide and six tall, an *outward* row and an *inward* row alternating, so
 * the values below are simply read across and down. Outward is the rim of ground that stands above
 * its neighbors -- its straight edges, its convex corners, and the flat middle; inward is where
 * that rim turns back on itself around a hole. Both are drawn on the higher cell: a cell with any
 * neighbor above it is not part of a step and takes <code>kFlat</code>.
 *
 * The names are gft's own. They describe the art rather than the geometry, and gft's top row reads
 * right-to-left, so do not read a direction out of them -- the pattern each one answers is in
 * <code>step_type_of</code>.
 */
enum class StepType : std::uint8_t
{
  kOutwardTopRight = 0,
  kOutwardTop = 1,
  kOutwardTopLeft = 2,
  kInwardBottomRight = 3,
  kInwardBottom = 4,
  kInwardBottomLeft = 5,
  kOutwardLeft = 6,
  kFlat = 7,
  kOutwardRight = 8,
  kInwardRight = 9,
  kNone = 10,
  kInwardLeft = 11,
  kOutwardBottomLeft = 12,
  kOutwardBottom = 13,
  kOutwardBottomRight = 14,
  kInwardTopRight = 15,
  kInwardTop = 16,
  kInwardTopLeft = 17,
};

inline constexpr std::size_t kStepTypeCount = 18;

/// The palette block a step terrain is read from: three tiles across, six down
inline constexpr int kStepBlockWidth = 3;
inline constexpr int kStepBlockHeight = 6;

/**
 * @brief How a cell stands against its eight neighbors, two bits each
 *
 * Bit pairs run NW, N, NE, W, E, SW, S, SE -- gft's own order, which is the eight neighbors of a
 * 3x3 read across and down with the middle left out. Each pair is a <code>StepRelation</code>.
 */
using StepCode = std::uint32_t;

enum class StepRelation : std::uint8_t
{
  /// The neighbor is at this cell's level
  kLevel = 0,
  /// The neighbor is lower, so this cell is the top of a step
  kBelow = 1,
  /// The neighbor is higher, so this cell is at the foot of one
  kAbove = 2,
};

/// Neighbors of a cell, in the order <code>StepCode</code> holds them
inline constexpr std::array<Direction, 8> kStepNeighbors = {
  Direction::kNorthWest,
  Direction::kNorth,
  Direction::kNorthEast,
  Direction::kWest,
  Direction::kEast,
  Direction::kSouthWest,
  Direction::kSouth,
  Direction::kSouthEast};

/// Sets one neighbor's relation, replacing whatever it held (the pair is cleared first, so this
/// can be called twice for the same neighbor without the two values running together)
constexpr StepCode step_code_with(StepCode code, std::size_t neighbor, StepRelation relation)
{
  const StepCode cleared = code & ~(static_cast<StepCode>(0x3U) << (2U * neighbor));
  return cleared | (static_cast<StepCode>(relation) << (2U * neighbor));
}

constexpr StepRelation step_relation(StepCode code, std::size_t neighbor)
{
  return static_cast<StepRelation>((code >> (2U * neighbor)) & 0x3U);
}

/**
 * @brief The tile a cell takes, from how it stands against its eight neighbors
 *
 * This is gft's <code>getSlopeFromBumpCode</code> table. Any pattern it does not name -- including
 * every pattern with a neighbor above this cell -- is <code>kFlat</code>, so only the higher side
 * of a step is drawn and the lower side stays plain ground.
 */
StepType step_type_of(StepCode code);

/// One band of height levels and the tiles its ground and its cliffs are drawn from
struct StepTerrain
{
  Name name;
  /// Levels this terrain covers, inclusive; a cell's level picks the first terrain that holds it
  int level_lo = 0;
  int level_hi = 0;
  std::array<TileRef, kStepTypeCount> tiles = {};
};

using StepTerrainId = std::uint16_t;
inline constexpr StepTerrainId kNoStepTerrain = std::numeric_limits<StepTerrainId>::max();

struct StepRules
{
  explicit StepRules(memory::Arena& arena) : terrains{memory::ArenaAllocator<StepTerrain>{arena}} {}

  memory::Vector<StepTerrain> terrains;
};

/// Index of the terrain named <code>name</code>, or <code>kNoStepTerrain</code>
StepTerrainId find_step_terrain(const StepRules& rules, std::string_view name);

/// Appends a terrain (or returns the one already named <code>name</code>)
StepTerrainId add_step_terrain(StepRules& rules, std::string_view name);

void remove_step_terrain(StepRules& rules, StepTerrainId id);

/// The first terrain whose level range holds <code>level</code>, or <code>kNoStepTerrain</code>
StepTerrainId step_terrain_at(const StepRules& rules, int level);

/// The terrain one of whose tiles is <code>tile</code>, or <code>kNoStepTerrain</code>
StepTerrainId step_terrain_of(const StepRules& rules, TileRef tile);

/**
 * @brief Reads a terrain's eighteen tiles out of one palette selection
 *
 * The selection is the 3x6 block gft's sheets use, read across and down. A block of another shape
 * is refused. Returns how many tiles were taken (18, or 0).
 */
std::size_t learn_step_layout(StepTerrain& terrain, const Grid<TileRef>& layout);

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::StepType>
{
  static constexpr std::array<std::string_view, 18> kNames = {
    "outward_top_right",
    "outward_top",
    "outward_top_left",
    "inward_bottom_right",
    "inward_bottom",
    "inward_bottom_left",
    "outward_left",
    "flat",
    "outward_right",
    "inward_right",
    "none",
    "inward_left",
    "outward_bottom_left",
    "outward_bottom",
    "outward_bottom_right",
    "inward_top_right",
    "inward_top",
    "inward_top_left"};
};
