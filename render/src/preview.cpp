/**
 * @copyright 2026-present Brian Cairl
 *
 * @file preview.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <bit>
#include <cmath>

// LVL
#include "lvl/render/preview.hpp"

namespace lvl::render
{
namespace
{

using image::Rgba;

struct Stop
{
  float t;
  Rgba color;
};

Rgba ramp(const Stop* stops, std::size_t n, float t)
{
  t = std::clamp(t, 0.0F, 1.0F);
  for (std::size_t i = 1; i < n; ++i)
  {
    if (t <= stops[i].t)
    {
      const float u = (t - stops[i - 1].t) / std::max(stops[i].t - stops[i - 1].t, 1e-6F);
      const auto mix = [u](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * u);
      };
      const Rgba& a = stops[i - 1].color;
      const Rgba& b = stops[i].color;
      return Rgba{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255};
    }
  }
  return stops[n - 1].color;
}

float normalized(const level::Layer& layer, float v)
{
  const float range = std::max(layer.range_max - layer.range_min, 1e-6F);
  return (v - layer.range_min) / range;
}

}  // namespace

Rgba terrain_ramp(float t)
{
  static constexpr Stop kStops[] = {
    {0.00F, {20, 40, 110, 255}},
    {0.30F, {40, 100, 180, 255}},
    {0.35F, {210, 200, 140, 255}},
    {0.45F, {90, 160, 70, 255}},
    {0.65F, {50, 110, 50, 255}},
    {0.80F, {120, 110, 100, 255}},
    {0.92F, {170, 165, 160, 255}},
    {1.00F, {250, 250, 250, 255}},
  };
  return ramp(kStops, std::size(kStops), t);
}

Rgba heat_ramp(float t)
{
  static constexpr Stop kStops[] = {
    {0.00F, {30, 30, 160, 255}},
    {0.33F, {30, 200, 220, 255}},
    {0.66F, {250, 230, 40, 255}},
    {1.00F, {220, 30, 20, 255}},
  };
  return ramp(kStops, std::size(kStops), t);
}

Rgba id_color(std::uint32_t id)
{
  std::uint32_t h = id * 2654435761U;
  h ^= h >> 15;
  return Rgba{
    static_cast<std::uint8_t>(64 + (h & 0x7F)),
    static_cast<std::uint8_t>(64 + ((h >> 8) & 0x7F)),
    static_cast<std::uint8_t>(64 + ((h >> 16) & 0x7F)),
    255};
}

void colorize(
  const level::Level& level,
  const level::Layer& layer,
  image::Image& out,
  const Atlases* atlases,
  std::string_view season)
{
  using namespace level;
  const Extents e = extents(layer);
  LVL_ASSERT(out.width == e.width and out.height == e.height) << "preview image must match the layer size";
  const std::size_t n = e.count();
  switch (layer.kind)
  {
  case LayerKind::kTiles: {
    const auto& g = plane<TileGrid>(layer);
    for (std::size_t i = 0; i < n; ++i)
    {
      const TileRef t = g.values[i];
      Rgba c{0, 0, 0, 0};
      if (!t.empty() and t.tileset < level.tilesets.size())
      {
        const TileIndex drawn = seasonal_tile(level.tilesets[t.tileset], t.tile, season);
        if (
          atlases != nullptr and atlases->tile_colors != nullptr and t.tileset < atlases->count and
          drawn < atlases->tile_colors[t.tileset].size())
        {
          c = atlases->tile_colors[t.tileset][drawn];
        }
        else
        {
          c = id_color(static_cast<std::uint32_t>(t.tileset) * 65536U + drawn);
        }
      }
      out.pixels[i] = c;
    }
    break;
  }
  case LayerKind::kSemantic:
  case LayerKind::kMaterial: {
    const auto& g = plane<IdGrid>(layer);
    for (std::size_t i = 0; i < n; ++i)
    {
      const std::uint16_t id = g.values[i];
      Rgba c{0, 0, 0, 0};
      if (id != 0)
      {
        if (layer.kind == LayerKind::kSemantic)
        {
          c = (id < level.semantics.size()) ? level.semantics[id].color : id_color(id);
        }
        else
        {
          c = (id < level.materials.size()) ? level.materials[id].color : id_color(id);
        }
      }
      out.pixels[i] = c;
    }
    break;
  }
  case LayerKind::kCollision: {
    const auto& g = plane<MaskGrid>(layer);
    for (std::size_t i = 0; i < n; ++i)
    {
      const std::uint8_t m = g.values[i];
      Rgba c{0, 0, 0, 0};
      if ((m & collision::kSolid) != 0)
      {
        // Intensity by number of solid quadrants
        const int quads = std::popcount(static_cast<unsigned>(m & collision::kSolid));
        c = Rgba{230, 40, 40, static_cast<std::uint8_t>(60 + 45 * quads)};
      }
      else if ((m & collision::kPlatform) != 0)
      {
        c = Rgba{240, 160, 30, 160};
      }
      else if ((m & collision::kTrigger) != 0)
      {
        c = Rgba{160, 60, 240, 140};
      }
      else if ((m & collision::kSightBlocker) != 0)
      {
        c = Rgba{60, 60, 60, 140};
      }
      out.pixels[i] = c;
    }
    break;
  }
  case LayerKind::kHeight:
  case LayerKind::kWater: {
    const auto& g = plane<ScalarGrid>(layer);
    const bool water = (layer.kind == LayerKind::kWater);
    for (int y = 0; y < e.height; ++y)
    {
      for (int x = 0; x < e.width; ++x)
      {
        const std::size_t i = e.index(x, y);
        const float v = g.values[i];
        const float t = normalized(layer, v);
        Rgba c{0, 0, 0, 0};
        switch (layer.preview)
        {
        case Preview::kNatural:
          if (water)
          {
            c = (v > 1e-4F)
              ? Rgba{40, 110, 220, static_cast<std::uint8_t>(std::clamp(80.0F + 175.0F * t, 0.0F, 255.0F))}
              : Rgba{0, 0, 0, 0};
          }
          else
          {
            const auto l = static_cast<std::uint8_t>(std::clamp(t, 0.0F, 1.0F) * 255.0F);
            c = Rgba{l, l, l, 255};
          }
          break;
        case Preview::kTerrain:
          c = terrain_ramp(t);
          break;
        case Preview::kHeat:
          c = heat_ramp(t);
          if (water and v <= 1e-4F)
          {
            c.a = 0;
          }
          break;
        case Preview::kContours: {
          // A line where a contour level is crossed between this cell and its east/south neighbors
          const float interval = std::max(layer.contour_interval, 1e-4F);
          const auto band = [&](float value) { return std::floor(normalized(layer, value) / interval); };
          const float b = band(v);
          const bool edge =
            (x + 1 < e.width and band(g.at(x + 1, y)) != b) or (y + 1 < e.height and band(g.at(x, y + 1)) != b);
          c = edge ? Rgba{20, 20, 20, 230} : Rgba{0, 0, 0, 0};
          break;
        }
        case Preview::kHillshade: {
          const float scale = 8.0F / std::max(layer.range_max - layer.range_min, 1e-6F);
          const float dzdx = (g.get(std::min(x + 1, e.width - 1), y) - g.get(std::max(x - 1, 0), y)) * 0.5F * scale;
          const float dzdy = (g.get(x, std::min(y + 1, e.height - 1)) - g.get(x, std::max(y - 1, 0))) * 0.5F * scale;
          // Light from the north-west, 45 degrees up
          const float nx = -dzdx;
          const float ny = -dzdy;
          const float nz = 1.0F;
          const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
          const float shade = std::clamp((-0.5F * nx - 0.5F * ny + 0.7071F * nz) / len, 0.0F, 1.0F);
          const auto l = static_cast<std::uint8_t>(shade * 255.0F);
          c = Rgba{l, l, l, 255};
          break;
        }
        }
        out.pixels[i] = c;
      }
    }
    break;
  }
  }
}

void average_tile_colors(const level::TileSet& ts, const image::Image& atlas, memory::Vector<image::Rgba>& out)
{
  out.assign(ts.tiles.size(), Rgba{0, 0, 0, 0});
  if (atlas.empty())
  {
    return;
  }
  for (std::size_t t = 0; t < ts.tiles.size(); ++t)
  {
    const auto r = level::tile_rect(ts.slice, atlas.width, atlas.height, static_cast<level::TileIndex>(t));
    std::uint64_t sum[3] = {0, 0, 0};
    std::uint64_t weight = 0;
    for (int y = r.y; y < std::min(r.y + r.height, atlas.height); ++y)
    {
      for (int x = r.x; x < std::min(r.x + r.width, atlas.width); ++x)
      {
        const Rgba p = atlas.at(x, y);
        sum[0] += static_cast<std::uint64_t>(p.r) * p.a;
        sum[1] += static_cast<std::uint64_t>(p.g) * p.a;
        sum[2] += static_cast<std::uint64_t>(p.b) * p.a;
        weight += p.a;
      }
    }
    if (weight > 0)
    {
      out[t] = Rgba{
        static_cast<std::uint8_t>(sum[0] / weight),
        static_cast<std::uint8_t>(sum[1] / weight),
        static_cast<std::uint8_t>(sum[2] / weight),
        255};
    }
  }
}

}  // namespace lvl::render
