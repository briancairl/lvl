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
  if (
    adj.direction == Direction::kWest or adj.direction == Direction::kNorth or adj.direction == Direction::kNorthEast or
    adj.direction == Direction::kNorthWest)
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
  // Second pass: observed neighbor pairs (the canonical directions cover every pair once)
  static constexpr Direction kCanonical[] = {
    Direction::kEast, Direction::kSouth, Direction::kSouthEast, Direction::kSouthWest};
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
      for (const Direction d : kCanonical)
      {
        const Cell n = step(Cell{x, y}, d);
        if (e.contains(n) and !example.at(n).empty())
        {
          set_allowed(rules, a, find_tile(rules, example.at(n)), d, true);
        }
      }
    }
  }
}

std::size_t diagonal_count(const WfcRules& rules)
{
  return static_cast<std::size_t>(std::count_if(
    rules.adjacency.begin(), rules.adjacency.end(), [](const WfcAdjacency& a) { return diagonal(a.direction); }));
}

void derive_diagonals(WfcRules& rules, memory::Arena& scratch)
{
  memory::Scope scope{scratch};
  // A corner is reached through either of its two sides: {corner, first side, second side}
  struct Path
  {
    Direction corner;
    Direction first;
    Direction second;
  };
  static constexpr Path kPaths[] = {
    {Direction::kSouthEast, Direction::kEast, Direction::kSouth},
    {Direction::kSouthEast, Direction::kSouth, Direction::kEast},
    {Direction::kSouthWest, Direction::kWest, Direction::kSouth},
    {Direction::kSouthWest, Direction::kSouth, Direction::kWest},
  };
  const auto n = static_cast<WfcTileId>(rules.tiles.size());
  // Snapshot of the side relation (an n*n bit matrix per side), so new corner rules don't feed back
  auto side = memory::make_vector<std::uint8_t>(scratch, 4 * static_cast<std::size_t>(n) * n, std::uint8_t{0});
  const auto at = [n](Direction d, WfcTileId a, WfcTileId b) { return (static_cast<std::size_t>(d) * n + a) * n + b; };
  for (const WfcAdjacency& adj : rules.adjacency)
  {
    if (!diagonal(adj.direction) and adj.a < n and adj.b < n)
    {
      side[at(adj.direction, adj.a, adj.b)] = 1;
      side[at(opposite(adj.direction), adj.b, adj.a)] = 1;
    }
  }
  for (const Path& p : kPaths)
  {
    for (WfcTileId a = 0; a < n; ++a)
    {
      for (WfcTileId c = 0; c < n; ++c)
      {
        if (side[at(p.first, a, c)] == 0)
        {
          continue;
        }
        for (WfcTileId b = 0; b < n; ++b)
        {
          if (side[at(p.second, c, b)] != 0)
          {
            set_allowed(rules, a, b, p.corner, true);
          }
        }
      }
    }
  }
}

}  // namespace lvl::level
