/**
 * @copyright 2026-present Brian Cairl
 *
 * @file preview.hpp
 *
 * Per-cell colors for layer previews (one pixel per cell). The editor uploads these as overlay
 * textures; exports blend them over the tile render. Same code path, so what you see is what you
 * export.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <string_view>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/level/level.hpp"
#include "lvl/project/project.hpp"

namespace lvl::render
{

/// Decoded atlas images (and their average tile colors), one per tileset, indexed by TileSetId
struct Atlases
{
  const image::Image* images = nullptr;
  const memory::Vector<image::Rgba>* tile_colors = nullptr;
  std::size_t count = 0;
};

/// Terrain ramp: deep water .. sand .. grass .. rock .. snow, for t in [0, 1]
image::Rgba terrain_ramp(float t);

/// Heat ramp: blue .. cyan .. yellow .. red, for t in [0, 1]
image::Rgba heat_ramp(float t);

/// Stable distinct color for an id with no catalog color
image::Rgba id_color(std::uint32_t id);

/**
 * @brief Colors every cell of <code>layer</code> into <code>out</code> (extents-sized, one pixel per cell)
 *
 * Tile layers use each tile's average color from <code>atlases</code> (see average_tile_colors), or
 * id colors where there is none.
 * Transparent pixels mean "nothing here".
 */
void colorize(
  const project::Project& project,
  const level::Layer& layer,
  image::Image& out,
  const Atlases* atlases = nullptr,
  std::string_view season = {});

/**
 * @brief Average color of every tile of a tileset, from its atlas
 *
 * The color is the alpha-weighted mean of the tile's pixels, and the alpha is their mean coverage, so
 * a tile that covers part of its cell is drawn part-transparent where it stands for a whole cell and
 * lets the layers under it through. Tiles with no opaque pixels are transparent.
 */
void average_tile_colors(const level::TileSet& tileset, const image::Image& atlas, memory::Vector<image::Rgba>& out);

}  // namespace lvl::render
