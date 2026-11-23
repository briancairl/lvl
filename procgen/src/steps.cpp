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

/// How many times a repair sweep may run before it is taken as settled. A sweep fills at most one
/// cell per offending cell, so a wide shape can need many; the cap is only there to end badly
constexpr int kMaxRepairs = 4096;

/// Morphology over one upper set, with the grid's edge treated as a copy of the nearest cell
class Morphology
{
public:
  Morphology(level::Extents e, memory::Arena& scratch) :
      e_{e},
      a_{memory::make_vector<std::uint8_t>(scratch, e.count(), std::uint8_t{0})},
      b_{memory::make_vector<std::uint8_t>(scratch, e.count(), std::uint8_t{0})}
  {}

  std::uint8_t* data() { return a_.data(); }

  /// Shaves off anything narrower than <code>n</code>
  void open(int n)
  {
    erode(n);
    dilate(n);
  }

  /// Fills anything narrower than <code>n</code>
  void close(int n)
  {
    dilate(n);
    erode(n);
  }

  /**
   * @brief Grows the set until every cell's neighborhood is one the table has a tile for
   *
   * The two passes above remove one-cell features, but they cannot remove a 45-degree boundary: a
   * staircase with treads one cell long reads as a corner cut across, which gft's table does not
   * name either. Rather than guess at a shape rule, this asks the table itself -- any cell whose
   * neighborhood it answers with "flat" although the ground drops away is a cell to repair -- and
   * squares the shape off by filling the cells that drop: its sides first, which is the smaller
   * change, and its corners only when no side is open.
   *
   * It only ever adds cells, so it settles: a staircase is rounded up into blocks rather than cut
   * through, and the set cannot grow past the grid.
   */
  bool repair()
  {
    bool touched = false;
    for (int y = 0; y < e_.height; ++y)
    {
      for (int x = 0; x < e_.width; ++x)
      {
        if (a_[e_.index(x, y)] == 0U)
        {
          continue;
        }
        level::StepCode code = 0;
        bool drops = false;
        for (std::size_t n = 0; n < level::kStepNeighbors.size(); ++n)
        {
          const Cell c = step(Cell{x, y}, level::kStepNeighbors[n]);
          // Off the grid counts as level with this cell, exactly as tile_steps reads it -- the two
          // have to agree about the border or the repair cannot see what the tiler will complain
          // about (erode and dilate replicate the edge instead, which is right for a window)
          const bool below = e_.contains(c) and a_[e_.index(c.x, c.y)] == 0U;
          code = level::step_code_with(code, n, below ? StepRelation::kBelow : StepRelation::kLevel);
          drops = drops or below;
        }
        if (!drops or level::step_type_of(code) != StepType::kFlat)
        {
          continue;
        }
        // One cell at a time, sides before corners: the smallest change that can help, so a
        // staircase is squared off rather than swallowed
        bool filled = false;
        for (const std::size_t n : {1U, 3U, 4U, 6U, 0U, 2U, 5U, 7U})
        {
          const Cell c = step(Cell{x, y}, level::kStepNeighbors[n]);
          if (!filled and e_.contains(c) and a_[e_.index(c.x, c.y)] == 0U)
          {
            a_[e_.index(c.x, c.y)] = 1U;
            filled = true;
          }
        }
        touched = touched or filled;
      }
    }
    return touched;
  }

private:
  [[nodiscard]] std::uint8_t at(int x, int y) const
  {
    // Off the grid reads as the nearest cell on it, so the map's edge is not shaved away
    return a_[e_.index(std::clamp(x, 0, e_.width - 1), std::clamp(y, 0, e_.height - 1))];
  }

  void erode(int n)
  {
    for (int y = 0; y < e_.height; ++y)
    {
      for (int x = 0; x < e_.width; ++x)
      {
        std::uint8_t all = 1;
        for (int dy = 0; dy < n and all != 0; ++dy)
        {
          for (int dx = 0; dx < n and all != 0; ++dx)
          {
            all = at(x + dx, y + dy);
          }
        }
        b_[e_.index(x, y)] = all;
      }
    }
    a_.swap(b_);
  }

