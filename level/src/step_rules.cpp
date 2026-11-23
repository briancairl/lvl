/**
 * @copyright 2026-present Brian Cairl
 *
 * @file step_rules.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/level/step_rules.hpp"

namespace lvl::level
{
namespace
{

/**
 * @brief gft's table, as a map from "which neighbors are below me" to a tile
 *
 * In gft every case leaves the "a neighbor is above me" group empty, so each pattern it names is
 * decided entirely by which of the eight neighbors are lower; the rest are level. That makes the
 * table an 8-bit mask (NW, N, NE, W, E, SW, S, SE) rather than gft's 24-bit code, and the patterns
 * it does not name fall through to kFlat, as they do there.
 */
struct BelowCase
{
  std::uint8_t below;
  StepType type;
};

constexpr std::uint8_t below_of(std::initializer_list<std::size_t> neighbors)
{
  std::uint8_t mask = 0;
  for (const std::size_t n : neighbors)
  {
    mask = static_cast<std::uint8_t>(mask | (1U << n));
  }
  return mask;
}

// Neighbor indices: 0 NW, 1 N, 2 NE, 3 W, 4 E, 5 SW, 6 S, 7 SE
constexpr std::array<BelowCase, 21> kCases = {{
  // Outward: the rim of ground standing above what is around it
  {below_of({0, 1, 2, 3, 5}), StepType::kOutwardTopRight},
  {below_of({0, 1, 2}), StepType::kOutwardTop},
  {below_of({0, 1, 2, 4, 7}), StepType::kOutwardTopLeft},
  {below_of({0, 3, 5}), StepType::kOutwardLeft},
  {below_of({2, 4, 7}), StepType::kOutwardRight},
  {below_of({0, 3, 5, 6, 7}), StepType::kOutwardBottomLeft},
  {below_of({5, 6, 7}), StepType::kOutwardBottom},
  {below_of({2, 4, 5, 6, 7}), StepType::kOutwardBottomRight},
  // Inward: where that rim turns back on itself, around a hole
  {below_of({7}), StepType::kInwardBottomRight},
  {below_of({5, 6}), StepType::kInwardBottom},
  {below_of({6, 7}), StepType::kInwardBottom},
  {below_of({5}), StepType::kInwardBottomLeft},
  {below_of({2, 4}), StepType::kInwardRight},
  {below_of({4, 7}), StepType::kInwardRight},
  {below_of({0, 3}), StepType::kInwardLeft},
  {below_of({3, 5}), StepType::kInwardLeft},
  {below_of({2}), StepType::kInwardTopRight},
  {below_of({1, 2}), StepType::kInwardTop},
  {below_of({0, 1}), StepType::kInwardTop},
  {below_of({0}), StepType::kInwardTopLeft},
  // Level on every side: plain ground
  {below_of({}), StepType::kFlat},
}};

}  // namespace

StepType step_type_of(StepCode code)
{
  std::uint8_t below = 0;
  for (std::size_t n = 0; n < kStepNeighbors.size(); ++n)
  {
    const StepRelation relation = step_relation(code, n);
    if (relation == StepRelation::kAbove)
    {
      // Something stands over this cell, so it is not the top of a step: plain ground
      return StepType::kFlat;
    }
    below = static_cast<std::uint8_t>(below | ((relation == StepRelation::kBelow) ? (1U << n) : 0U));
  }
  for (const BelowCase& c : kCases)
  {
    if (c.below == below)
    {
      return c.type;
    }
  }
  return StepType::kFlat;
}

StepTerrainId find_step_terrain(const StepRules& rules, std::string_view name)
{
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    if (rules.terrains[i].name == name)
    {
      return static_cast<StepTerrainId>(i);
    }
  }
  return kNoStepTerrain;
}

StepTerrainId add_step_terrain(StepRules& rules, std::string_view name)
{
  if (const StepTerrainId existing = find_step_terrain(rules, name); existing != kNoStepTerrain)
  {
    return existing;
  }
  LVL_ASSERT_LT(rules.terrains.size(), static_cast<std::size_t>(kNoStepTerrain)) << "too many step terrains";
  rules.terrains.push_back(StepTerrain{});
  rules.terrains.back().name = name;
  return static_cast<StepTerrainId>(rules.terrains.size() - 1);
}

void remove_step_terrain(StepRules& rules, StepTerrainId id)
{
  LVL_ASSERT_LT(id, rules.terrains.size());
  rules.terrains.erase(rules.terrains.begin() + id);
}

StepTerrainId step_terrain_at(const StepRules& rules, int level)
{
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    const StepTerrain& t = rules.terrains[i];
    if (level >= std::min(t.level_lo, t.level_hi) and level <= std::max(t.level_lo, t.level_hi))
    {
      return static_cast<StepTerrainId>(i);
    }
  }
  return kNoStepTerrain;
}

StepTerrainId step_terrain_of(const StepRules& rules, TileRef tile)
{
  if (tile.empty())
  {
    return kNoStepTerrain;
  }
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    for (const TileRef& t : rules.terrains[i].tiles)
    {
      if (t == tile)
      {
        return static_cast<StepTerrainId>(i);
      }
    }
  }
  return kNoStepTerrain;
}

std::size_t learn_step_layout(StepTerrain& terrain, const Grid<TileRef>& layout)
{
  StepLayout shape = StepLayout::kStacked;
  if (!step_layout_of(layout.extents.width, layout.extents.height, shape))
  {
    return 0;
  }
  for (int y = 0; y < layout.extents.height; ++y)
  {
    for (int x = 0; x < layout.extents.width; ++x)
    {
      terrain.tiles[step_type_at(shape, x, y)] = layout.at(x, y);
    }
  }
  return kStepTypeCount;
}

}  // namespace lvl::level
