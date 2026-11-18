/**
 * @copyright 2026-present Brian Cairl
 *
 * @file wfc.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

// LVL
#include "lvl/core/random.hpp"
#include "lvl/procgen/wfc.hpp"

namespace lvl::procgen
{
namespace
{

using level::Cell;
using level::Direction;
using level::Extents;
using level::TileRef;
using level::WaterRule;
using level::WfcRules;
using Word = std::uint64_t;

constexpr std::uint32_t kUnset = 0xFFFFFFFFU;

struct HeapEntry
{
  float entropy;
  std::uint32_t cell;
};

struct HeapGreater
{
  bool operator()(const HeapEntry& a, const HeapEntry& b) const { return a.entropy > b.entropy; }
};

/// Solver state; all arrays in the scratch arena
class Solver
{
public:
  Solver(
    const WfcRules& rules,
    const WfcConstraints& constraints,
    const WfcParams& params,
    const level::Grid<TileRef>& out,
    memory::Arena& scratch) :
      rules_{rules},
      constraints_{constraints},
      params_{params},
      extents_{out.extents},
      tiles_{rules.tiles.size()},
      words_{(rules.tiles.size() + 63) / 64},
      compat_{memory::make_vector<Word>(scratch, 8 * tiles_ * words_, Word{0})},
      initial_{memory::make_vector<Word>(scratch, extents_.count() * words_, Word{0})},
      wave_{memory::make_vector<Word>(scratch, extents_.count() * words_, Word{0})},
      in_region_{memory::make_vector<std::uint8_t>(scratch, extents_.count(), std::uint8_t{0})},
      chosen_{memory::make_vector<std::uint32_t>(scratch, extents_.count(), kUnset)},
      stack_{memory::make_vector<std::uint32_t>(scratch)},
      heap_{memory::make_vector<HeapEntry>(scratch)},
      log_weights_{memory::make_vector<float>(scratch, tiles_, 0.0F)},
      support_{memory::make_vector<Word>(scratch, words_, Word{0})}
  {
    region_ = params.region;
    if (region_.width <= 0 or region_.height <= 0)
    {
      region_ = Region{0, 0, extents_.width, extents_.height};
    }
    region_.x = std::clamp(region_.x, 0, extents_.width);
    region_.y = std::clamp(region_.y, 0, extents_.height);
    region_.width = std::clamp(region_.width, 0, extents_.width - region_.x);
    region_.height = std::clamp(region_.height, 0, extents_.height - region_.y);
    for (std::size_t t = 0; t < tiles_; ++t)
    {
      const float w = std::max(rules.tiles[t].weight, 1e-6F);
      log_weights_[t] = w * std::log(w);
    }
    for (const auto& a : rules.adjacency)
    {
      if (a.a < tiles_ and a.b < tiles_ and (rules.diagonals or !level::diagonal(a.direction)))
      {
        set(compat(a.direction, a.a), a.b);
        set(compat(level::opposite(a.direction), a.b), a.a);
      }
    }
    stack_.reserve(extents_.count());
    heap_.reserve(4 * extents_.count());
  }

  /// Builds every cell's initial domain from the layer constraints; false (with status) if impossible
  Status initialize()
  {
    const auto& c = constraints_;
    const float range = std::max(c.height_hi - c.height_lo, 1e-6F);
    for (int y = 0; y < extents_.height; ++y)
    {
      for (int x = 0; x < extents_.width; ++x)
      {
        const std::size_t i = extents_.index(x, y);
        const bool inside =
          x >= region_.x and y >= region_.y and x < region_.x + region_.width and y < region_.y + region_.height;
        in_region_[i] = inside ? 1 : 0;
        Word* dom = initial(i);
        // A rule tile already placed: fixed (inside the region only if keep_existing)
        if (c.existing != nullptr and (!inside or params_.keep_existing))
        {
          const TileRef existing = c.existing->values[i];
          if (const auto id = level::find_tile(rules_, existing); !existing.empty() and id < tiles_)
          {
            set(dom, id);
            in_region_[i] = inside ? 2 : 3;  // 2/3: fixed inside/outside
            continue;
          }
        }
        if (!inside)
        {
          continue;
        }
        const float h = (c.height != nullptr) ? (c.height->values[i] - c.height_lo) / range : 0.5F;
        const bool wet = (c.water != nullptr) and c.water->values[i] > c.water_threshold;
        bool shore = false;
        if (c.water != nullptr and !wet)
        {
          for (const Direction d : level::kDirections)
          {
            const Cell n = level::step(Cell{x, y}, d);
            shore = shore or (extents_.contains(n) and c.water->at(n) > c.water_threshold);
          }
        }
        const std::uint16_t semantic = (c.semantic != nullptr) ? c.semantic->values[i] : 0;
        for (std::size_t t = 0; t < tiles_; ++t)
        {
          const auto& tile = rules_.tiles[t];
          if (tile.weight <= 0.0F)
          {
            continue;
          }
          if (c.height != nullptr and (h < tile.height_min or h > tile.height_max))
          {
            continue;
          }
          if (
            c.water != nullptr and
            ((tile.water == WaterRule::kDry and wet) or (tile.water == WaterRule::kWet and !wet) or
             (tile.water == WaterRule::kShore and !shore)))
          {
            continue;
          }
          if (c.semantic != nullptr and tile.semantic != level::kNoSemantic and tile.semantic != semantic)
          {
            continue;
          }
          set(dom, t);
        }
        if (empty(dom))
        {
          return Status::error("no WFC tile satisfies the layer constraints at cell (%d, %d)", x, y);
        }
      }
    }
    return Status::okay();
  }

  /// One full attempt; true on success
  bool attempt(std::uint64_t seed, WfcResult& result, jobs::Progress* progress)
  {
    std::memcpy(wave_.data(), initial_.data(), wave_.size() * sizeof(Word));
    std::fill(chosen_.begin(), chosen_.end(), kUnset);
    rng_ = random::seeded(seed);
    heap_.clear();
    stack_.clear();
    std::size_t open = 0;
    for (std::size_t i = 0; i < extents_.count(); ++i)
    {
      if (in_region_[i] == 1)
      {
        ++open;
        push_entropy(i);
      }
      if (in_region_[i] != 0)
      {
        stack_.push_back(static_cast<std::uint32_t>(i));
      }
    }
    if (progress != nullptr)
    {
      progress->reset(open + 1);
    }
    if (!propagate())
    {
      ++result.contradictions;
      return false;
    }
    while (true)
    {
      if (progress != nullptr and progress->cancelled())
      {
        return false;
      }
      const std::uint32_t cell = next_cell();
      if (cell == kUnset)
      {
        break;
      }
      collapse(cell);
      ++result.collapsed;
      if (progress != nullptr)
      {
        progress->advance();
      }
      stack_.push_back(cell);
      if (!propagate())
      {
        ++result.contradictions;
        return false;
      }
    }
    return true;
  }

  /// Writes the solution into the region of <code>out</code>
  void write(level::Grid<TileRef>& out) const
  {
    for (std::size_t i = 0; i < extents_.count(); ++i)
    {
      if (in_region_[i] == 1 or in_region_[i] == 2)
      {
        const std::size_t t = first(wave(i));
        if (t < tiles_)
        {
          out.values[i] = rules_.tiles[t].tile;
        }
      }
    }
  }

private:
  Word* compat(Direction d, std::size_t t)
  {
    return compat_.data() + (static_cast<std::size_t>(d) * tiles_ + t) * words_;
  }
  const Word* compat(Direction d, std::size_t t) const
  {
    return compat_.data() + (static_cast<std::size_t>(d) * tiles_ + t) * words_;
  }
  Word* initial(std::size_t i) { return initial_.data() + i * words_; }
  Word* wave(std::size_t i) { return wave_.data() + i * words_; }
  const Word* wave(std::size_t i) const { return wave_.data() + i * words_; }

  static void set(Word* bits, std::size_t t) { bits[t / 64] |= Word{1} << (t % 64); }

  bool empty(const Word* bits) const
  {
    for (std::size_t w = 0; w < words_; ++w)
    {
      if (bits[w] != 0)
      {
        return false;
      }
    }
    return true;
  }

  std::size_t count(const Word* bits) const
  {
    std::size_t n = 0;
    for (std::size_t w = 0; w < words_; ++w)
    {
      n += static_cast<std::size_t>(std::popcount(bits[w]));
    }
    return n;
  }

  std::size_t first(const Word* bits) const
  {
    for (std::size_t w = 0; w < words_; ++w)
    {
      if (bits[w] != 0)
      {
        return w * 64 + static_cast<std::size_t>(std::countr_zero(bits[w]));
      }
    }
    return tiles_;
  }

  template <typename Fn> void for_each(const Word* bits, Fn&& fn) const
  {
    for (std::size_t w = 0; w < words_; ++w)
    {
      for (Word b = bits[w]; b != 0; b &= b - 1)
      {
        fn(w * 64 + static_cast<std::size_t>(std::countr_zero(b)));
      }
    }
  }

  float entropy(std::size_t i) const
  {
    float sum = 0.0F;
    float sum_log = 0.0F;
    for_each(wave(i), [&](std::size_t t) {
      sum += std::max(rules_.tiles[t].weight, 1e-6F);
      sum_log += log_weights_[t];
    });
    return std::log(sum) - sum_log / sum;
  }

  void push_entropy(std::size_t i)
  {
    // Lazy heap: stale entries are skipped when popped; rebuild if it grows too large
    if (heap_.size() >= heap_.capacity())
    {
      heap_.clear();
      for (std::size_t k = 0; k < extents_.count(); ++k)
      {
        if (in_region_[k] == 1 and chosen_[k] == kUnset and count(wave(k)) > 1)
        {
          heap_.push_back(HeapEntry{entropy(k) + 1e-4F * random::unit(rng_), static_cast<std::uint32_t>(k)});
        }
      }
      std::make_heap(heap_.begin(), heap_.end(), HeapGreater{});
      return;
    }
    heap_.push_back(HeapEntry{entropy(i) + 1e-4F * random::unit(rng_), static_cast<std::uint32_t>(i)});
    std::push_heap(heap_.begin(), heap_.end(), HeapGreater{});
  }

  /// Lowest-entropy open cell, or kUnset when all are decided
  std::uint32_t next_cell()
  {
    while (!heap_.empty())
    {
      std::pop_heap(heap_.begin(), heap_.end(), HeapGreater{});
      const HeapEntry e = heap_.back();
      heap_.pop_back();
      if (chosen_[e.cell] != kUnset)
      {
        continue;
      }
      const std::size_t n = count(wave(e.cell));
      if (n == 1)
      {
        chosen_[e.cell] = static_cast<std::uint32_t>(first(wave(e.cell)));
        continue;
      }
      // Stale if the domain changed since this entry was pushed (a newer one exists)
      if (std::abs(entropy(e.cell) - e.entropy) > 2e-4F)
      {
        continue;
      }
      return e.cell;
    }
    // Any remaining open cells are decided by propagation
    for (std::size_t i = 0; i < extents_.count(); ++i)
    {
      if (in_region_[i] == 1 and chosen_[i] == kUnset)
      {
        if (count(wave(i)) == 1)
        {
          chosen_[i] = static_cast<std::uint32_t>(first(wave(i)));
        }
        else
        {
          return static_cast<std::uint32_t>(i);
        }
      }
    }
    return kUnset;
  }

  void collapse(std::uint32_t i)
  {
    Word* dom = wave(i);
    float total = 0.0F;
    for_each(dom, [&](std::size_t t) { total += std::max(rules_.tiles[t].weight, 1e-6F); });
    float pick = random::unit(rng_) * total;
    std::size_t chosen = first(dom);
    for_each(dom, [&](std::size_t t) {
      if (pick >= 0.0F)
      {
        chosen = t;
      }
      pick -= std::max(rules_.tiles[t].weight, 1e-6F);
    });
    std::fill(dom, dom + words_, Word{0});
    set(dom, chosen);
    chosen_[i] = static_cast<std::uint32_t>(chosen);
  }

  /// Arc-consistency propagation from the cells on the stack; false on contradiction
  bool propagate()
  {
    while (!stack_.empty())
    {
      const std::uint32_t i = stack_.back();
      stack_.pop_back();
      const Cell c = extents_.cell(i);
      for (const Direction d : level::kNeighbors)
      {
        if (!rules_.diagonals and level::diagonal(d))
        {
          continue;
        }
        const Cell nc = level::step(c, d);
        if (!extents_.contains(nc))
        {
          continue;
        }
        const std::size_t ni = extents_.index(nc);
        if (in_region_[ni] == 0)
        {
          continue;
        }
        // Tiles the neighbor may hold given everything still possible here
        std::fill(support_.begin(), support_.end(), Word{0});
        for_each(wave(i), [&](std::size_t t) {
          const Word* allowed = compat(d, t);
          for (std::size_t w = 0; w < words_; ++w)
          {
            support_[w] |= allowed[w];
          }
        });
        Word* nd = wave(ni);
        bool changed = false;
        bool any = false;
        for (std::size_t w = 0; w < words_; ++w)
        {
          const Word next = nd[w] & support_[w];
          changed = changed or (next != nd[w]);
          any = any or (next != 0);
          nd[w] = next;
        }
        if (!any)
        {
          return false;
        }
        if (changed)
        {
          stack_.push_back(static_cast<std::uint32_t>(ni));
          if (in_region_[ni] == 1 and chosen_[ni] == kUnset)
          {
            push_entropy(ni);
          }
        }
      }
    }
    return true;
  }

  const WfcRules& rules_;
  const WfcConstraints& constraints_;
  const WfcParams& params_;
  Extents extents_;
  Region region_;
  std::size_t tiles_;
  std::size_t words_;
  memory::Vector<Word> compat_;
  memory::Vector<Word> initial_;
  memory::Vector<Word> wave_;
  /// 0 = outside, 1 = open inside, 2 = fixed inside, 3 = fixed outside
  memory::Vector<std::uint8_t> in_region_;
  memory::Vector<std::uint32_t> chosen_;
  memory::Vector<std::uint32_t> stack_;
  memory::Vector<HeapEntry> heap_;
  memory::Vector<float> log_weights_;
  memory::Vector<Word> support_;
  random::State rng_;
};

}  // namespace

WfcResult solve(
  const WfcRules& rules,
  const WfcConstraints& constraints,
  const WfcParams& params,
  level::Grid<TileRef>& out,
  memory::Arena& scratch,
  jobs::Progress* progress)
{
  WfcResult result;
  if (rules.tiles.empty())
  {
    result.status = Status::error("the WFC rules have no tiles");
    return result;
  }
  if (out.extents.count() == 0)
  {
    result.status = Status::error("nothing to generate (empty grid)");
    return result;
  }
  memory::Scope scope{scratch};
  if (progress != nullptr)
  {
    progress->stage("Wave function collapse");
  }
  Solver solver{rules, constraints, params, out, scratch};
  if (result.status = solver.initialize(); !result.status.ok)
  {
    return result;
  }
  const int attempts = std::max(1, params.max_attempts);
  for (int a = 0; a < attempts; ++a)
  {
    ++result.attempts;
    if (solver.attempt(params.seed + static_cast<std::uint64_t>(a) * 0x9E3779B97F4A7C15ULL, result, progress))
    {
      solver.write(out);
      result.status = Status::okay();
      return result;
    }
    if (progress != nullptr and progress->cancelled())
    {
      result.status = Status::error("cancelled");
      return result;
    }
  }
  result.status = Status::error(
    "no solution after %d attempts (%zu contradictions); loosen the adjacency or layer constraints",
    result.attempts,
    result.contradictions);
  return result;
}

}  // namespace lvl::procgen
