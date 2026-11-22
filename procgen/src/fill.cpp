/**
 * @copyright 2026-present Brian Cairl
 *
 * @file fill.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/core/random.hpp"
#include "lvl/procgen/fill.hpp"

namespace lvl::procgen
{

FillResult fill_masked(
  const level::TileRef* tiles,
  std::size_t count,
  const FillMask& mask,
  std::uint64_t seed,
  level::Grid<level::TileRef>& out)
{
  FillResult result;
  const level::Extents e = out.extents;
  if (count == 0 or e.count() == 0)
  {
    return result;
  }
  LVL_ASSERT_TRUE(mask.band == nullptr or mask.band->extents == e) << "the band layer is a different size";
  LVL_ASSERT_TRUE(mask.ids == nullptr or mask.ids->extents == e) << "the id layer is a different size";
  // The whole grid, unless a region narrows it
  const bool whole = (mask.region.width <= 0 or mask.region.height <= 0);
  const int x0 = whole ? 0 : std::max(mask.region.x, 0);
  const int y0 = whole ? 0 : std::max(mask.region.y, 0);
  const int x1 = whole ? e.width : std::min(mask.region.x + mask.region.width, e.width);
  const int y1 = whole ? e.height : std::min(mask.region.y + mask.region.height, e.height);
  // A band given the wrong way round matches nothing, which is what the numbers say
  for (int y = y0; y < y1; ++y)
  {
    for (int x = x0; x < x1; ++x)
    {
      const std::size_t i = e.index(x, y);
      if (mask.band != nullptr)
      {
        const float v = mask.band->values[i];
        if (v < mask.lo or v > mask.hi)
        {
          continue;
        }
      }
      if (mask.ids != nullptr and mask.ids->values[i] != mask.id)
      {
        continue;
      }
      if (mask.only_empty and !out.values[i].empty())
      {
        continue;
      }
      ++result.matched;
      // By cell rather than in order, so the scatter does not shift when the mask changes
      const std::size_t at =
        (count < 2) ? 0 : static_cast<std::size_t>(random::hash(seed, x, y) % static_cast<std::uint64_t>(count));
      const level::TileRef tile = tiles[at];
      result.changed += (out.values[i] == tile) ? 0U : 1U;
      out.values[i] = tile;
    }
  }
  return result;
}

}  // namespace lvl::procgen
