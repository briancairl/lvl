/**
 * @copyright 2026-present Brian Cairl
 *
 * @file shape.hpp
 *
 * Deliberate shaping of a heightmap: a mound stamped over a rectangle, and a smoothing pass over
 * one.
 *
 * Noise makes terrain that looks like terrain, which is not the same thing as terrain the tiles can
 * draw. A hill that is going to be cut into height levels and tiled wants a *shape* -- a square
 * pyramid terraces into nested square rings, which is exactly what lvl::procgen::tidy_levels would
 * otherwise have to square off, and what lvl::level::step_type_of has tiles for. So these are the
 * two things a heightmap needs by hand once the noise has been generated: a mound put where the
 * level wants a hill, and the roughness taken back out of what the noise left.
 *
 * Both are free functions over one plane, instant even on a large map, and neither reads anything
 * but the heightmap itself.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/json.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/procgen/wfc.hpp"

namespace lvl::procgen
{

/**
 * @brief How a mound's distance from its center is measured, which is what gives it its shape
 *
 * <code>kPyramid</code> is the one worth reaching for when the map is going to be stepped: the
 * Chebyshev distance is constant along a square ring, so every terrace cut out of the mound is a
 * square and every corner of it is one the step tiles have an answer for. The round shapes are for
 * a hill that is going to be looked at rather than tiled, or for one that noise will roughen up
 * again afterwards.
 */
enum class ShapeKind : std::uint8_t
{
  /// Chebyshev distance: a square base and square terraces
  kPyramid,
  /// Euclidean distance: a round base, straight slope
  kCone,
  /// Manhattan distance: a diamond base
  kDiamond,
};

/// How a stamped mound combines with the heights already on the layer
enum class ShapeBlend : std::uint8_t
{
  /// The mound is the height
  kReplace,
  kAdd,
  /// Keeps whichever is higher, so a mound never digs into higher ground
  kMax,
  kMin,
  /// Scales what is there by the mound, which is how a map is faded to its rim
  kMultiply,
};

/**
 * @brief A mound stamped over a rectangle
 *
 * The rectangle is the mound's base: its center is the peak and its rim is <code>base</code>,
 * whatever its aspect, so the distance is normalized per axis and an oblong rectangle gives an
 * oblong mound.
 */
struct PyramidParams
{
  ShapeKind kind = ShapeKind::kPyramid;
  ShapeBlend blend = ShapeBlend::kMax;
  /// Height at the center
  float peak = 1.0F;
  /// Height at the rim of the rectangle
  float base = 0.0F;
  /// Fraction of the way out that is a flat top (0 = a point, 1 = a plateau with no slope at all)
  float plateau = 0.0F;
  /// Rounds the slope off into a dome rather than leaving it straight
  bool rounded = false;
  /**
   * @brief Cuts the slope into this many flat rings (0 = a smooth slope)
   *
   * The heights are quantized to <code>steps + 1</code> values, the innermost exactly
   * <code>peak</code> and the outermost exactly <code>base</code>, so a stepped pyramid is already
   * the terraces it will be cut into -- which is how a hill comes out of the tiling with no cell
   * the step tiles cannot express.
   */
  int steps = 0;
};

struct ShapeResult
{
  /// Cells inside the rectangle
  std::size_t cells = 0;
  /// Cells whose height the stamp changed
  std::size_t changed = 0;
};

/**
 * @brief Stamps a mound over <code>region</code> of <code>out</code> (the whole grid when empty)
 *
 * Cells outside the rectangle are left exactly as they are, so a mound can be put on a map without
 * disturbing the rest of it. Nothing is clamped: the heights a stamp writes are the ones the
 * parameters ask for, as a layer's range is the user's to set.
 */
ShapeResult apply_pyramid(const PyramidParams& params, Region region, level::Grid<float>& out);

/**
 * @brief The mean of the cells within <code>radius</code> of (x, y) that are on the grid
 *
 * Off-grid cells are left out of the mean rather than counted as 0, so smoothing never pulls the
 * map's edge down towards nothing. This is one cell of a box blur, and it is what both the
 * smoothing pass and the editor's smooth brush are built out of, so the two agree.
 */
float blurred_at(const level::Grid<float>& grid, level::Cell c, int radius);

struct SmoothParams
{
  /// Cells from the center of the kernel (1 = the eight neighbors)
  int radius = 1;
  /// How much of the blurred value each cell takes (0 = nothing, 1 = fully blurred)
  float amount = 1.0F;
  /// Passes over the region, each over what the last one left
  int iterations = 1;
};

struct SmoothResult
{
  std::size_t cells = 0;
  /// Cells whose height the pass changed
  std::size_t changed = 0;
  /// The largest height change any cell saw
  float max_delta = 0.0F;
};

/**
 * @brief Box-blurs <code>region</code> of <code>grid</code> (the whole grid when empty)
 *
 * Cells outside the rectangle keep their heights but are still read, so smoothing a region does not
 * leave a seam at its edge. <code>scratch</code> holds one float per cell and is rewound on return.
 */
SmoothResult smooth_scalar(const SmoothParams& params, Region region, level::Grid<float>& grid, memory::Arena& scratch);

}  // namespace lvl::procgen

template <> struct lvl::EnumNames<lvl::procgen::ShapeKind>
{
  static constexpr std::array<std::string_view, 3> kNames = {"pyramid", "cone", "diamond"};
};

template <> struct lvl::EnumNames<lvl::procgen::ShapeBlend>
{
  static constexpr std::array<std::string_view, 5> kNames = {"replace", "add", "max", "min", "multiply"};
};
