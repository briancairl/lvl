/**
 * @copyright 2026-present Brian Cairl
 *
 * @file autotile.hpp
 *
 * Auto-tiling: after cells have been painted with one tile, re-fill the band of cells around them
 * so the level's tile adjacency rules (lvl::level::WfcRules) hold again.
 *
 * This is what the editor's Auto tool paints with. The painted cells are the "centers" the user
 * drew (a path's middle, a lake's water); the cells around them are re-chosen from the rule tiles
 * so that each sits legally next to its neighbors, which is how the edge and corner tiles of a path
 * or the shore of a lake appear by themselves. Painting more centers runs it again, and the band
 * re-fills around the larger shape: cells holding a painted tile are never overwritten, so earlier
 * strokes keep their centers.
 *
 * Unlike lvl::procgen::solve (full wave-function collapse over a region), this is a local, greedy
 * and deterministic repair: it never fails, and it leaves a cell alone when no rule tile fits
 * better than what is there. That makes it usable on every mouse-move of a stroke.
 */
#pragma once

// C++ Standard Library
#include <cstddef>

// LVL
#include "lvl/core/memory.hpp"
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
