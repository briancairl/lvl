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

namespace
{

/// Resolution a cell's hash is taken at when weights decide the pick
constexpr std::uint64_t kTickets = 1U << 24U;

}  // namespace

FillResult fill_masked(
  const level::TileRef* tiles,
  const float* weights,
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
  // Running total of the weights, so a cell's pick is one walk of the list. Weights that are all
  // zero (or absent) stand for a weight of 1 each, so an untouched tileset scatters evenly and
  // giving every tile the same weight comes to exactly the same thing
  float total = 0.0F;
  if (weights != nullptr)
  {
    for (std::size_t i = 0; i < count; ++i)
    {
      total += std::max(weights[i], 0.0F);
    }
  }
  const bool weighted = (total > 0.0F);
  total = weighted ? total : static_cast<float>(count);
  const auto weight_of = [weights, weighted](std::size_t i) { return weighted ? std::max(weights[i], 0.0F) : 1.0F; };
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
      std::size_t at = 0;
      if (count > 1)
      {
        // Where this cell falls in the running total; the last tile takes any rounding slack
        const float u = static_cast<float>(random::hash(seed, x, y) % kTickets) / static_cast<float>(kTickets) * total;
        float upto = 0.0F;
        at = count - 1;
        for (std::size_t t = 0; t < count; ++t)
        {
          upto += weight_of(t);
          if (u < upto)
          {
            at = t;
            break;
          }
        }
        // A tile of weight 0 can still be landed on by the slack above, so step off it
        while (weight_of(at) <= 0.0F and at > 0)
        {
          --at;
        }
      }
      const level::TileRef tile = tiles[at];
      result.changed += (out.values[i] == tile) ? 0U : 1U;
      out.values[i] = tile;
    }
  }
  return result;
}

}  // namespace lvl::procgen
