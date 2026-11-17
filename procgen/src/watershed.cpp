/**
 * @copyright 2026-present Brian Cairl
 *
 * @file watershed.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>
#include <limits>

// LVL
#include "lvl/core/random.hpp"
#include "lvl/procgen/watershed.hpp"

namespace lvl::procgen
{
namespace
{

using level::Extents;
using level::Grid;

constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

/// Height and gradient at a fractional position (bilinear)
struct Sample
{
  float height;
  float gx;
  float gy;
};

Sample sample(const Grid<float>& h, float x, float y)
{
  const Extents e = h.extents;
  const int ix = std::clamp(static_cast<int>(x), 0, e.width - 2);
  const int iy = std::clamp(static_cast<int>(y), 0, e.height - 2);
  const float u = x - static_cast<float>(ix);
  const float v = y - static_cast<float>(iy);
  const float h00 = h.at(ix, iy);
  const float h10 = h.at(ix + 1, iy);
  const float h01 = h.at(ix, iy + 1);
  const float h11 = h.at(ix + 1, iy + 1);
  return Sample{
    h00 * (1 - u) * (1 - v) + h10 * u * (1 - v) + h01 * (1 - u) * v + h11 * u * v,
    (h10 - h00) * (1 - v) + (h11 - h01) * v,
    (h01 - h00) * (1 - u) + (h11 - h10) * u};
}

/// Adds <code>amount</code> spread over the four cells around (x, y) (bilinear weights)
void deposit(Grid<float>& h, float x, float y, float amount)
{
  const int ix = static_cast<int>(x);
  const int iy = static_cast<int>(y);
  const float u = x - static_cast<float>(ix);
  const float v = y - static_cast<float>(iy);
  h.at(ix, iy) += amount * (1 - u) * (1 - v);
  h.at(ix + 1, iy) += amount * u * (1 - v);
  h.at(ix, iy + 1) += amount * (1 - u) * v;
  h.at(ix + 1, iy + 1) += amount * u * v;
}

/// Removes up to <code>amount</code> within <code>radius</code> of (x, y), weighted by distance
float take(Grid<float>& h, float x, float y, float amount, int radius)
{
  const Extents e = h.extents;
  const int cx = static_cast<int>(x);
  const int cy = static_cast<int>(y);
  float weights = 0.0F;
  for (int dy = -radius; dy <= radius; ++dy)
  {
    for (int dx = -radius; dx <= radius; ++dx)
    {
      const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (d <= static_cast<float>(radius) and e.contains(cx + dx, cy + dy))
      {
        weights += static_cast<float>(radius) - d + 1e-3F;
      }
    }
  }
  float taken = 0.0F;
  for (int dy = -radius; dy <= radius; ++dy)
  {
    for (int dx = -radius; dx <= radius; ++dx)
    {
      const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (d <= static_cast<float>(radius) and e.contains(cx + dx, cy + dy))
      {
        float& cell = h.at(cx + dx, cy + dy);
        const float t = amount * (static_cast<float>(radius) - d + 1e-3F) / weights;
        cell -= t;
        taken += t;
      }
    }
  }
  return taken;
}

struct FloodNode
{
  float level;
  std::uint32_t index;
};

struct FloodGreater
{
  bool operator()(const FloodNode& a, const FloodNode& b) const
  {
    return (a.level > b.level) or (a.level == b.level and a.index > b.index);
  }
};

}  // namespace

void erode(Grid<float>& h, const ErosionParams& p, std::uint64_t seed, jobs::Progress* progress)
{
  const Extents e = h.extents;
  if (e.width < 3 or e.height < 3)
  {
    return;
  }
  auto rng = random::seeded(seed);
  const std::uint32_t report = std::max<std::uint32_t>(1, p.droplets / 100);
  for (std::uint32_t d = 0; d < p.droplets; ++d)
  {
    if (progress != nullptr and d % report == 0)
    {
      if (progress->cancelled())
      {
        return;
      }
      progress->advance();
    }
    float x = random::uniform(rng, 0.0F, static_cast<float>(e.width - 1) - 1e-3F);
    float y = random::uniform(rng, 0.0F, static_cast<float>(e.height - 1) - 1e-3F);
    float dir_x = 0.0F;
    float dir_y = 0.0F;
    float speed = 1.0F;
    float water = 1.0F;
    float sediment = 0.0F;
    for (int life = 0; life < p.max_lifetime; ++life)
    {
      const Sample s = sample(h, x, y);
      dir_x = dir_x * p.inertia - s.gx * (1.0F - p.inertia);
      dir_y = dir_y * p.inertia - s.gy * (1.0F - p.inertia);
      const float len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
      if (len < 1e-6F)
      {
        break;
      }
      dir_x /= len;
      dir_y /= len;
      const float nx = x + dir_x;
      const float ny = y + dir_y;
      if (nx < 0.0F or ny < 0.0F or nx >= static_cast<float>(e.width - 1) or ny >= static_cast<float>(e.height - 1))
      {
        break;
      }
      const float delta = sample(h, nx, ny).height - s.height;
      const float capacity = std::max(-delta * speed * water * p.capacity, p.min_capacity);
      if (sediment > capacity or delta > 0.0F)
      {
        // Uphill: fill the pit behind us; otherwise drop excess sediment
        const float amount = (delta > 0.0F) ? std::min(delta, sediment) : (sediment - capacity) * p.deposition;
        sediment -= amount;
        deposit(h, x, y, amount);
      }
      else
      {
        const float amount = std::min((capacity - sediment) * p.erosion, -delta);
        sediment += take(h, x, y, amount, std::max(1, p.radius));
      }
      speed = std::sqrt(std::max(0.0F, speed * speed + delta * p.gravity));
      water *= (1.0F - p.evaporation);
      x = nx;
      y = ny;
    }
  }
}

WatershedResult run_watershed(
  Grid<float>& height,
  const WatershedOutputs& out,
  const WatershedParams& p,
  std::uint64_t seed,
  memory::Arena& scratch,
  jobs::Progress* progress)
{
  LVL_ASSERT_NON_NULL(out.water) << "run_watershed needs a water output";
  const Extents e = height.extents;
  const std::size_t n = e.count();
  LVL_ASSERT_EQ(out.water->extents.count(), n);
  memory::Scope scope{scratch};

  const int iterations = std::max(1, p.iterations);
  if (progress != nullptr)
  {
    progress->reset(static_cast<std::size_t>(iterations) * (p.erode ? 101 : 1) + 1);
    progress->stage("Water-shedding");
  }

  auto filled = memory::make_vector<float>(scratch, n, 0.0F);
  auto downstream = memory::make_vector<std::uint32_t>(scratch, n, 0U);
  auto order = memory::make_vector<std::uint32_t>(scratch);
  order.reserve(n);
  auto visited = memory::make_vector<std::uint8_t>(scratch, n, std::uint8_t{0});
  auto cut = memory::make_vector<float>(scratch, n, 0.0F);
  auto flow = memory::make_vector<float>(scratch, n, 0.0F);
  auto rank = memory::make_vector<std::uint32_t>(scratch, n, 0U);
  auto heap = memory::make_vector<FloodNode>(scratch);
  heap.reserve(n);
  const auto push = [&heap](FloodNode node) {
    heap.push_back(node);
    std::push_heap(heap.begin(), heap.end(), FloodGreater{});
  };
  constexpr std::uint32_t kOutlet = std::numeric_limits<std::uint32_t>::max();
  constexpr float kEpsilon = 1e-5F;

  WatershedResult result;
  for (int iteration = 0; iteration < iterations; ++iteration)
  {
    if (progress != nullptr and progress->cancelled())
    {
      break;
    }
    if (p.erode)
    {
      erode(height, p.erosion, seed + static_cast<std::uint64_t>(iteration), progress);
    }

    // Priority-flood from the border: every cell gets a downstream neighbor towards an outlet
    std::fill(visited.begin(), visited.end(), std::uint8_t{0});
    std::fill(cut.begin(), cut.end(), 0.0F);
    order.clear();
    heap.clear();
    for (int y = 0; y < e.height; ++y)
    {
      for (int x = 0; x < e.width; ++x)
      {
        if (x == 0 or y == 0 or x == e.width - 1 or y == e.height - 1)
        {
          const auto i = static_cast<std::uint32_t>(e.index(x, y));
          filled[i] = height.values[i];
          downstream[i] = kOutlet;
          visited[i] = 1;
          push(FloodNode{filled[i], i});
        }
      }
    }
    while (!heap.empty())
    {
      std::pop_heap(heap.begin(), heap.end(), FloodGreater{});
      const FloodNode c = heap.back();
      heap.pop_back();
      order.push_back(c.index);
      const level::Cell cc = e.cell(c.index);
      for (int k = 0; k < 8; ++k)
      {
        const int nx = cc.x + kDx[k];
        const int ny = cc.y + kDy[k];
        if (!e.contains(nx, ny))
        {
          continue;
        }
        const auto ni = static_cast<std::uint32_t>(e.index(nx, ny));
        if (visited[ni] != 0)
        {
          continue;
        }
        visited[ni] = 1;
        downstream[ni] = c.index;
        const float h = height.values[ni];
        if (p.mode == DepressionMode::kFillLakes)
        {
          filled[ni] = std::max(h, filled[c.index] + kEpsilon);
        }
        else
        {
          filled[ni] = h;
          if (h <= filled[c.index])
          {
            // n sits in a depression: cut the path to the outlet down below it
            float target = h - kEpsilon;
            for (std::uint32_t k2 = c.index; k2 != kOutlet and filled[k2] >= target; k2 = downstream[k2])
            {
              cut[k2] = std::max(cut[k2], height.values[k2] - target);
              filled[k2] = target;
              target -= kEpsilon;
            }
          }
        }
        push(FloodNode{filled[ni], ni});
      }
    }

    // Route each cell to its steepest-descent neighbor on the conditioned surface (the flood parent
    // where the surface is flat), then accumulate flow from the highest cells down
    for (const std::uint32_t i : order)
    {
      if (downstream[i] == kOutlet)
      {
        continue;
      }
      const level::Cell c = e.cell(i);
      float best = 0.0F;
      for (int k = 0; k < 8; ++k)
      {
        const int nx = c.x + kDx[k];
        const int ny = c.y + kDy[k];
        if (!e.contains(nx, ny))
        {
          continue;
        }
        const auto ni = static_cast<std::uint32_t>(e.index(nx, ny));
        const float slope = (filled[i] - filled[ni]) / ((k < 4) ? 1.0F : 1.41421356F);
        if (slope > best)
        {
          best = slope;
          downstream[i] = ni;
        }
      }
    }
    for (std::size_t r = 0; r < order.size(); ++r)
    {
      rank[order[r]] = static_cast<std::uint32_t>(r);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
      return (filled[a] > filled[b]) or (filled[a] == filled[b] and rank[a] > rank[b]);
    });
    std::fill(flow.begin(), flow.end(), p.rainfall);
    for (const std::uint32_t i : order)
    {
      if (const std::uint32_t d = downstream[i]; d != kOutlet)
      {
        flow[d] += flow[i];
      }
    }

    // Carve into the terrain for the next iteration: breach cuts (except tunnels) and river beds
    for (std::size_t i = 0; i < n; ++i)
    {
      const bool river = flow[i] >= p.river_threshold;
      bool tunnel = false;
      if (p.mode == DepressionMode::kBreach and cut[i] > 0.0F)
      {
        tunnel = river and cut[i] > p.tunnel_threshold;
        if (!tunnel)
        {
          height.values[i] -= cut[i];
        }
      }
      if (river and !tunnel)
      {
        height.values[i] -= p.carve * p.river_depth * std::log2(flow[i] / p.river_threshold + 1.0F);
      }
    }
    if (progress != nullptr)
    {
      progress->advance();
    }
  }

  // Outputs from the final routing
  Grid<float>& water = *out.water;
  for (std::size_t i = 0; i < n; ++i)
  {
    float depth = 0.0F;
    std::uint16_t id = 0;
    const bool river = flow[i] >= p.river_threshold;
    const bool tunnel = (p.mode == DepressionMode::kBreach) and river and cut[i] > p.tunnel_threshold;
    const float river_depth = river ? p.river_depth * std::log2(flow[i] / p.river_threshold + 1.0F) : 0.0F;
    const float fill_depth = (p.mode == DepressionMode::kFillLakes) ? filled[i] - height.values[i] : 0.0F;
    // Shallow fills along carved river beds are still river; deeper than the river, it's a lake
    if (fill_depth > p.lake_min_depth and fill_depth > river_depth)
    {
      depth = fill_depth;
      id = out.lake_id;
      ++result.lake_cells;
    }
    else if (tunnel)
    {
      id = out.tunnel_id;
      ++result.tunnel_cells;
    }
    else if (river)
    {
      depth = std::max(river_depth, fill_depth);
      id = out.river_id;
      ++result.river_cells;
    }
    water.values[i] = depth;
    if (out.semantic != nullptr and id != 0)
    {
      out.semantic->values[i] = id;
    }
    if (out.flow != nullptr)
    {
      out.flow->values[i] = flow[i];
    }
  }
  if (progress != nullptr)
  {
    progress->advance();
  }
  return result;
}

bool load(json::Reader& r, const Json& j, ErosionParams& v)
{
  r.object(j, "erosion");
  r.get_optional(j, "droplets", v.droplets);
  r.get_optional(j, "max_lifetime", v.max_lifetime);
  r.get_optional(j, "inertia", v.inertia);
  r.get_optional(j, "capacity", v.capacity);
  r.get_optional(j, "min_capacity", v.min_capacity);
  r.get_optional(j, "deposition", v.deposition);
  r.get_optional(j, "erosion", v.erosion);
  r.get_optional(j, "evaporation", v.evaporation);
  r.get_optional(j, "gravity", v.gravity);
  r.get_optional(j, "radius", v.radius);
  return r.ok();
}

void save(Json& j, const ErosionParams& v)
{
  j = Json::object();
  json::write(j, "droplets", v.droplets);
  json::write(j, "max_lifetime", v.max_lifetime);
  json::write(j, "inertia", v.inertia);
  json::write(j, "capacity", v.capacity);
  json::write(j, "min_capacity", v.min_capacity);
  json::write(j, "deposition", v.deposition);
  json::write(j, "erosion", v.erosion);
  json::write(j, "evaporation", v.evaporation);
  json::write(j, "gravity", v.gravity);
  json::write(j, "radius", v.radius);
}

bool load(json::Reader& r, const Json& j, WatershedParams& v)
{
  r.object(j, "watershed");
  r.get_optional(j, "iterations", v.iterations);
  r.get_optional(j, "erode", v.erode);
  r.get_optional(j, "erosion", v.erosion);
  r.get_optional(j, "rainfall", v.rainfall);
  r.get_optional(j, "river_threshold", v.river_threshold);
  r.get_optional(j, "river_depth", v.river_depth);
  r.get_optional(j, "carve", v.carve);
  r.get_optional(j, "mode", v.mode);
  r.get_optional(j, "lake_min_depth", v.lake_min_depth);
  r.get_optional(j, "tunnel_threshold", v.tunnel_threshold);
  return r.ok();
}

void save(Json& j, const WatershedParams& v)
{
  j = Json::object();
  json::write(j, "iterations", v.iterations);
  json::write(j, "erode", v.erode);
  json::write(j, "erosion", v.erosion);
  json::write(j, "rainfall", v.rainfall);
  json::write(j, "river_threshold", v.river_threshold);
  json::write(j, "river_depth", v.river_depth);
  json::write(j, "carve", v.carve);
  json::write(j, "mode", v.mode);
  json::write(j, "lake_min_depth", v.lake_min_depth);
  json::write(j, "tunnel_threshold", v.tunnel_threshold);
}

}  // namespace lvl::procgen
