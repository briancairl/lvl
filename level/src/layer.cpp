/**
 * @copyright 2026-present Brian Cairl
 *
 * @file layer.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <limits>

// LVL
#include "lvl/core/visitor.hpp"
#include "lvl/level/layer.hpp"

namespace lvl::level
{

Layer make_layer(memory::Arena& arena, LayerKind kind, std::string_view name, Extents e)
{
  Layer layer;
  layer.name.assign(name);
  layer.kind = kind;
  switch (plane_index(kind))
  {
  case 0: {
    TileGrid g{arena};
    g.assign(e, kEmptyTile);
    layer.data = std::move(g);
    break;
  }
  case 1: {
    IdGrid g{arena};
    g.assign(e, 0);
    layer.data = std::move(g);
    break;
  }
  case 2: {
    ScalarGrid g{arena};
    g.assign(e, 0.0F);
    layer.data = std::move(g);
    layer.preview = (kind == LayerKind::kHeight) ? Preview::kTerrain : Preview::kNatural;
    break;
  }
  default: {
    MaskGrid g{arena};
    g.assign(e, 0);
    layer.data = std::move(g);
    break;
  }
  }
  if (kind == LayerKind::kWater)
  {
    layer.opacity = 0.75F;
  }
  return layer;
}

void resize(Layer& layer, Extents e)
{
  std::visit(
    overloaded{
      [e](TileGrid& g) { g.resize(e, kEmptyTile); },
      [e](IdGrid& g) { g.resize(e, 0); },
      [e](ScalarGrid& g) { g.resize(e, 0.0F); },
      [e](MaskGrid& g) { g.resize(e, 0); },
    },
    layer.data);
}

Extents extents(const Layer& layer)
{
  return std::visit([](const auto& g) { return g.extents; }, layer.data);
}

void fit_range(Layer& layer)
{
  if (!is_scalar(layer.kind))
  {
    return;
  }
  const auto& g = plane<ScalarGrid>(layer);
  if (g.values.empty())
  {
    return;
  }
  const auto [lo, hi] = std::minmax_element(g.values.begin(), g.values.end());
  layer.range_min = *lo;
  layer.range_max = (*hi > *lo) ? *hi : *lo + 1.0F;
}

}  // namespace lvl::level
