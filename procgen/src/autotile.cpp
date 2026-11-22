/**
 * @copyright 2026-present Brian Cairl
 *
 * @file autotile.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

// LVL
#include "lvl/core/random.hpp"
#include "lvl/procgen/autotile.hpp"

namespace lvl::procgen
{
namespace
{

using level::AutoTerrain;
using level::Cell;
using level::Direction;
using level::Extents;
using level::NeighborMask;
using level::TileRef;
using level::WfcRules;
using level::WfcTileId;

/// Rule tile of a cell: not computed yet, or none (an empty cell, or a tile the rules do not know)
constexpr std::uint16_t kUnknown = 0xFFFFU;
constexpr std::uint16_t kNone = 0xFFFEU;

/// A cell of the working box the user painted (or that holds a painted tile): never rewritten
constexpr std::uint8_t kLocked = 1U;

constexpr float kEpsilon = 1e-4F;

/// One neighbor's constraint on a cell
struct Constraint
{
  Direction direction;
  std::uint16_t id;
  float weight;
};

/// A cell of the band that may be re-filled
struct Candidate
{
  /// Index within the band
  std::uint32_t cell;
  /// Cells from the nearest painted cell
  std::uint16_t distance;
  /// Tile that was there before the re-fill (to report what changed)
  TileRef before;
};

/// The band of cells around the painted ones, and what is known about each
class Patch
{
public:
  Patch(
    const WfcRules& rules,
    const AutoTileParams& params,
    level::Grid<TileRef>& tiles,
    Extents box,
    Cell corner,
    memory::Arena& scratch) :
      rules_{rules},
      tiles_{tiles},
      n_{rules.tiles.size()},
      box_{box},
      corner_{corner},
      locked_weight_{std::max(params.locked_weight, 1.0F)},
      allow_{memory::make_vector<std::uint8_t>(scratch, 8 * n_ * n_, std::uint8_t{0})},
      ids_{memory::make_vector<std::uint16_t>(scratch, box.count(), kUnknown)},
      flags_{memory::make_vector<std::uint8_t>(scratch, box.count(), std::uint8_t{0})},
      distance_{memory::make_vector<std::uint16_t>(scratch, box.count(), kUnknown)},
      cells_{memory::make_vector<Candidate>(scratch)}
  {
    for (const auto& adj : rules.adjacency)
    {
      if (adj.a < n_ and adj.b < n_ and (rules.diagonals or !level::diagonal(adj.direction)))
      {
        allow_[index(adj.direction, adj.a, adj.b)] = 1U;
        allow_[index(level::opposite(adj.direction), adj.b, adj.a)] = 1U;
      }
    }
  }

  /// Marks a cell as painted: locked, and the center the band is measured from
  void paint(Cell c, int radius)
  {
    for (int y = c.y - radius; y <= c.y + radius; ++y)
    {
      for (int x = c.x - radius; x <= c.x + radius; ++x)
      {
        if (inside(Cell{x, y}))
        {
          const std::size_t i = local(Cell{x, y});
          const auto d = static_cast<std::uint16_t>(std::max(std::abs(x - c.x), std::abs(y - c.y)));
          distance_[i] = std::min(distance_[i], d);
        }
      }
    }
    flags_[local(c)] |= kLocked;
  }

  /// Locks every cell of the band holding <code>tile</code> (a tile the user painted)
  void lock_tile(TileRef tile)
  {
    for (std::size_t i = 0; i < box_.count(); ++i)
    {
      if (tiles_.at(global(i)) == tile)
      {
        flags_[i] |= kLocked;
      }
    }
  }

  /// Collects the cells to re-fill, nearest the painted ones first
  void collect(int radius)
  {
    for (std::size_t i = 0; i < box_.count(); ++i)
    {
      if (distance_[i] > static_cast<std::uint16_t>(radius) or (flags_[i] & kLocked) != 0U)
      {
        continue;
      }
      // A tile the rules do not know is left alone (hand-placed decoration)
      const TileRef tile = tiles_.at(global(i));
      if (!tile.empty() and id_of(tile) == kNone)
      {
        continue;
      }
      cells_.push_back(Candidate{.cell = static_cast<std::uint32_t>(i), .distance = distance_[i], .before = tile});
    }
    // Nearest the painted cells first, so each sweep carries the rules one cell further outward
    std::stable_sort(
      cells_.begin(), cells_.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
  }

  /// One relaxation sweep; returns how many cells it rewrote
  std::size_t sweep()
  {
    std::size_t changed = 0;
    for (const Candidate& candidate : cells_)
    {
      const std::uint16_t current = id(candidate.cell);
      const std::uint16_t best = choose(global(candidate.cell), current);
      if (best != kNone and best != current)
      {
        tiles_.at(global(candidate.cell)) = rules_.tiles[best].tile;
        ids_[candidate.cell] = best;
        ++changed;
      }
    }
    return changed;
  }

  /// Cells whose tile differs from the one that was there, and cells left violating a rule
  void tally(AutoTileResult& result)
  {
    result.considered = cells_.size();
    for (const Candidate& candidate : cells_)
    {
      const std::uint32_t i = candidate.cell;
      result.changed += (tiles_.at(global(i)) == candidate.before) ? 0U : 1U;
      const std::uint16_t here = id(i);
      bool violated = (here == kNone);
      for (const Constraint& c : neighbors(global(i)))
      {
        violated = violated or (here != kNone and allow_[index(c.direction, here, c.id)] == 0U);
      }
      result.unresolved += violated ? 1U : 0U;
    }
  }

private:
  [[nodiscard]] bool inside(Cell c) const
  {
    return c.x >= corner_.x and c.y >= corner_.y and c.x < corner_.x + box_.width and c.y < corner_.y + box_.height;
  }
  [[nodiscard]] std::size_t local(Cell c) const { return box_.index(c.x - corner_.x, c.y - corner_.y); }
  [[nodiscard]] Cell global(std::size_t i) const
  {
    const Cell c = box_.cell(i);
    return Cell{c.x + corner_.x, c.y + corner_.y};
  }
  [[nodiscard]] std::size_t index(Direction d, std::uint16_t a, std::uint16_t b) const
  {
    return (static_cast<std::size_t>(d) * n_ + a) * n_ + b;
  }
  [[nodiscard]] std::uint16_t id_of(TileRef tile) const
  {
    if (tile.empty())
    {
      return kNone;
    }
    const WfcTileId id = level::find_tile(rules_, tile);
    return (id < n_) ? static_cast<std::uint16_t>(id) : kNone;
  }
  /// Rule tile of a cell of the band (cached)
  std::uint16_t id(std::size_t i)
  {
    if (ids_[i] == kUnknown)
    {
      ids_[i] = id_of(tiles_.at(global(i)));
    }
    return ids_[i];
  }

  /// Neighbor directions the rules constrain (the corners only when they are enabled)
  [[nodiscard]] std::size_t directions() const { return rules_.diagonals ? 8U : 4U; }

  /// Fills <code>buffer_</code> with the neighbors that constrain <code>c</code>
  std::span<const Constraint> neighbors(Cell c)
  {
    std::size_t count = 0;
    for (std::size_t k = 0; k < directions(); ++k)
    {
      // Direction's four sides are 0..3 and its corners 4..7, so this is the sides, then the corners
      const auto d = static_cast<Direction>(k);
      const Cell nc = level::step(c, d);
      if (!tiles_.extents.contains(nc))
      {
        continue;
      }
      const bool in_box = inside(nc);
      const std::uint16_t nid = in_box ? id(local(nc)) : id_of(tiles_.at(nc));
      if (nid == kNone)
      {
        continue;
      }
      const bool locked = in_box and (flags_[local(nc)] & kLocked) != 0U;
      buffer_[count++] = Constraint{.direction = d, .id = nid, .weight = locked ? locked_weight_ : 1.0F};
    }
    return std::span<const Constraint>{buffer_.data(), count};
  }

  /// How well tile <code>t</code> sits in a cell: its neighbors' adjacencies, for and against
  [[nodiscard]] float score(std::uint16_t t, std::span<const Constraint> around) const
  {
    float total = 0.0F;
    for (const Constraint& c : around)
    {
      total += (allow_[index(c.direction, t, c.id)] != 0U) ? c.weight : -c.weight;
    }
    return total;
  }

  /// The rule tile that fits a cell best, or <code>current</code> when nothing beats it
  std::uint16_t choose(Cell c, std::uint16_t current)
  {
    const std::span<const Constraint> around = neighbors(c);
    const float keep = (current == kNone) ? -std::numeric_limits<float>::max() : score(current, around);
    std::uint16_t best = kNone;
    float best_score = 0.0F;
    for (std::size_t t = 0; t < n_; ++t)
    {
      if (rules_.tiles[t].weight <= 0.0F or t == current)
      {
        continue;
      }
      const auto id = static_cast<std::uint16_t>(t);
      const float s = score(id, around);
      // Ties go to the more frequent tile, then to the lower id, so the result is deterministic
      if (
        best == kNone or s > best_score + kEpsilon or
        (s > best_score - kEpsilon and rules_.tiles[t].weight > rules_.tiles[best].weight + kEpsilon))
      {
        best = id;
        best_score = s;
      }
    }
    // The tile already there wins a tie, so a finished area is left as it is
    return (best != kNone and best_score > keep + kEpsilon) ? best : current;
  }

  const WfcRules& rules_;
  level::Grid<TileRef>& tiles_;
  std::size_t n_;
  Extents box_;
  Cell corner_;
  float locked_weight_;
  memory::Vector<std::uint8_t> allow_;
  memory::Vector<std::uint16_t> ids_;
  memory::Vector<std::uint8_t> flags_;
  memory::Vector<std::uint16_t> distance_;
  memory::Vector<Candidate> cells_;
  std::array<Constraint, 8> buffer_ = {};
};

/// Rectangle of cells a stroke can affect, clipped to the grid
struct Window
{
  Cell corner;
  Extents extents;

  [[nodiscard]] bool holds(Cell c) const
  {
    return c.x >= corner.x and c.y >= corner.y and c.x < corner.x + extents.width and c.y < corner.y + extents.height;
  }
  [[nodiscard]] std::size_t index(Cell c) const { return extents.index(c.x - corner.x, c.y - corner.y); }
  [[nodiscard]] Cell cell(std::size_t i) const
  {
    const Cell c = extents.cell(i);
    return Cell{c.x + corner.x, c.y + corner.y};
  }
};

/// The cells within <code>radius</code> of any painted cell, clipped to the grid (empty if none are on it)
Window window_around(const Cell* painted, std::size_t count, Extents grid, int radius)
{
  Cell lo{grid.width, grid.height};
  Cell hi{-1, -1};
  for (std::size_t i = 0; i < count; ++i)
  {
    if (grid.contains(painted[i]))
    {
      lo = Cell{std::min(lo.x, painted[i].x), std::min(lo.y, painted[i].y)};
      hi = Cell{std::max(hi.x, painted[i].x), std::max(hi.y, painted[i].y)};
    }
  }
  if (hi.x < 0)
  {
    return Window{Cell{0, 0}, Extents{0, 0}};
  }
  const Cell corner{std::max(0, lo.x - radius), std::max(0, lo.y - radius)};
  const Cell far{std::min(grid.width - 1, hi.x + radius), std::min(grid.height - 1, hi.y + radius)};
  return Window{corner, Extents{far.x - corner.x + 1, far.y - corner.y + 1}};
}

}  // namespace

TerrainPaintResult paint_terrain(
  const level::AutoTileRules& rules,
  level::TerrainId terrain,
  const Cell* painted,
  std::size_t count,
  level::Grid<TileRef>& tiles,
  memory::Arena& scratch)
{
  TerrainPaintResult result;
  const Extents grid = tiles.extents;
  if (count == 0 or terrain >= rules.terrains.size() or grid.count() == 0)
  {
    return result;
  }
  const AutoTerrain& t = rules.terrains[terrain];
  const int expand = std::clamp(t.expand, 0, 4);
  // Cells whose tile may change, and the cells whose membership those depend on
  const int reach = std::max(1, 2 * expand);
  const Window window = window_around(painted, count, grid, 3 * expand + 2);
  if (window.extents.count() == 0)
  {
    return result;
  }
  // A cell is core where the user painted, or where it already holds the terrain's middle; with no
  // border to grow, every one of the terrain's tiles is core
  // Any of the fully-surrounded tiles marks a cell as core
  const auto is_middle = [&t](TileRef tile) {
    for (const level::AutoVariant& v : t.variants)
    {
      if (v.mask == level::kAllNeighbors and v.tile == tile)
      {
        return true;
      }
    }
    return false;
  };
  memory::Scope scope{scratch};
  auto core = memory::make_vector<std::uint8_t>(scratch, window.extents.count(), std::uint8_t{0});
  for (std::size_t i = 0; i < window.extents.count(); ++i)
  {
    const TileRef tile = tiles.at(window.cell(i));
    const bool mine = (expand > 0) ? is_middle(tile) : (terrain_of(rules, tile) == terrain);
    core[i] = mine ? 1U : 0U;
  }
  for (std::size_t i = 0; i < count; ++i)
  {
    if (window.holds(painted[i]))
    {
      core[window.index(painted[i])] = 1U;
    }
  }
  // The terrain covers the core grown by expand
  auto covered = memory::make_vector<std::uint8_t>(scratch, window.extents.count(), std::uint8_t{0});
  for (std::size_t i = 0; i < window.extents.count(); ++i)
  {
    const Cell c = window.cell(i);
    bool near_core = false;
    for (int y = c.y - expand; y <= c.y + expand and !near_core; ++y)
    {
      for (int x = c.x - expand; x <= c.x + expand and !near_core; ++x)
      {
        near_core = window.holds(Cell{x, y}) and core[window.index(Cell{x, y})] != 0U;
      }
    }
    covered[i] = near_core ? 1U : 0U;
  }
  // Off the grid counts as covered, so a terrain running off the map has no border along its edge
  const auto covers = [&](Cell c) { return !grid.contains(c) or (window.holds(c) and covered[window.index(c)] != 0U); };
  for (std::size_t i = 0; i < count; ++i)
  {
    if (!grid.contains(painted[i]))
    {
      continue;
    }
    for (int y = painted[i].y - reach; y <= painted[i].y + reach; ++y)
    {
      for (int x = painted[i].x - reach; x <= painted[i].x + reach; ++x)
      {
        const Cell c{x, y};
        if (!grid.contains(c) or !window.holds(c) or covered[window.index(c)] == 0U)
        {
          continue;
        }
        // Already given a tile by an earlier painted cell of this stroke
        if (covered[window.index(c)] == 2U)
        {
          continue;
        }
        covered[window.index(c)] = 2U;
        NeighborMask mask = level::kNoNeighbors;
        for (const Direction d : level::kNeighbors)
        {
          if (covers(level::step(c, d)))
          {
            mask = static_cast<NeighborMask>(mask | level::mask_of(d));
          }
        }
        // A pattern with several tiles picks by cell, so repainting an area never re-rolls it
        const TileRef tile = resolve(t, mask, random::hash(terrain, c.x, c.y));
        if (tile.empty())
        {
          ++result.missing;
          continue;
        }
        ++result.covered;
        result.changed += (tiles.at(c) == tile) ? 0U : 1U;
        tiles.at(c) = tile;
      }
    }
  }
  return result;
}

AutoTileResult autotile(
  const WfcRules& rules,
  const Cell* painted,
  std::size_t count,
  const AutoTileParams& params,
  level::Grid<TileRef>& tiles,
  memory::Arena& scratch)
{
  AutoTileResult result;
  const Extents e = tiles.extents;
  const int radius = params.radius;
  if (count == 0 or rules.tiles.empty() or radius <= 0 or e.count() == 0)
  {
    return result;
  }
  // The band around the painted cells is all this has to look at
  Cell lo{e.width, e.height};
  Cell hi{-1, -1};
  for (std::size_t i = 0; i < count; ++i)
  {
    if (e.contains(painted[i]))
    {
      lo = Cell{std::min(lo.x, painted[i].x), std::min(lo.y, painted[i].y)};
      hi = Cell{std::max(hi.x, painted[i].x), std::max(hi.y, painted[i].y)};
    }
  }
  if (hi.x < 0)
  {
    return result;
  }
  const Cell corner{std::max(0, lo.x - radius), std::max(0, lo.y - radius)};
  const Cell far{std::min(e.width - 1, hi.x + radius), std::min(e.height - 1, hi.y + radius)};
  const Extents box{far.x - corner.x + 1, far.y - corner.y + 1};

  memory::Scope scope{scratch};
  Patch patch{rules, params, tiles, box, corner, scratch};
  for (std::size_t i = 0; i < count; ++i)
  {
    if (e.contains(painted[i]))
    {
      patch.paint(painted[i], radius);
    }
  }
  // Cells holding a painted tile are centers too: earlier strokes keep theirs
  for (std::size_t i = 0; i < count; ++i)
  {
    if (!e.contains(painted[i]))
    {
      continue;
    }
    const TileRef tile = tiles.at(painted[i]);
    bool seen = false;
    for (std::size_t j = 0; j < i; ++j)
    {
      seen = seen or (e.contains(painted[j]) and tiles.at(painted[j]) == tile);
    }
    if (!seen and !tile.empty())
    {
      patch.lock_tile(tile);
    }
  }
  patch.collect(radius);
  for (int i = 0; i < std::max(1, params.iterations); ++i)
  {
    if (patch.sweep() == 0)
    {
      break;
    }
  }
  patch.tally(result);
  return result;
}

}  // namespace lvl::procgen
