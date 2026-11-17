/**
 * @copyright 2026-present Brian Cairl
 *
 * @file wfc_rules.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/level/wfc_rules.hpp"

namespace lvl::level
{

WfcTileId find_tile(const WfcRules& rules, TileRef tile)
{
  const auto itr =
    std::find_if(rules.tiles.begin(), rules.tiles.end(), [tile](const WfcTile& t) { return t.tile == tile; });
  return static_cast<WfcTileId>(itr - rules.tiles.begin());
}

WfcTileId add_tile(WfcRules& rules, TileRef tile)
{
  const WfcTileId id = find_tile(rules, tile);
  if (id == rules.tiles.size())
  {
    LVL_ASSERT_LT(rules.tiles.size(), 0xFFFFUL) << "too many WFC tiles";
    rules.tiles.push_back(WfcTile{.tile = tile});
  }
  return id;
}

void remove_tile(WfcRules& rules, WfcTileId id)
{
  LVL_ASSERT_LT(id, rules.tiles.size());
  rules.tiles.erase(rules.tiles.begin() + id);
  std::erase_if(rules.adjacency, [id](const WfcAdjacency& a) { return a.a == id or a.b == id; });
  for (auto& a : rules.adjacency)
  {
    a.a = (a.a > id) ? static_cast<WfcTileId>(a.a - 1) : a.a;
    a.b = (a.b > id) ? static_cast<WfcTileId>(a.b - 1) : a.b;
  }
}

WfcAdjacency canonical(WfcAdjacency adj)
{
  if (adj.direction == Direction::kWest or adj.direction == Direction::kNorth)
  {
    return WfcAdjacency{.a = adj.b, .b = adj.a, .direction = opposite(adj.direction)};
  }
  return adj;
}

bool allowed(const WfcRules& rules, WfcTileId a, WfcTileId b, Direction direction)
{
  const auto c = canonical(WfcAdjacency{.a = a, .b = b, .direction = direction});
  return std::find(rules.adjacency.begin(), rules.adjacency.end(), c) != rules.adjacency.end();
}

void set_allowed(WfcRules& rules, WfcTileId a, WfcTileId b, Direction direction, bool allow)
{
  const auto c = canonical(WfcAdjacency{.a = a, .b = b, .direction = direction});
  const auto itr = std::find(rules.adjacency.begin(), rules.adjacency.end(), c);
  if (allow and itr == rules.adjacency.end())
  {
    rules.adjacency.push_back(c);
  }
  else if (!allow and itr != rules.adjacency.end())
  {
    rules.adjacency.erase(itr);
  }
}

void learn(WfcRules& rules, const Grid<TileRef>& example)
{
  const Extents e = example.extents;
  // First pass: tiles, and weights = occurrence counts (tiles not in the example keep their weight)
  for (const TileRef& t : example.values)
  {
    if (!t.empty())
    {
      add_tile(rules, t);
    }
  }
  memory::Vector<float> counts(rules.tiles.size(), 0.0F, memory::ArenaAllocator<float>{rules.tiles.get_allocator()});
  for (const TileRef& t : example.values)
  {
    if (!t.empty())
    {
      counts[find_tile(rules, t)] += 1.0F;
    }
  }
  for (std::size_t i = 0; i < counts.size(); ++i)
  {
    if (counts[i] > 0.0F)
    {
      rules.tiles[i].weight = counts[i];
    }
  }
  // Second pass: observed east/south neighbor pairs
  for (int y = 0; y < e.height; ++y)
  {
    for (int x = 0; x < e.width; ++x)
    {
      const TileRef here = example.at(x, y);
      if (here.empty())
      {
        continue;
      }
      const WfcTileId a = find_tile(rules, here);
      if (x + 1 < e.width and !example.at(x + 1, y).empty())
      {
        set_allowed(rules, a, find_tile(rules, example.at(x + 1, y)), Direction::kEast, true);
      }
      if (y + 1 < e.height and !example.at(x, y + 1).empty())
      {
        set_allowed(rules, a, find_tile(rules, example.at(x, y + 1)), Direction::kSouth, true);
      }
    }
  }
}

}  // namespace lvl::level
