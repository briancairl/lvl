/**
 * @copyright 2026-present Brian Cairl
 *
 * @file steps.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/procgen/steps.hpp"

namespace lvl::procgen
{
namespace
{

using level::Cell;
using level::Direction;
using level::StepCode;
using level::StepRelation;
using level::StepTerrain;
using level::StepTerrainId;
using level::StepType;
using level::TileRef;

}  // namespace

TerraceResult terrace(const level::ScalarGrid& height, const TerraceParams& params, level::IdGrid& levels)
{
  TerraceResult result;
  const level::Extents e = height.extents;
  if (e.count() == 0)
  {
    return result;
  }
  LVL_ASSERT_TRUE(levels.extents == e) << "the level layer is a different size to the heightmap";
  // The range to cut up: what was asked for, or what the heightmap actually holds
  float lo = params.lo;
  float hi = params.hi;
  if (!(hi > lo))
  {
    lo = height.values[0];
    hi = height.values[0];
    for (const float v : height.values)
    {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  const int ceiling = std::max(params.max_level, 0);
  const float span = hi - lo;
  if (!(span > 0.0F))
  {
    // A flat map is one level, whatever was asked for
    for (std::size_t i = 0; i < e.count(); ++i)
    {
      result.changed += (levels.values[i] == 0) ? 0U : 1U;
      levels.values[i] = 0;
    }
    return result;
  }
  const float rate = (params.mode == TerraceMode::kRate) ? std::max(params.rate, 1e-6F)
                                                         : span / static_cast<float>(std::max(params.levels, 1));
  result.rate = rate;
  for (std::size_t i = 0; i < e.count(); ++i)
  {
    const float above = height.values[i] - lo;
    int level = static_cast<int>(std::floor(above / rate));
    // The top of the range belongs to the last band rather than to one of its own, and a stray
    // value outside [lo, hi] is clamped rather than making a band nothing else reaches
    const int top_band = (params.mode == TerraceMode::kRate) ? static_cast<int>(std::floor(span / rate - 1e-6F))
                                                             : std::max(params.levels, 1) - 1;
    level = std::clamp(level, 0, std::min(top_band, ceiling));
    result.top = std::max(result.top, level);
    const auto value = static_cast<std::uint16_t>(level);
    result.changed += (levels.values[i] == value) ? 0U : 1U;
    levels.values[i] = value;
  }
  return result;
}

StepFillResult tile_steps(
  const level::StepRules& rules,
  const level::IdGrid& levels,
  const StepFillParams& params,
  level::Grid<TileRef>& tiles,
  level::MaskGrid* collision)
{
  StepFillResult result;
  const level::Extents e = tiles.extents;
  if (e.count() == 0)
  {
    return result;
  }
  LVL_ASSERT_TRUE(levels.extents == e) << "the level layer is a different size to the tile layer";
  LVL_ASSERT_TRUE(collision == nullptr or collision->extents == e) << "the collision layer is a different size";
  for (int y = 0; y < e.height; ++y)
  {
    for (int x = 0; x < e.width; ++x)
    {
      const std::size_t i = e.index(x, y);
      const int here = static_cast<int>(levels.values[i]);
      // How this cell stands against each of its eight neighbors. Off the grid counts as level, so
      // the map's edge is not read as a cliff
      StepCode code = 0;
      bool steps_down = false;
      bool steps_up = false;
      for (std::size_t n = 0; n < level::kStepNeighbors.size(); ++n)
      {
        const Cell c = step(Cell{x, y}, level::kStepNeighbors[n]);
        const int there = e.contains(c) ? static_cast<int>(levels.values[e.index(c.x, c.y)]) : here;
        const StepRelation relation = (there == here) ? StepRelation::kLevel
          : (there < here)                            ? StepRelation::kBelow
                                                      : StepRelation::kAbove;
        code = level::step_code_with(code, n, relation);
        steps_down = steps_down or (relation == StepRelation::kBelow);
        steps_up = steps_up or (relation == StepRelation::kAbove);
      }

      if (collision != nullptr and params.collision != StepCollision::kNone)
      {
        // Set where this cell is on the blocked side of a step, cleared where it is not, so a
        // re-run after the heightmap changes leaves no walls where the cliff has gone
        const bool block = (params.collision == StepCollision::kAbove) ? steps_down : steps_up;
        const std::uint8_t mask = collision->values[i];
        collision->values[i] =
          static_cast<std::uint8_t>(block ? (mask | level::collision::kSolid) : (mask & ~level::collision::kSolid));
        result.blocked += block ? 1U : 0U;
      }

      const StepTerrainId id = level::step_terrain_at(rules, here);
      if (id == level::kNoStepTerrain)
      {
        ++result.untermed;
        if (!params.keep_untermed)
        {
          result.changed += tiles.values[i].empty() ? 0U : 1U;
          tiles.values[i] = level::kEmptyTile;
        }
        continue;
      }
      const StepTerrain& terrain = rules.terrains[id];
      const TileRef tile = terrain.tiles[static_cast<std::size_t>(level::step_type_of(code))];
      if (tile.empty())
      {
        ++result.missing;
        continue;
      }
      ++result.tiled;
      result.changed += (tiles.values[i] == tile) ? 0U : 1U;
      tiles.values[i] = tile;
    }
  }
  return result;
}

}  // namespace lvl::procgen
