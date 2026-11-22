/**
 * @copyright 2026-present Brian Cairl
 *
 * @file autotile.hpp
 *
 * Auto-tiling: after cells have been painted with one tile, re-fill the band of cells around them
 * so the level's tile adjacency rules (lvl::level::WfcRules) hold again.
 *
 * This is what the editor's Auto tool paints with. The painted cells are the middle of the shape the
 * user drew (a path's centre, a lake's water); the cells around them are given the tiles that belong
 * at the border, which is how a path's edges and corners or a lake's shore appear by themselves.
 * Painting more middle runs it again and the border re-tiles around the larger shape.
 *
 * There are two entry points, one per kind of tile adjacency rule the level holds:
 *
 * - <code>paint_terrain</code> uses the auto-tiling rules (lvl::level::AutoTileRules), which key a
 *   terrain's tiles on the pattern of like neighbors. This is the one that prescribes edges and
 *   inner/outer corners, because a pattern names exactly one tile.
 * - <code>autotile</code> uses the WFC adjacency rules, scoring each rule tile against its
 *   neighbors' adjacencies. Those rules cannot tell one corner from another (see
 *   auto_tile_rules.hpp), so this only tidies a band up against hard constraints; it is the
 *   fallback for a level whose tiles have no terrain.
 *
 * Both are local, greedy and deterministic rather than solvers: they never fail, and they are cheap
 * enough to run on every mouse-move of a stroke. Whole regions are still generated with
 * lvl::procgen::solve.
 */
#pragma once

// C++ Standard Library
#include <cstddef>

// LVL
#include "lvl/core/memory.hpp"
#include "lvl/level/auto_tile_rules.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"
#include "lvl/level/wfc_rules.hpp"

namespace lvl::procgen
{

struct AutoTileParams
{
  /// Cells around a painted cell that may be re-filled (0 = only the painted cells, so nothing)
  int radius = 2;
  /// Relaxation sweeps over those cells (each spreads the rules one cell further outward)
  int iterations = 4;
  /// How much a painted cell's constraint counts against an ordinary neighbor's
  float locked_weight = 4.0F;
};

struct AutoTileResult
{
  /// Cells the re-fill changed
  std::size_t changed = 0;
  /// Cells considered (within <code>radius</code> of a painted cell, and not locked)
  std::size_t considered = 0;
  /// Cells where no rule tile satisfies every neighbor (the rules cannot express the shape drawn)
  std::size_t unresolved = 0;
};

struct TerrainPaintResult
{
  /// Cells the terrain covers and which were given a tile
  std::size_t covered = 0;
  /// Cells whose tile changed
  std::size_t changed = 0;
  /// Cells whose pattern the terrain has no tile for at all (it has no variants)
  std::size_t missing = 0;
};

/**
 * @brief Paints a terrain through <code>painted</code> and tiles its border from the rules
 *
 * The painted cells, together with the cells already holding the terrain's fully-surrounded tile
 * (its every tile, when <code>expand</code> is 0), are the terrain's *core*. The terrain covers the
 * core grown by <code>expand</code>, and every covered cell near the stroke takes the tile for the
 * pattern of covered neighbors around it. So with the default <code>expand</code> of 1, painting
 * one cell lays down the whole nine-slice: the cell itself fully surrounded, the ring around it
 * edges and outer corners. Painting more core re-tiles the ring, which is how an edge becomes a
 * straight run as a path grows.
 *
 * Because the covered area follows from the core alone, a stroke never has to remember what the
 * previous one did. Cells outside the grid count as covered, so a terrain running off the map has
 * no border drawn along the map's edge. <code>scratch</code> holds a few arrays over the cells
 * around the stroke, and is rewound.
 */
TerrainPaintResult paint_terrain(
  const level::AutoTileRules& rules,
  level::TerrainId terrain,
  const level::Cell* painted,
  std::size_t count,
  level::Grid<level::TileRef>& tiles,
  memory::Arena& scratch);

/**
 * @brief Re-fills the cells around <code>painted</code> so <code>rules</code> hold around them
 *
 * The painted cells, and every cell holding one of the tiles they hold, are locked: they constrain
 * their neighbors but are never rewritten. Of the remaining cells within <code>radius</code>
 * (Chebyshev) of a painted cell, those holding a rule tile or nothing are re-chosen: each takes the
 * rule tile that satisfies the most of its neighbors' adjacencies, counting a locked neighbor
 * <code>locked_weight</code> times an ordinary one, and keeping the tile already there on a tie (so
 * a finished area is stable and the result does not depend on where the stroke started). Corner
 * adjacencies count only when <code>WfcRules::diagonals</code> is set. A cell holding a tile the
 * rules do not know is left alone, so hand-placed decoration survives.
 *
 * Cells are visited nearest-first and the sweeps stop as soon as one changes nothing.
 * <code>scratch</code> holds an 8*n*n adjacency table (n = rule tiles) plus a few arrays over the
 * grid, and is rewound.
 */
AutoTileResult autotile(
  const level::WfcRules& rules,
  const level::Cell* painted,
  std::size_t count,
  const AutoTileParams& params,
  level::Grid<level::TileRef>& tiles,
  memory::Arena& scratch);

}  // namespace lvl::procgen
