/**
 * @copyright 2026-present Brian Cairl
 *
 * @file compose.hpp
 *
 * CPU rendering of a whole level to an image: for PNG export of the full map, and for the small
 * mini-preview. Rows are rendered in parallel.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <string_view>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"
#include "lvl/level/level.hpp"
#include "lvl/render/preview.hpp"

namespace lvl::render
{

struct ComposeOptions
{
  /// Output pixels per cell (0 = the level's cell width)
  int pixels_per_cell = 0;
  /// Season whose tile variants are drawn (empty = base tiles)
  std::string_view season = {};
  /// Blend visible non-tile layers over the tiles with their preview colors and opacity
  bool overlays = true;
  /// Multiply by the season's tint
  bool tint = false;
  /// Draw portals and path graphs
  bool objects = false;
  /// Color under empty cells
  image::Rgba background = {0, 0, 0, 0};
};

/// Bytes an image of <code>width</code> x <code>height</code> needs
constexpr std::size_t image_bytes(std::size_t width, std::size_t height)
{
  return width * height * sizeof(image::Rgba);
}

/**
 * @brief Renders the visible layers of <code>level</code> into <code>out</code> (allocated from <code>dest</code>)
 *
 * Fails (rather than asserts) if the image would not fit in <code>dest</code>.
 */
Status compose(
  const level::Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  memory::Arena& dest,
  image::Image& out,
  std::size_t workers = 1,
  jobs::Progress* progress = nullptr);

/**
 * @brief Renders a small preview of the level, at most <code>max_size</code> pixels on its longer side
 *
 * Each cell is drawn as its tiles' average colors with overlays, then the result is resampled.
 */
Status minimap(
  const level::Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  int max_size,
  memory::Arena& dest,
  memory::Arena& scratch,
  image::Image& out);

/// Renders and saves the full map as PNG
Status export_png(
  const char* path,
  const level::Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  memory::Arena& scratch,
  std::size_t workers = 1,
  jobs::Progress* progress = nullptr);

/// Renders and saves the mini-preview as PNG
Status export_minimap_png(
  const char* path,
  const level::Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  int max_size,
  memory::Arena& scratch);

}  // namespace lvl::render
