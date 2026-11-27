/**
 * @copyright 2026-present Brian Cairl
 *
 * @file shape.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>

// LVL
#include "lvl/procgen/shape.hpp"

namespace lvl::procgen
{
namespace
{

using level::Cell;
using level::Extents;

/// Relative height change below which a smoothing pass counts a cell as unmoved
constexpr float kEpsilon = 1e-6F;

/// The rectangle a region names, clipped to the grid (an empty region is the whole grid)
struct Bounds
{
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  [[nodiscard]] bool empty() const { return x1 <= x0 or y1 <= y0; }
  [[nodiscard]] std::size_t count() const
  {
    return static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
  }
};

Bounds clip(Region region, Extents e)
{
  const bool whole = (region.width <= 0 or region.height <= 0);
  return Bounds{
    .x0 = whole ? 0 : std::max(region.x, 0),
    .y0 = whole ? 0 : std::max(region.y, 0),
    .x1 = whole ? e.width : std::min(region.x + region.width, e.width),
    .y1 = whole ? e.height : std::min(region.y + region.height, e.height)};
}

/// The region as the mound's base: unclipped, so a mound half off the map is half a mound
Region base_of(Region region, Extents e)
{
  if (region.width <= 0 or region.height <= 0)
  {
    return Region{.x = 0, .y = 0, .width = e.width, .height = e.height};
  }
  return region;
}

/// The mean of the cells of <code>values</code> within <code>r</code> of <code>c</code> that are on the grid
float mean_of(const float* values, Extents e, Cell c, int r)
{
  const int x0 = std::max(c.x - r, 0);
  const int y0 = std::max(c.y - r, 0);
  const int x1 = std::min(c.x + r + 1, e.width);
  const int y1 = std::min(c.y + r + 1, e.height);
  if (x1 <= x0 or y1 <= y0)
  {
    return e.contains(c) ? values[e.index(c)] : 0.0F;
  }
  float sum = 0.0F;
  for (int y = y0; y < y1; ++y)
  {
    for (int x = x0; x < x1; ++x)
    {
      sum += values[e.index(x, y)];
    }
  }
  return sum / static_cast<float>(static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0));
}

float blend_value(ShapeBlend blend, float was, float wanted)
{
  switch (blend)
  {
  case ShapeBlend::kReplace:
    return wanted;
  case ShapeBlend::kAdd:
    return was + wanted;
  case ShapeBlend::kMax:
    return std::max(was, wanted);
  case ShapeBlend::kMin:
    return std::min(was, wanted);
  case ShapeBlend::kMultiply:
    return was * wanted;
  }
  return wanted;
}

}  // namespace

ShapeResult apply_pyramid(const PyramidParams& params, Region region, level::Grid<float>& out)
{
  ShapeResult result;
  const Extents e = out.extents;
  const Bounds b = clip(region, e);
  if (b.empty())
  {
    return result;
  }
  result.cells = b.count();

  // The mound's base is the region as asked for rather than as clipped, so one that runs off the
  // map is the part of a mound that fits rather than a whole one squeezed into what is left
  const Region base = base_of(region, e);
  const float cx = static_cast<float>(base.x) + 0.5F * static_cast<float>(base.width - 1);
  const float cy = static_cast<float>(base.y) + 0.5F * static_cast<float>(base.height - 1);
  // Half the base per axis, so an oblong rectangle gives an oblong mound; never 0, since a base one
  // cell wide is all peak rather than a division by nothing
  const float hx = std::max(0.5F * static_cast<float>(base.width - 1), 1.0F);
  const float hy = std::max(0.5F * static_cast<float>(base.height - 1), 1.0F);
  const float plateau = std::clamp(params.plateau, 0.0F, 1.0F);
  const int steps = std::max(params.steps, 0);

  for (int y = b.y0; y < b.y1; ++y)
  {
    for (int x = b.x0; x < b.x1; ++x)
    {
      const float dx = std::abs((static_cast<float>(x) - cx) / hx);
      const float dy = std::abs((static_cast<float>(y) - cy) / hy);
      float t = 0.0F;
      switch (params.kind)
      {
      case ShapeKind::kPyramid:
        t = std::max(dx, dy);
        break;
      case ShapeKind::kCone:
        t = std::sqrt(dx * dx + dy * dy);
        break;
      case ShapeKind::kDiamond:
        t = dx + dy;
        break;
      }
      // Past the flat top, the rest of the way out is the slope
      t = (plateau < 1.0F) ? (t - plateau) / (1.0F - plateau) : 0.0F;
      t = std::clamp(t, 0.0F, 1.0F);
      if (params.rounded)
      {
        t = t * t * (3.0F - 2.0F * t);
      }
      if (steps > 0)
      {
        // Quantized to steps + 1 heights, the innermost exactly the peak and the outermost exactly
        // the base, so the rings are flat and the mound is already its own terraces
        t = std::round(t * static_cast<float>(steps)) / static_cast<float>(steps);
      }
      const float wanted = params.peak + (params.base - params.peak) * t;
      float& v = out.at(x, y);
      const float next = blend_value(params.blend, v, wanted);
      if (next != v)
      {
        v = next;
        ++result.changed;
      }
    }
  }
  return result;
}

float blurred_at(const level::Grid<float>& grid, Cell c, int radius)
{
  return mean_of(grid.values.data(), grid.extents, c, radius);
}

SmoothResult smooth_scalar(const SmoothParams& params, Region region, level::Grid<float>& grid, memory::Arena& scratch)
{
  SmoothResult result;
  const Extents e = grid.extents;
  const Bounds b = clip(region, e);
  if (b.empty() or e.count() == 0)
  {
    return result;
  }
  result.cells = b.count();
  const float amount = std::clamp(params.amount, 0.0F, 1.0F);
  const int radius = std::max(params.radius, 1);
  const int iterations = std::max(params.iterations, 1);
  if (amount <= 0.0F)
  {
    return result;
  }

  memory::Scope scope{scratch};
  // Each pass reads the heights the last one left, so no cell is blurred against neighbors the same
  // pass has already moved. The copy is of the whole plane, since cells outside the region keep
  // their heights but are still read -- which is what leaves no seam at the region's edge
  auto source = memory::make_vector<float>(scratch, e.count(), 0.0F);
  auto before = memory::make_vector<float>(scratch, result.cells, 0.0F);
  const int span = b.x1 - b.x0;
  for (std::size_t i = 0; i < result.cells; ++i)
  {
    const int x = b.x0 + static_cast<int>(i % static_cast<std::size_t>(span));
    const int y = b.y0 + static_cast<int>(i / static_cast<std::size_t>(span));
    before[i] = grid.values[e.index(x, y)];
  }

  for (int pass = 0; pass < iterations; ++pass)
  {
    std::copy(grid.values.begin(), grid.values.end(), source.begin());
    for (int y = b.y0; y < b.y1; ++y)
    {
      for (int x = b.x0; x < b.x1; ++x)
      {
        const float v = source[e.index(x, y)];
        const float next = v + (mean_of(source.data(), e, Cell{x, y}, radius) - v) * amount;
        // The mean of equal values is not exactly equal to them in floating point, so a move this
        // small is not one: writing it would leave flat ground drifting, and every pass over an
        // already-smooth map reporting that it changed every cell
        if (std::abs(next - v) > kEpsilon * std::max(1.0F, std::abs(v)))
        {
          grid.values[e.index(x, y)] = next;
        }
      }
    }
  }

  for (std::size_t i = 0; i < result.cells; ++i)
  {
    const int x = b.x0 + static_cast<int>(i % static_cast<std::size_t>(span));
    const int y = b.y0 + static_cast<int>(i / static_cast<std::size_t>(span));
    const float delta = grid.values[e.index(x, y)] - before[i];
    if (delta != 0.0F)
    {
      ++result.changed;
      result.max_delta = std::max(result.max_delta, std::abs(delta));
    }
  }
  return result;
}

}  // namespace lvl::procgen