  void dilate(int n)
  {
    // The reflected element, so erode then dilate is a proper opening
    for (int y = 0; y < e_.height; ++y)
    {
      for (int x = 0; x < e_.width; ++x)
      {
        std::uint8_t any = 0;
        for (int dy = 0; dy < n and any == 0; ++dy)
        {
          for (int dx = 0; dx < n and any == 0; ++dx)
          {
            any = at(x - dx, y - dy);
          }
        }
        b_[e_.index(x, y)] = any;
      }
    }
    a_.swap(b_);
  }

  level::Extents e_;
  memory::Vector<std::uint8_t> a_;
  memory::Vector<std::uint8_t> b_;
};

}  // namespace

TidyResult tidy_levels(level::IdGrid& levels, const TidyParams& params, memory::Arena& scratch)
{
  TidyResult result;
  const level::Extents e = levels.extents;
  const int n = std::max(params.min_feature, 1);
  const bool morphology = (n >= 2) and (params.remove_spurs or params.fill_notches);
  if (e.count() == 0 or (!morphology and !params.square_corners))
  {
    result.stable = true;
    return result;
  }
  int top = 0;
  for (const std::uint16_t v : levels.values)
  {
    top = std::max(top, static_cast<int>(v));
  }
  if (top == 0)
  {
    result.stable = true;
    return result;
  }
  memory::Scope scope{scratch};
  Morphology work{e, scratch};
  auto out = memory::make_vector<std::uint16_t>(scratch, e.count(), std::uint16_t{0});
  auto before = memory::make_vector<std::uint16_t>(scratch, e.count(), std::uint16_t{0});
  before.assign(levels.values.begin(), levels.values.end());

  // The upper sets -- the cells at level k or above -- are nested by definition, and cleaning each
  // one on its own would break that: a cell could end up in the set for level 3 but not the one for
  // level 2, and then the levels read back off them are not the sets that were cleaned. So they are
  // built from the top down, each one starting from the set above it, which keeps them nested
  // whatever the cleaning does. A cell's level is then the highest set it belongs to.
  auto above = memory::make_vector<std::uint8_t>(scratch, e.count(), std::uint8_t{0});
  int round = 0;
  bool settled = false;
  bool repaired = true;
  for (; round < std::max(params.passes, 1); ++round)
  {
    std::fill(out.begin(), out.end(), std::uint16_t{0});
    std::fill(above.begin(), above.end(), std::uint8_t{0});
    for (int k = top; k >= 1; --k)
    {
      std::uint8_t* mask = work.data();
      for (std::size_t i = 0; i < e.count(); ++i)
      {
        mask[i] = (static_cast<int>(levels.values[i]) >= k) ? 1U : 0U;
      }
      if (morphology and round == 0)
      {
        if (params.remove_spurs)
        {
          work.open(n);
        }
        if (params.fill_notches)
        {
          work.close(n);
        }
        mask = work.data();
      }
      // Never narrower than the terrace above it, so the sets stay nested
      for (std::size_t i = 0; i < e.count(); ++i)
      {
        mask[i] = static_cast<std::uint8_t>(mask[i] | above[i]);
      }
      if (params.square_corners)
      {
        int fix = 0;
        for (; fix < kMaxRepairs and work.repair(); ++fix)
        {
        }
        repaired = repaired and (fix < kMaxRepairs);
        mask = work.data();
      }
      for (std::size_t i = 0; i < e.count(); ++i)
      {
        above[i] = mask[i];
        if (mask[i] != 0U and out[i] == 0U)
        {
          out[i] = static_cast<std::uint16_t>(k);
        }
      }
    }
    const bool moved = !std::equal(out.begin(), out.end(), levels.values.begin());
    levels.values.assign(out.begin(), out.end());
    if (!moved)
    {
      settled = true;
      break;
    }
  }
  for (std::size_t i = 0; i < e.count(); ++i)
  {
    result.changed += (before[i] == levels.values[i]) ? 0U : 1U;
  }
  result.passes = round + 1;
  result.stable = settled and repaired;
  return result;
}

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

      const StepType type = level::step_type_of(code);
      // A cell with ground below it that still came out flat is a shape the table has no tile for:
      // a one-cell spur or notch, which tidy_levels is there to remove. A cell with ground *above*
      // it is not one of those -- the table draws nothing on the lower side of a step by design
      result.unexpressed += (steps_down and !steps_up and type == StepType::kFlat) ? 1U : 0U;

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
      const TileRef tile = terrain.tiles[static_cast<std::size_t>(type)];
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
