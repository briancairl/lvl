/**
 * @copyright 2026-present Brian Cairl
 *
 * @file compose.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <bit>
#include <cmath>

// LVL
#include "lvl/render/compose.hpp"
#include "lvl/render/preview.hpp"

namespace lvl::render
{
namespace
{

using image::Rgba;
using namespace level;

Rgba multiply(Rgba c, Rgba tint)
{
  return Rgba{
    static_cast<std::uint8_t>(c.r * tint.r / 255),
    static_cast<std::uint8_t>(c.g * tint.g / 255),
    static_cast<std::uint8_t>(c.b * tint.b / 255),
    c.a};
}

const Season* find(const project::Project& project, std::string_view name)
{
  const SeasonId id = project::find_season(project, name);
  return (id < project.seasons.size()) ? &project.seasons[id] : nullptr;
}

/// Draws a line (Bresenham) into an image
void line(image::Image& img, int x0, int y0, int x1, int y1, Rgba color)
{
  const int dx = std::abs(x1 - x0);
  const int dy = -std::abs(y1 - y0);
  const int sx = (x0 < x1) ? 1 : -1;
  const int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  while (true)
  {
    if (x0 >= 0 and y0 >= 0 and x0 < img.width and y0 < img.height)
    {
      img.at(x0, y0) = image::blend(img.at(x0, y0), color);
    }
    if (x0 == x1 and y0 == y1)
    {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy)
    {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx)
    {
      err += dx;
      y0 += sy;
    }
  }
}

void draw_objects(const Level& level, image::Image& img, float scale)
{
  const Registry& reg = level.registry;
  for (auto [e, edge] : reg.view<PathEdge>().each())
  {
    const auto& a = reg.get<Position>(edge.from);
    const auto& b = reg.get<Position>(edge.to);
    const GraphId g = reg.get<PathNode>(edge.from).graph;
    if (g < level.graphs.size() and level.graphs[g].visible)
    {
      line(
        img,
        static_cast<int>(a.x * scale),
        static_cast<int>(a.y * scale),
        static_cast<int>(b.x * scale),
        static_cast<int>(b.y * scale),
        level.graphs[g].color);
    }
  }
  for (auto [e, pos, ext, portal] : reg.view<Position, Extent, Portal>().each())
  {
    const int x0 = static_cast<int>(pos.x * scale);
    const int y0 = static_cast<int>(pos.y * scale);
    const int x1 = static_cast<int>((pos.x + ext.width) * scale) - 1;
    const int y1 = static_cast<int>((pos.y + ext.height) * scale) - 1;
    const Rgba c{80, 220, 255, 255};
    line(img, x0, y0, x1, y0, c);
    line(img, x1, y0, x1, y1, c);
    line(img, x1, y1, x0, y1, c);
    line(img, x0, y1, x0, y0, c);
  }
}

/// Per-layer previews (one pixel per cell) for the overlay layers, from <code>arena</code>
struct Overlays
{
  const Layer* layers[256];
  image::Image images[256];
  std::size_t count = 0;
};

void build_overlays(
  const project::Project& project,
  const Level& level,
  const Atlases& atlases,
  std::string_view season,
  memory::Arena& arena,
  Overlays& out)
{
  for (const auto& layer : level.layers)
  {
    if (!layer.visible or layer.kind == LayerKind::kTiles or out.count == 256)
    {
      continue;
    }
    image::Image img = image::create(arena, level.settings.extents.width, level.settings.extents.height);
    colorize(project, layer, img, &atlases, season);
    out.layers[out.count] = &layer;
    out.images[out.count] = img;
    ++out.count;
  }
}

}  // namespace

Status compose(
  const project::Project& project,
  const Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  memory::Arena& dest,
  image::Image& out,
  std::size_t workers,
  jobs::Progress* progress)
{
  const Extents e = level.settings.extents;
  const int ppc = (options.pixels_per_cell > 0) ? options.pixels_per_cell : level.settings.cell_width;
  const std::size_t width = static_cast<std::size_t>(e.width) * static_cast<std::size_t>(ppc);
  const std::size_t height = static_cast<std::size_t>(e.height) * static_cast<std::size_t>(ppc);
  const std::size_t overlay_bytes = options.overlays ? level.layers.size() * image_bytes(e.width, e.height) : 0;
  if (width > 32768 or height > 32768 or image_bytes(width, height) + overlay_bytes + 4096 > memory::available(dest))
  {
    return Status::error(
      "a %zux%zu image needs %zu MiB; %zu MiB available (lower pixels per cell)",
      width,
      height,
      (image_bytes(width, height) + overlay_bytes) >> 20,
      memory::available(dest) >> 20);
  }
  out = image::create(dest, static_cast<int>(width), static_cast<int>(height), options.background);
  Overlays overlays;
  if (options.overlays)
  {
    build_overlays(project, level, atlases, options.season, dest, overlays);
  }
  const Season* season = options.tint ? find(project, options.season) : nullptr;

  if (progress != nullptr)
  {
    progress->reset(static_cast<std::size_t>(e.height) + 1);
    progress->stage("Rendering map");
  }
  // Each worker renders whole cell rows (ppc pixel rows each); layers are drawn bottom to top
  jobs::parallel_for(static_cast<std::size_t>(e.height), workers, [&](std::size_t row, std::size_t) {
    const int cy = static_cast<int>(row);
    std::size_t overlay = 0;
    for (const auto& layer : level.layers)
    {
      if (!layer.visible)
      {
        continue;
      }
      if (layer.kind != LayerKind::kTiles)
      {
        if (!options.overlays or overlay >= overlays.count)
        {
          continue;
        }
        const image::Image& img = overlays.images[overlay++];
        for (int cx = 0; cx < e.width; ++cx)
        {
          const Rgba c = img.at(cx, cy);
          if (c.a == 0)
          {
            continue;
          }
          for (int py = 0; py < ppc; ++py)
          {
            for (int px = 0; px < ppc; ++px)
            {
              Rgba& dst = out.at(cx * ppc + px, cy * ppc + py);
              dst = image::blend(dst, c, layer.opacity);
            }
          }
        }
        continue;
      }
      const auto& g = plane<TileGrid>(layer);
      for (int cx = 0; cx < e.width; ++cx)
      {
        const TileRef t = g.at(cx, cy);
        if (t.empty() or t.tileset >= project.tilesets.size())
        {
          continue;
        }
        const TileSet& ts = project.tilesets[t.tileset];
        const TileIndex drawn = seasonal_tile(ts, t.tile, options.season);
        const image::Image* atlas = (t.tileset < atlases.count) ? &atlases.images[t.tileset] : nullptr;
        if (atlas == nullptr or atlas->empty())
        {
          const Rgba c = id_color(static_cast<std::uint32_t>(t.tileset) * 65536U + drawn);
          for (int py = 0; py < ppc; ++py)
          {
            for (int px = 0; px < ppc; ++px)
            {
              Rgba& dst = out.at(cx * ppc + px, cy * ppc + py);
              dst = image::blend(dst, c, layer.opacity);
            }
          }
          continue;
        }
        const TileRect r = tile_rect(ts.slice, atlas->width, atlas->height, drawn);
        for (int py = 0; py < ppc; ++py)
        {
          const int sy = std::min(r.y + py * r.height / ppc, atlas->height - 1);
          for (int px = 0; px < ppc; ++px)
          {
            const int sx = std::min(r.x + px * r.width / ppc, atlas->width - 1);
            Rgba& dst = out.at(cx * ppc + px, cy * ppc + py);
            dst = image::blend(dst, atlas->at(sx, sy), layer.opacity);
          }
        }
      }
    }
    if (season != nullptr)
    {
      for (int py = 0; py < ppc; ++py)
      {
        for (int x = 0; x < out.width; ++x)
        {
          Rgba& dst = out.at(x, cy * ppc + py);
          dst = multiply(dst, season->tint);
        }
      }
    }
    if (progress != nullptr)
    {
      progress->advance();
    }
  });
  if (options.objects)
  {
    draw_objects(level, out, static_cast<float>(ppc));
  }
  if (progress != nullptr)
  {
    progress->advance();
  }
  return Status::okay();
}

Status minimap(
  const project::Project& project,
  const Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  int max_size,
  memory::Arena& dest,
  memory::Arena& scratch,
  image::Image& out)
{
  const Extents e = level.settings.extents;
  memory::Scope scope{scratch};
  if (image_bytes(e.width, e.height) * (level.layers.size() + 2) > memory::available(scratch))
  {
    return Status::error("not enough scratch memory for a %dx%d mini-preview", e.width, e.height);
  }
  // One pixel per cell: tile average colors and overlays
  image::Image cells = image::create(scratch, e.width, e.height, options.background);
  image::Image layer_img = image::create(scratch, e.width, e.height);
  for (const auto& layer : level.layers)
  {
    if (!layer.visible or (!options.overlays and layer.kind != LayerKind::kTiles))
    {
      continue;
    }
    colorize(project, layer, layer_img, &atlases, options.season);
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
      cells.pixels[i] = image::blend(cells.pixels[i], layer_img.pixels[i], layer.opacity);
    }
  }
  if (options.tint)
  {
    if (const Season* season = find(project, options.season); season != nullptr)
    {
      for (std::size_t i = 0; i < cells.size(); ++i)
      {
        cells.pixels[i] = multiply(cells.pixels[i], season->tint);
      }
    }
  }
  if (options.objects)
  {
    draw_objects(level, cells, 1.0F);
  }
  // Fit the longer side to max_size (never upscaled past one pixel per cell * 4)
  const int longer = std::max(e.width, e.height);
  const float scale = std::min(static_cast<float>(std::max(max_size, 1)) / static_cast<float>(longer), 4.0F);
  const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(e.width) * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(e.height) * scale)));
  if (image_bytes(w, h) > memory::available(dest))
  {
    return Status::error("not enough memory for a %dx%d mini-preview", w, h);
  }
  out = image::resize(cells, w, h, dest, scratch);
  return Status::okay();
}

Status export_png(
  const char* path,
  const project::Project& project,
  const Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  memory::Arena& scratch,
  std::size_t workers,
  jobs::Progress* progress)
{
  memory::Scope scope{scratch};
  // The image comes from the bottom of scratch; PNG encoding works above it (in a sub-arena)
  image::Image img;
  LVL_RETURN_IF_ERROR(compose(project, level, atlases, options, scratch, img, workers, progress));
  if (progress != nullptr)
  {
    progress->stage("Encoding PNG");
  }
  memory::Arena encode;
  memory::push(scratch, encode, "png-encode");
  const Status status = image::save_png(path, img, encode);
  memory::pop(scratch, encode);
  return status;
}

Status export_layer_png(
  const char* path,
  const project::Project& project,
  const level::Layer& layer,
  const Atlases* atlases,
  std::string_view season,
  memory::Arena& scratch)
{
  const level::Extents size = level::extents(layer);
  if (size.width <= 0 or size.height <= 0)
  {
    return Status::error("the layer has no cells to write");
  }
  // One for the pixels, one for the PNG encoder, as the other exports do
  memory::Arena result;
  memory::Arena work;
  memory::push(scratch, result, memory::available(scratch) / 4, "layer-result");
  memory::push(scratch, work, "layer-work");
  image::Image img = image::create(result, size.width, size.height);
  Status status =
    img.empty() ? Status::error("not enough memory for a %dx%d image", size.width, size.height) : Status::okay();
  if (status.ok)
  {
    colorize(project, layer, img, atlases, season);
    status = image::save_png(path, img, work);
  }
  memory::pop(scratch, work);
  memory::pop(scratch, result);
  return status;
}

Status export_minimap_png(
  const char* path,
  const project::Project& project,
  const Level& level,
  const Atlases& atlases,
  const ComposeOptions& options,
  int max_size,
  memory::Arena& scratch)
{
  // Two sub-arenas: one for the result image, one for working memory
  memory::Arena result;
  memory::Arena work;
  memory::push(scratch, result, memory::available(scratch) / 4, "minimap-result");
  memory::push(scratch, work, "minimap-work");
  image::Image img;
  Status status = minimap(project, level, atlases, options, max_size, result, work, img);
  if (status.ok)
  {
    status = image::save_png(path, img, work);
  }
  memory::pop(scratch, work);
  memory::pop(scratch, result);
  return status;
}

}  // namespace lvl::render
