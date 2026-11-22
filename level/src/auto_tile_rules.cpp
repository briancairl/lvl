/**
 * @copyright 2026-present Brian Cairl
 *
 * @file auto_tile_rules.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <array>
#include <bit>

// LVL
#include "lvl/level/auto_tile_rules.hpp"

namespace lvl::level
{
namespace
{

/// Bits two patterns agree on
int same(NeighborMask a, NeighborMask b, NeighborMask of)
{
  return static_cast<int>(std::popcount(static_cast<unsigned>(static_cast<NeighborMask>(~(a ^ b)) & of)));
}

/**
 * @brief How well two patterns agree, sides before corners
 *
 * The sides are what place a tile -- a north edge is a north edge whatever the corners do -- so a
 * side counts for more than every corner together. Without that, a set that has no tile for a
 * pattern could answer it with a tile meant for another edge that happens to share corners.
 */
int agreement(NeighborMask a, NeighborMask b) { return same(a, b, kSideNeighbors) * 8 + same(a, b, kCornerNeighbors); }

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

std::size_t variant_count(const AutoTerrain& terrain, NeighborMask mask)
{
  const NeighborMask wanted = reduce(mask, terrain.neighborhood);
  std::size_t count = 0;
  for (const AutoVariant& v : terrain.variants)
  {
    count += (v.mask == wanted) ? 1U : 0U;
  }
  return count;
}

void set_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile)
{
  const NeighborMask key = reduce(mask, terrain.neighborhood);
  std::erase_if(terrain.variants, [key](const AutoVariant& v) { return v.mask == key; });
  if (!tile.empty())
  {
    add_variant(terrain, key, tile);
  }
}

void add_variant(AutoTerrain& terrain, NeighborMask mask, TileRef tile)
{
  const NeighborMask key = reduce(mask, terrain.neighborhood);
  if (tile.empty())
  {
    return;
  }
  for (const AutoVariant& v : terrain.variants)
  {
    if (v.mask == key and v.tile == tile)
    {
      return;
    }
  }
  terrain.variants.push_back(AutoVariant{.mask = key, .tile = tile});
  // By pattern, keeping each pool in the order it was given, so the editor's slots and the files
  // list them the same way every time
  std::stable_sort(terrain.variants.begin(), terrain.variants.end(), [](const AutoVariant& a, const AutoVariant& b) {
    return a.mask < b.mask;
  });
}

TileRef resolve(const AutoTerrain& terrain, NeighborMask mask, std::uint64_t key)
{
  const NeighborMask wanted = reduce(mask, terrain.neighborhood);
  // The pattern itself, or the nearest one the terrain has tiles for
  NeighborMask chosen = wanted;
  if (find_variant(terrain, wanted) == nullptr)
  {
    int best = -1;
    for (const AutoVariant& v : terrain.variants)
    {
      if (const int score = agreement(v.mask, wanted); score > best)
      {
        best = score;
        chosen = v.mask;
      }
    }
    if (best < 0)
    {
      return kEmptyTile;
    }
  }
  // One of that pattern's pool, by key, so a cell keeps its tile however often it is repainted
  const std::size_t count = variant_count(terrain, chosen);
  std::size_t at = (count < 2) ? 0 : static_cast<std::size_t>(key % count);
  for (const AutoVariant& v : terrain.variants)
  {
    if (v.mask == chosen and at-- == 0)
    {
      return v.tile;
    }
  }
  return kEmptyTile;
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

namespace
{

/// Cells a layout may hold for a terrain to be read out of it (a handful of palette tiles)
constexpr std::size_t kMaxLayoutCells = 1024;

/// Top-left corner of the first fully-filled <code>size</code> x <code>size</code> block not yet taken
bool find_block(const Grid<TileRef>& layout, const std::uint8_t* taken, int size, Cell& out)
{
  const Extents e = layout.extents;
  for (int y = 0; y + size <= e.height; ++y)
  {
    for (int x = 0; x + size <= e.width; ++x)
    {
      bool whole = true;
      for (int dy = 0; dy < size and whole; ++dy)
      {
        for (int dx = 0; dx < size and whole; ++dx)
        {
          const std::size_t i = e.index(x + dx, y + dy);
          whole = !layout.values[i].empty() and taken[i] == 0U;
        }
      }
      if (whole)
      {
        out = Cell{x, y};
        return true;
      }
    }
  }
  return false;
}

/// The nine-slice pattern of cell (x, y) of a 3x3 block: a large patch of terrain seen through it
NeighborMask nine_slice_mask(int x, int y)
{
  NeighborMask mask = kNoNeighbors;
  for (const Direction d : kNeighbors)
  {
    const Cell offset = step(Cell{0, 0}, d);
    if (within(x, 3, offset.x) and within(y, 3, offset.y))
    {
      mask = static_cast<NeighborMask>(mask | mask_of(d));
    }
  }
  return mask;
}

}  // namespace

std::size_t learn_from_layout(AutoTerrain& terrain, const Grid<TileRef>& layout)
{
  const Extents e = layout.extents;
  if (e.count() == 0)
  {
    return 0;
  }
  if (e.count() > kMaxLayoutCells)
  {
    return 0;
  }
  // Which cells a block has already taken (on the stack: a layout is a handful of palette tiles)
  std::array<std::uint8_t, kMaxLayoutCells> taken = {};
  const auto take = [&](Cell corner, int size, auto&& assign) {
    for (int dy = 0; dy < size; ++dy)
    {
      for (int dx = 0; dx < size; ++dx)
      {
        const std::size_t i = e.index(corner.x + dx, corner.y + dy);
        taken[i] = 1U;
        assign(dx, dy, layout.values[i]);
      }
    }
  };
  std::size_t written = 0;
  // The nine-slice: the four sides, the four outer corners and the fully-surrounded tile
  Cell corner{0, 0};
  const bool nine_slice = find_block(layout, taken.data(), 3, corner);
  if (nine_slice)
  {
    take(corner, 3, [&terrain](int x, int y, TileRef tile) { set_variant(terrain, nine_slice_mask(x, y), tile); });
    written += 9;
  }
  // The inner corners: surrounded except at the corner the tile sits at in the block. A side-only
  // terrain cannot tell them apart, so it leaves them alone
  if (terrain.neighborhood != Neighborhood::kSides and find_block(layout, taken.data(), 2, corner))
  {
    static constexpr Direction kMissing[2][2] = {
      {Direction::kNorthWest, Direction::kSouthWest}, {Direction::kNorthEast, Direction::kSouthEast}};
    take(corner, 2, [&terrain](int x, int y, TileRef tile) {
      set_variant(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(kMissing[x][y])), tile);
    });
    written += 4;
  }
  // Whatever else was selected alongside a nine-slice: more middles, which a painted area scatters
  // between. On its own a single tile is the middle; anything else is no shape this understands
  if (!nine_slice and e.count() != 1)
  {
    return written;
  }
  for (std::size_t i = 0; i < e.count(); ++i)
  {
    if (taken[i] == 0U and !layout.values[i].empty())
    {
      add_variant(terrain, kAllNeighbors, layout.values[i]);
      ++written;
    }
  }
  return written;
}

}  // namespace lvl::level
