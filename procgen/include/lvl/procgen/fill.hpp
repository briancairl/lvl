/**
 * @copyright 2026-present Brian Cairl
 *
 * @file fill.hpp
 *
 * Whole-map tile fills masked by the level's other layers: every cell whose height falls inside a
 * band, and whose semantic layer holds a given ground type, takes one of a set of tiles.
 *
 * This is the counterpart of painting by hand. What gets filled is not a brush stroke but whatever
 * the layers the level already has say it should be -- the grass that belongs between two
 * elevations, the sand wherever the semantic layer says "beach" -- so a fill follows the terrain
 * when the terrain is regenerated. With several tiles it scatters them, which is how a handful of
 * grass variants cover a meadow without the player seeing one tile repeat.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/level/layer.hpp"
#include "lvl/level/types.hpp"
#include "lvl/procgen/wfc.hpp"

namespace lvl::procgen
{

/**
 * @brief Which cells a fill may write
 *
 * Every test is optional and a cell must pass all of them. The two layer tests are by plane rather
 * than by layer kind, so the band may be a heightmap or water depth, and the ids a semantic or a
 * material layer; both planes must have the same extents as the tiles being filled.
 */
struct FillMask
{
  /// Scalar layer whose value must fall in [<code>lo</code>, <code>hi</code>] (null: no band test)
  const level::ScalarGrid* band = nullptr;
  float lo = 0.0F;
  float hi = 1.0F;
  /// Id layer which must hold <code>id</code> (null: no id test)
  const level::IdGrid* ids = nullptr;
  std::uint16_t id = 0;
  /// Cells outside this rectangle are left alone (zero width or height: the whole grid)
  Region region;
  /// Only write where the cell has no tile, so what is already drawn survives
  bool only_empty = false;
};

struct FillResult
{
  /// Cells the mask let through
  std::size_t matched = 0;
  /// Cells whose tile the fill changed
  std::size_t changed = 0;
};

/**
 * @brief Writes one of <code>tiles</code> into every cell <code>mask</code> lets through
 *
 * With one tile, every matched cell takes it. With several, each cell takes one by a hash of its
 * position and <code>seed</code>, so the scatter is stable: filling twice over writes the same
 * tiles, re-running after an edit leaves the rest of the map where it was, and only the seed moves
 * them. A <code>count</code> of 0 writes nothing.
 */
FillResult fill_masked(
  const level::TileRef* tiles,
  std::size_t count,
  const FillMask& mask,
  std::uint64_t seed,
  level::Grid<level::TileRef>& out);

}  // namespace lvl::procgen
