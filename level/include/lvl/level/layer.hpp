/**
 * @copyright 2026-present Brian Cairl
 *
 * @file layer.hpp
 *
 * A level is a stack of layers, each one data plane over the level's grid. The plane's element
 * type is determined by the layer's kind:
 *
 * | kind       | element       | meaning                                         |
 * |------------|---------------|-------------------------------------------------|
 * | kTiles     | TileRef       | tile assignment (drawn from a tileset)          |
 * | kSemantic  | SemanticId    | semantic tile type ("road", "forest", ...)      |
 * | kHeight    | float         | heightmap (any range; previewed normalized)     |
 * | kCollision | uint8 mask    | grid colliders (see lvl::level::collision)      |
 * | kMaterial  | MaterialId    | surface material                                |
 * | kWater     | float         | water depth (rivers/lakes; watershed output)    |
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>
#include <variant>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"

namespace lvl::level
{

enum class LayerKind : std::uint8_t
{
  kTiles,
  kSemantic,
  kHeight,
  kCollision,
  kMaterial,
  kWater,
};

inline constexpr std::size_t kLayerKindCount = 6;

/// How a layer is drawn in the editor's viewport
enum class Preview : std::uint8_t
{
  /// Kind's natural look: tiles as tiles, ids in catalog colors, scalars in grayscale, masks as glyphs
  kNatural,
  /// Scalars mapped through a terrain color ramp (deep water .. snow)
  kTerrain,
  /// Scalars as a blue-to-red heat ramp
  kHeat,
  /// Scalars as iso-lines
  kContours,
  /// Scalars shaded by a light from the north-west
  kHillshade,
};

inline constexpr std::size_t kPreviewCount = 5;

using TileGrid = Grid<TileRef>;
using IdGrid = Grid<std::uint16_t>;
using ScalarGrid = Grid<float>;
using MaskGrid = Grid<std::uint8_t>;

/// A layer's data plane (alternative chosen by LayerKind; see <code>plane_index</code>)
using LayerData = std::variant<TileGrid, IdGrid, ScalarGrid, MaskGrid>;

/// Index of the LayerData alternative used by <code>kind</code>
constexpr std::size_t plane_index(LayerKind kind)
{
  switch (kind)
  {
  case LayerKind::kTiles:
    return 0;
  case LayerKind::kSemantic:
  case LayerKind::kMaterial:
    return 1;
  case LayerKind::kHeight:
  case LayerKind::kWater:
    return 2;
  case LayerKind::kCollision:
    return 3;
  }
  return 0;
}

/// True for kinds whose cells are floats
constexpr bool is_scalar(LayerKind kind) { return plane_index(kind) == 2; }

struct Layer
{
  Name name;
  LayerKind kind = LayerKind::kTiles;
  /// Shown in the viewport (and included in exports)
  bool visible = true;
  /// Protected from editing
  bool locked = false;
  /// Viewport/export opacity [0, 1]
  float opacity = 1.0F;
  Preview preview = Preview::kNatural;
  /// Scalar layers: values mapped to the bottom/top of the preview ramp
  float range_min = 0.0F;
  float range_max = 1.0F;
  /// Scalar layers: iso-line spacing for Preview::kContours
  float contour_interval = 0.1F;
  LayerData data;
};

/// Creates a layer of <code>kind</code> sized <code>extents</code>, every cell at the kind's empty value
Layer make_layer(memory::Arena& arena, LayerKind kind, std::string_view name, Extents extents);

/// Resizes a layer's plane (keeping overlapping cells)
void resize(Layer& layer, Extents extents);

/// Grid dimensions of a layer
Extents extents(const Layer& layer);

/// Recomputes <code>range_min</code>/<code>range_max</code> from a scalar layer's data
void fit_range(Layer& layer);

/// Typed access to a layer's plane (asserts the kind matches)
template <typename GridT> GridT& plane(Layer& layer)
{
  auto* g = std::get_if<GridT>(&layer.data);
  LVL_ASSERT_NON_NULL(g) << "layer '" << layer.name << "' does not hold the requested plane type";
  return *g;
}

template <typename GridT> const GridT& plane(const Layer& layer)
{
  const auto* g = std::get_if<GridT>(&layer.data);
  LVL_ASSERT_NON_NULL(g) << "layer '" << layer.name << "' does not hold the requested plane type";
  return *g;
}

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::LayerKind>
{
  static constexpr std::array<std::string_view, 6> kNames =
    {"tiles", "semantic", "height", "collision", "material", "water"};
};

template <> struct lvl::EnumNames<lvl::level::Preview>
{
  static constexpr std::array<std::string_view, 5> kNames = {"natural", "terrain", "heat", "contours", "hillshade"};
};
