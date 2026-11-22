/**
 * @copyright 2026-present Brian Cairl
 *
 * @file auto_tile_rules.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <bit>

// LVL
#include "lvl/level/auto_tile_rules.hpp"

namespace lvl::level
{
namespace
{

/// Agreement between two patterns: how many of the eight neighbors they say the same thing about
int agreement(NeighborMask a, NeighborMask b)
{
  return static_cast<int>(std::popcount(static_cast<unsigned>(static_cast<NeighborMask>(~(a ^ b)))));
}

/// Whether the terrain reaches <code>offset</code> cells along one axis of a nine-slice block
bool within(int coordinate, int size, int offset)
{
  // 0 is the low edge of a large patch (nothing beyond it), size - 1 the high edge, between them
  // the middle, which the patch continues through
  const bool low = (coordinate == 0) and (offset < 0);
  const bool high = (coordinate == size - 1) and (offset > 0);
  return !low and !high;
}

}  // namespace

TerrainId find_terrain(const AutoTileRules& rules, std::string_view name)
{
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    if (rules.terrains[i].name == name)
    {
      return static_cast<TerrainId>(i);
    }
  }
  return kNoTerrain;
}

TerrainId add_terrain(AutoTileRules& rules, memory::Arena& arena, std::string_view name)
{
  if (const TerrainId existing = find_terrain(rules, name); existing != kNoTerrain)
  {
    return existing;
  }
  LVL_ASSERT_LT(rules.terrains.size(), static_cast<std::size_t>(kNoTerrain)) << "too many terrains";
  rules.terrains.emplace_back(arena);
  rules.terrains.back().name = name;
  return static_cast<TerrainId>(rules.terrains.size() - 1);
}

void remove_terrain(AutoTileRules& rules, TerrainId id)
{
  LVL_ASSERT_LT(id, rules.terrains.size());
  rules.terrains.erase(rules.terrains.begin() + id);
}

TerrainId terrain_of(const AutoTileRules& rules, TileRef tile)
{
  if (tile.empty())
  {
    return kNoTerrain;
  }
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    for (const AutoVariant& v : rules.terrains[i].variants)
    {
      if (v.tile == tile)
      {
        return static_cast<TerrainId>(i);
      }
    }
  }
  return kNoTerrain;
}

NeighborMask reduce(NeighborMask mask, Neighborhood neighborhood)
{
  if (neighborhood == Neighborhood::kSides)
  {
    return static_cast<NeighborMask>(mask & kSideNeighbors);
  }
  NeighborMask out = static_cast<NeighborMask>(mask & kSideNeighbors);
  for (const Direction corner : kDiagonals)
  {
    const auto [a, b] = corner_sides(corner);
    // A corner only tells the tiles apart where the terrain reaches both of its sides
    if (has(mask, corner) and has(mask, a) and has(mask, b))
    {
      out = static_cast<NeighborMask>(out | mask_of(corner));
    }
  }
  return out;
}

const AutoVariant* find_variant(const AutoTerrain& terrain, NeighborMask mask)
{
  const NeighborMask wanted = reduce(mask, terrain.neighborhood);
  for (const AutoVariant& v : terrain.variants)
  {
    if (v.mask == wanted)
    {
      return &v;
    }
  }
  return nullptr;
}

void set_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile)
{
  const NeighborMask key = reduce(mask, terrain.neighborhood);
  const auto itr = std::find_if(
    terrain.variants.begin(), terrain.variants.end(), [key](const AutoVariant& v) { return v.mask == key; });
  if (tile.empty())
  {
    if (itr != terrain.variants.end())
    {
      terrain.variants.erase(itr);
    }
    return;
  }
  if (itr != terrain.variants.end())
  {
    itr->tile = tile;
    return;
  }
  terrain.variants.push_back(AutoVariant{.mask = key, .tile = tile});
  // Ascending, so the editor's slots and the files list them in a stable order
  std::sort(terrain.variants.begin(), terrain.variants.end(), [](const AutoVariant& a, const AutoVariant& b) {
    return a.mask < b.mask;
  });
}

TileRef resolve(const AutoTerrain& terrain, NeighborMask mask)
{
  const NeighborMask wanted = reduce(mask, terrain.neighborhood);
  const AutoVariant* best = nullptr;
  int best_agreement = -1;
  for (const AutoVariant& v : terrain.variants)
  {
    if (v.mask == wanted)
    {
      return v.tile;
    }
    // Nothing for this pattern: the nearest one keeps the cell filled
    if (const int score = agreement(v.mask, wanted); score > best_agreement)
    {
      best_agreement = score;
      best = &v;
    }
  }
  return (best == nullptr) ? kEmptyTile : best->tile;
}

memory::Vector<NeighborMask> patterns(Neighborhood neighborhood, memory::Arena& arena)
{
  auto out = memory::make_vector<NeighborMask>(arena);
  for (int m = 0; m <= static_cast<int>(kAllNeighbors); ++m)
  {
    const auto mask = static_cast<NeighborMask>(m);
    if (reduce(mask, neighborhood) == mask)
    {
      out.push_back(mask);
    }
  }
  return out;
}

std::size_t learn_from_layout(AutoTerrain& terrain, const Grid<TileRef>& layout)
{
  const Extents e = layout.extents;
  const bool nine_slice = (e.width == 3 and e.height == 3);
  const bool inner_corners = (e.width == 2 and e.height == 2);
  const bool middle = (e.width == 1 and e.height == 1);
  // Side-only tiling has no inner corners to put anywhere
  if (inner_corners and terrain.neighborhood == Neighborhood::kSides)
  {
    return 0;
  }
  if (!nine_slice and !inner_corners and !middle)
  {
    return 0;
  }
  std::size_t written = 0;
  for (int y = 0; y < e.height; ++y)
  {
    for (int x = 0; x < e.width; ++x)
    {
      const TileRef tile = layout.at(x, y);
      if (tile.empty())
      {
        continue;
      }
      NeighborMask mask = kNoNeighbors;
      if (inner_corners)
      {
        // The terrain surrounds the cell except at one corner: the one this tile sits at
        static constexpr Direction kMissing[2][2] = {
          {Direction::kNorthWest, Direction::kSouthWest}, {Direction::kNorthEast, Direction::kSouthEast}};
        mask = static_cast<NeighborMask>(kAllNeighbors & ~mask_of(kMissing[x][y]));
      }
      else
      {
        for (const Direction d : kNeighbors)
        {
          const Cell offset = step(Cell{0, 0}, d);
          if (within(x, e.width, offset.x) and within(y, e.height, offset.y))
          {
            mask = static_cast<NeighborMask>(mask | mask_of(d));
          }
        }
      }
      set_variant(terrain, mask, tile);
      ++written;
    }
  }
  return written;
}

}  // namespace lvl::level
