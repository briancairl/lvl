/**
 * @copyright 2026-present Brian Cairl
 *
 * @file scatter.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/core/random.hpp"
#include "lvl/procgen/scatter.hpp"

namespace lvl::procgen
{
namespace
{

using namespace level;

/// An object resolved to the group it places, or skipped
struct Resolved
{
  const TileSet* tileset = nullptr;
  const TileGroup* group = nullptr;
  TileSetId id = kNoTileSet;
  float weight = 0.0F;
};

/// The region, clamped to the grid
struct Bounds
{
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
};

Bounds bounds_of(const ScatterMask& mask, Extents e)
{
  const bool whole = (mask.region.width <= 0 or mask.region.height <= 0);
  return Bounds{
    whole ? 0 : std::max(mask.region.x, 0),
    whole ? 0 : std::max(mask.region.y, 0),
    whole ? e.width : std::min(mask.region.x + mask.region.width, e.width),
    whole ? e.height : std::min(mask.region.y + mask.region.height, e.height)};
}

}  // namespace

ScatterResult scatter_groups(
  const TileSets& tilesets,
  const ScatterObject* objects,
  std::size_t object_count,
  const ScatterMask& mask,
  const ScatterParams& params,
  std::uint64_t seed,
  memory::Arena& scratch,
  Grid<TileRef>& out)
{
  ScatterResult result;
  const Extents e = out.extents;
  if (objects == nullptr or object_count == 0 or e.count() == 0 or params.count <= 0)
  {
    return result;
  }
  LVL_ASSERT_TRUE(mask.band == nullptr or mask.band->extents == e) << "the band layer is a different size";
  LVL_ASSERT_TRUE(mask.ids == nullptr or mask.ids->extents == e) << "the id layer is a different size";
  LVL_ASSERT_TRUE(mask.collision == nullptr or mask.collision->extents == e)
    << "the collision layer is a different size";

  memory::Scope scope{scratch};
  const Bounds b = bounds_of(mask, e);
  if (b.x1 <= b.x0 or b.y1 <= b.y0)
  {
    return result;
  }

  // An object naming a tileset or a group that has gone is skipped rather than refused: a profile
  // outlives the tilesets it was written against, and the editor already lists such an entry as
  // dangling. One too big for the region could never be placed, so it is skipped too
  auto resolved = memory::make_vector<Resolved>(scratch);
  resolved.reserve(object_count);
  float total = 0.0F;
  bool weighted = false;
  for (std::size_t i = 0; i < object_count; ++i)
  {
    const ScatterObject& object = objects[i];
    if (object.tileset >= tilesets.size())
    {
      continue;
    }
    const TileSet& ts = tilesets[object.tileset];
    if (object.group >= ts.groups.size())
    {
      continue;
    }
    const TileGroup& group = ts.groups[object.group];
    if (group.size.width > (b.x1 - b.x0) or group.size.height > (b.y1 - b.y0))
    {
      continue;
    }
    const float weight = std::max(object.weight, 0.0F);
    weighted = weighted or (weight > 0.0F);
    total += weight;
    resolved.push_back(Resolved{&ts, &group, object.tileset, weight});
  }
  if (resolved.empty())
  {
    return result;
  }
  // Weights that are all zero (or absent) stand for a weight of 1 each, so an untouched profile
  // scatters evenly and giving every object the same weight comes to exactly the same thing
  total = weighted ? total : static_cast<float>(resolved.size());
  const auto weight_of = [&resolved, weighted](std::size_t i) { return weighted ? resolved[i].weight : 1.0F; };

  // One byte per cell: the room the placements have taken, their footprints grown by the spacing.
  // A placement is tested against the footprints alone, so two objects end up at least `spacing`
  // cells apart without the test having to look at every other object
  auto taken = memory::make_vector<std::uint8_t>(scratch, e.count(), 0U);
  const int spacing = std::max(params.spacing, 0);
  const int attempts = std::max(params.attempts, 1);
  random::State rng = random::seeded(seed);

  const auto pick = [&](float u) {
    float upto = 0.0F;
    std::size_t at = resolved.size() - 1;
    for (std::size_t i = 0; i < resolved.size(); ++i)
    {
      upto += weight_of(i);
      if (u < upto)
      {
        at = i;
        break;
      }
    }
    // An object of weight 0 can still be landed on by the rounding slack above, so step off it
    while (weight_of(at) <= 0.0F and at > 0)
    {
      --at;
    }
    return at;
  };

  const auto fits = [&](const Resolved& object, int x, int y) {
    const TileGroup& group = *object.group;
    if (x < b.x0 or y < b.y0 or x + group.size.width > b.x1 or y + group.size.height > b.y1)
    {
      return false;
    }
    for (int gy = 0; gy < group.size.height; ++gy)
    {
      for (int gx = 0; gx < group.size.width; ++gx)
      {
        // A hole writes nothing, so it is not part of the object and is not tested
        if (group_cell(*object.tileset, group, gx, gy) == kNoTileIndex)
        {
          continue;
        }
        const std::size_t i = e.index(x + gx, y + gy);
        if (taken[i] != 0U)
        {
          return false;
        }
        if (mask.band != nullptr and (mask.band->values[i] < mask.lo or mask.band->values[i] > mask.hi))
        {
          return false;
        }
        if (mask.ids != nullptr and mask.ids->values[i] != mask.id)
        {
          return false;
        }
        if (mask.collision != nullptr and (mask.collision->values[i] & mask.blocked) != 0U)
        {
          return false;
        }
        if (mask.only_empty and !out.values[i].empty())
        {
          return false;
        }
      }
    }
    return true;
  };

  for (int placed = 0; placed < params.count; ++placed)
  {
    bool done = false;
    for (int attempt = 0; attempt < attempts and !done; ++attempt)
    {
      const std::size_t at = pick(random::unit(rng) * total);
      const Resolved& object = resolved[at];
      const int x = b.x0 + static_cast<int>(random::below(rng, static_cast<std::uint32_t>(b.x1 - b.x0)));
      const int y = b.y0 + static_cast<int>(random::below(rng, static_cast<std::uint32_t>(b.y1 - b.y0)));
      if (!fits(object, x, y))
      {
        ++result.rejected;
        continue;
      }
      const TileGroup& group = *object.group;
      for (int gy = 0; gy < group.size.height; ++gy)
      {
        for (int gx = 0; gx < group.size.width; ++gx)
        {
          const TileIndex tile = group_cell(*object.tileset, group, gx, gy);
          if (tile == kNoTileIndex)
          {
            continue;
          }
          out.at(x + gx, y + gy) = TileRef{object.id, tile};
          ++result.cells;
        }
      }
      // The footprint grown by the spacing, clamped to the grid: the whole box, holes included, so
      // nothing is *placed* into a tree's gap. Only a candidate's solid cells are tested against
      // this, so a later object's own gap may still sit over an earlier object's tile -- two trees
      // interlocking through each other's gaps, which is what you want and writes no cell twice
      for (int gy = -spacing; gy < group.size.height + spacing; ++gy)
      {
        for (int gx = -spacing; gx < group.size.width + spacing; ++gx)
        {
          if (e.contains(x + gx, y + gy))
          {
            taken[e.index(x + gx, y + gy)] = 1U;
          }
        }
      }
      ++result.placed;
      done = true;
    }
    result.unplaced += done ? 0U : 1U;
  }
  return result;
}

}  // namespace lvl::procgen
