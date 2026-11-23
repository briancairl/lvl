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
      b_{memory::make_vector<std::uint8_t>(scratch, e.count(), std::uint8_t{0})},
      c_{memory::make_vector<std::uint8_t>(scratch, e.count(), std::uint8_t{0})}
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
   * @brief Snaps the set to a lattice of n x n blocks, each block going the way its cells vote
   *
   * This is what squares a terrace off without moving it. A boundary that runs diagonally cannot be
   * drawn at all (see <code>repair</code>), and growing cells one at a time until it can turns a
   * valley into a plateau -- the fix cascades, each cell it raises making offenders of its
   * neighbors, and a diagonal carries that along its whole length. Voting a whole block at a time
   * cannot cascade: every block is decided once, by what was already there.
   *
   * The lattice has a phase, and it matters: a two-cell terrace lying across the blocks rather
   * than along them is a tie in every block it touches and can be voted away. So each terrace is
   * snapped at whichever of the n x n phases changes the fewest of its cells, which is what keeps
   * a feature the width it was asked to keep.
   */
  void snap(int n, int ox, int oy)
  {
    phase_x_ = ox;
    phase_y_ = oy;
    snap_at(n, ox, oy);
  }

  /**
   * @brief Fills blocks that meet only at a corner, so the lattice has no diagonal joins either
   *
   * Once the set is a union of blocks, the one arrangement still left without a tile is two blocks
   * touching corner to corner -- the same 45-degree join as before, one scale up. Filling the two
   * empty blocks squares it off. Blocks only ever fill, so this ends, and because a block is only
   * reached when it already touches the terrace at two corners it does not run away.
   */
  bool join_blocks(int n)
  {
    bool touched = false;
    const int ox = phase_x_;
    const int oy = phase_y_;
    const auto block = [&](int bx, int by) {
      const int x = std::clamp(bx, 0, e_.width - 1);
      const int y = std::clamp(by, 0, e_.height - 1);
      return (bx < e_.width and by < e_.height) and a_[e_.index(x, y)] != 0U;
    };
    const auto fill = [&](int bx, int by) {
      for (int dy = std::max(by, 0); dy < by + n and dy < e_.height; ++dy)
      {
        for (int dx = std::max(bx, 0); dx < bx + n and dx < e_.width; ++dx)
        {
          a_[e_.index(dx, dy)] = 1U;
        }
      }
    };
    for (int by = oy - n; by + n < e_.height; by += n)
    {
      for (int bx = ox - n; bx + n < e_.width; bx += n)
      {
        if (bx + n < 0 or by + n < 0)
        {
          continue;
        }
        const bool nw = block(bx, by);
        const bool ne = block(bx + n, by);
        const bool sw = block(bx, by + n);
        const bool se = block(bx + n, by + n);
        if (nw and se and !ne and !sw)
        {
          fill(bx + n, by);
          fill(bx, by + n);
          touched = true;
        }
        else if (ne and sw and !nw and !se)
        {
          fill(bx, by);
          fill(bx + n, by + n);
          touched = true;
        }
      }
    }
    return touched;
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
  /// One pass of the vote, with the lattice starting at (ox, oy)
  void snap_at(int n, int ox, int oy)
  {
    for (int by = oy - n; by < e_.height; by += n)
    {
      for (int bx = ox - n; bx < e_.width; bx += n)
      {
        int inside = 0;
        int cells = 0;
        for (int dy = std::max(by, 0); dy < by + n and dy < e_.height; ++dy)
        {
          for (int dx = std::max(bx, 0); dx < bx + n and dx < e_.width; ++dx)
          {
            inside += (a_[e_.index(dx, dy)] != 0U) ? 1 : 0;
            ++cells;
          }
        }
        if (cells == 0)
        {
          continue;
        }
        // A tie goes to the terrace, so a plateau rounds outward rather than losing its edge
        const auto vote = static_cast<std::uint8_t>((inside * 2 >= cells) ? 1 : 0);
        for (int dy = std::max(by, 0); dy < by + n and dy < e_.height; ++dy)
        {
          for (int dx = std::max(bx, 0); dx < bx + n and dx < e_.width; ++dx)
          {
            a_[e_.index(dx, dy)] = vote;
          }
        }
      }
    }
  }

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
  memory::Vector<std::uint8_t> c_;
  int phase_x_ = 0;
  int phase_y_ = 0;
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
  // The lattice is anchored at the grid's own origin. Choosing the phase per terrace was tried and
  // is worse: each one picks the phase that suits it alone, and terraces on different phases do not
  // nest squarely, which leaves the repair below cascading again. A feature exactly min_feature
  // wide that lies across the blocks rather than along them can still be voted away; that is the
  // price of squaring off, and it is why min_feature is a preference rather than a guarantee.
  const int phase_x = 0;
  const int phase_y = 0;
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
        // Never narrower than the terrace above it, so the sets stay nested -- and taken in before
        // the squaring off, so the union is squared off too rather than breaking it again
        mask[i] = (static_cast<int>(levels.values[i]) >= k or above[i] != 0U) ? 1U : 0U;
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
        work.snap(n, phase_x, phase_y);
        for (int join = 0; join < kMaxRepairs and work.join_blocks(n); ++join)
        {
        }
        mask = work.data();
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
  const int bands = std::max(params.levels, 1);
  const float rate = (params.mode == TerraceMode::kRate) ? std::max(params.rate, 1e-6F) : span;
  // Each band is `skew` times the one below it. At 1 they are equal and the sums below collapse to
  // plain division, which is worth keeping exact rather than going through the logarithms
  const float skew = std::clamp(params.skew, 0.05F, 20.0F);
  const bool equal = std::abs(skew - 1.0F) < 1e-4F;
  const float log_skew = equal ? 1.0F : std::log(skew);
  // A geometric series of `bands` terms, as a fraction of the first: 1 + s + s^2 + ...
  const float series =
    equal ? static_cast<float>(bands) : (1.0F - std::pow(skew, static_cast<float>(bands))) / (1.0F - skew);
  // The lowest band's height. By count it is whatever divides the range into `bands` of this
  // shape; by rate it is given
  const float first = (params.mode == TerraceMode::kRate) ? rate : span / series;

  /// Which band a height this far above the bottom of the range falls in
  const auto band_of = [&](float above) {
    if (equal)
    {
      return static_cast<int>(std::floor(above / first));
    }
    // above = first * (1 - s^k) / (1 - s), inverted
    const float arg = 1.0F - above * (1.0F - skew) / first;
    if (!(arg > 0.0F))
    {
      // The bands converge before reaching this height, so there is no band for it
      return ceiling;
    }
    return static_cast<int>(std::floor(std::log(arg) / log_skew));
  };

  // The top of the range belongs to the last band rather than to one of its own
  const int top_band = (params.mode == TerraceMode::kRate) ? band_of(span * (1.0F - 1e-6F)) : bands - 1;
  const int highest = std::clamp(top_band, 0, ceiling);
  result.rate = first;
  result.top_rate = equal ? first : (first * std::pow(skew, static_cast<float>(highest)));
  for (std::size_t i = 0; i < e.count(); ++i)
  {
    const float above = std::clamp(height.values[i] - lo, 0.0F, span);
    const int level = std::clamp(band_of(above), 0, highest);
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
  level::Grid<TileRef>* const* layers,
  std::size_t layer_count,
  level::MaskGrid* collision)
{
  StepFillResult result;
  const level::Extents e = levels.extents;
  if (e.count() == 0)
  {
    return result;
  }
  LVL_ASSERT_TRUE(collision == nullptr or collision->extents == e) << "the collision layer is a different size";

  // Collision comes from the levels themselves, not from any one terrace, so it stays a single
  // flat mask however many layers the tiles are spread over
  if (collision != nullptr and params.collision != StepCollision::kNone)
  {
    for (int y = 0; y < e.height; ++y)
    {
      for (int x = 0; x < e.width; ++x)
      {
        const std::size_t i = e.index(x, y);
        const int here = static_cast<int>(levels.values[i]);
        bool down = false;
        bool up = false;
        for (const Direction d : level::kStepNeighbors)
        {
          const Cell c = step(Cell{x, y}, d);
          const int there = e.contains(c) ? static_cast<int>(levels.values[e.index(c.x, c.y)]) : here;
          down = down or (there < here);
          up = up or (there > here);
        }
        // Set where this cell is on the blocked side of a step, cleared where it is not, so a
        // re-run after the heightmap changes leaves no walls where the cliff has gone
        const bool block = (params.collision == StepCollision::kAbove) ? down : up;
        const std::uint8_t mask = collision->values[i];
        collision->values[i] =
          static_cast<std::uint8_t>(block ? (mask | level::collision::kSolid) : (mask & ~level::collision::kSolid));
        result.blocked += block ? 1U : 0U;
      }
    }
  }

  // A cell whose own level no terrain has art for is left alone and counted
  for (std::size_t i = 0; i < e.count(); ++i)
  {
    result.untermed +=
      (level::step_terrain_at(rules, static_cast<int>(levels.values[i])) == level::kNoStepTerrain) ? 1U : 0U;
  }

  // One layer per height level: layer k is the ground at level k and above, drawn in the art of
  // whichever terrain's band holds level k
  for (std::size_t k = 0; k < layer_count; ++k)
  {
    level::Grid<TileRef>* grid = layers[k];
    if (grid == nullptr)
    {
      continue;
    }
    LVL_ASSERT_TRUE(grid->extents == e) << "a step layer is a different size to the level layer";
    const auto floor = static_cast<int>(k);
    const StepTerrainId id = level::step_terrain_at(rules, floor);
    const auto inside = [&](Cell c) {
      // Off the grid counts as part of the same ground, so the map's edge is not a cliff
      return !e.contains(c) or static_cast<int>(levels.values[e.index(c.x, c.y)]) >= floor;
    };
    for (int y = 0; y < e.height; ++y)
    {
      for (int x = 0; x < e.width; ++x)
      {
        const std::size_t i = e.index(x, y);
        if (static_cast<int>(levels.values[i]) < floor or id == level::kNoStepTerrain)
        {
          // Below this terrace, or a level with no art: clear it, so the layer holds only what it
          // draws and a re-run leaves nothing behind
          result.changed += grid->values[i].empty() ? 0U : 1U;
          grid->values[i] = level::kEmptyTile;
          continue;
        }
        level::StepCode code = 0;
        bool drops = false;
        for (std::size_t n = 0; n < level::kStepNeighbors.size(); ++n)
        {
          const bool below = !inside(step(Cell{x, y}, level::kStepNeighbors[n]));
          code = level::step_code_with(code, n, below ? StepRelation::kBelow : StepRelation::kLevel);
          drops = drops or below;
        }
        const StepType type = level::step_type_of(code);
        // Ground dropping away that still comes out flat is a shape the table has no tile for: a
        // one-cell spur or a 45-degree edge, which tidy_levels is there to square off
        result.unexpressed += (drops and type == StepType::kFlat) ? 1U : 0U;
        const TileRef tile = rules.terrains[id].tiles[static_cast<std::size_t>(type)];
        if (tile.empty())
        {
          ++result.missing;
          continue;
        }
        ++result.tiled;
        result.changed += (grid->values[i] == tile) ? 0U : 1U;
        grid->values[i] = tile;
      }
    }
  }
  return result;
}

}  // namespace lvl::procgen
