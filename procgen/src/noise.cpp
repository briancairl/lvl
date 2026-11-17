/**
 * @copyright 2026-present Brian Cairl
 *
 * @file noise.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>
#include <limits>

// LVL
#include "lvl/core/random.hpp"
#include "lvl/procgen/noise.hpp"

namespace lvl::procgen
{
namespace
{

constexpr float fade(float t) { return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F); }

constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Gradient at an integer lattice point, from a hash (8 directions)
float gradient(std::uint64_t seed, int ix, int iy, float dx, float dy)
{
  const auto h = static_cast<std::uint32_t>(random::hash(seed, ix, iy) >> 61);  // 0..7
  constexpr float kDiag = 0.70710678F;
  switch (h)
  {
  case 0:
    return dx;
  case 1:
    return -dx;
  case 2:
    return dy;
  case 3:
    return -dy;
  case 4:
    return kDiag * (dx + dy);
  case 5:
    return kDiag * (dx - dy);
  case 6:
    return kDiag * (-dx + dy);
  default:
    return kDiag * (-dx - dy);
  }
}

float luminance(const image::Rgba& p)
{
  return (0.2126F * static_cast<float>(p.r) + 0.7152F * static_cast<float>(p.g) + 0.0722F * static_cast<float>(p.b)) /
    255.0F;
}

float combine(Blend blend, float a, float b)
{
  switch (blend)
  {
  case Blend::kAdd:
    return a + b;
  case Blend::kMultiply:
    return a * b;
  case Blend::kMax:
    return std::max(a, b);
  case Blend::kMin:
    return std::min(a, b);
  }
  return a + b;
}

}  // namespace

float perlin(std::uint64_t seed, float x, float y)
{
  const float fx = std::floor(x);
  const float fy = std::floor(y);
  const int ix = static_cast<int>(fx);
  const int iy = static_cast<int>(fy);
  const float dx = x - fx;
  const float dy = y - fy;
  const float u = fade(dx);
  const float v = fade(dy);
  const float n00 = gradient(seed, ix, iy, dx, dy);
  const float n10 = gradient(seed, ix + 1, iy, dx - 1.0F, dy);
  const float n01 = gradient(seed, ix, iy + 1, dx, dy - 1.0F);
  const float n11 = gradient(seed, ix + 1, iy + 1, dx - 1.0F, dy - 1.0F);
  // Scale so the output spans roughly [-1, 1]
  return 1.4142F * lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
}

float sample(const NoiseLayer& layer, std::uint64_t seed, float x, float y)
{
  const std::uint64_t s = seed + layer.seed_offset * 0x9E3779B97F4A7C15ULL;
  float frequency = layer.frequency;
  float amplitude = 1.0F;
  float sum = 0.0F;
  float norm = 0.0F;
  const int octaves = std::clamp(layer.octaves, 1, 16);
  for (int o = 0; o < octaves; ++o)
  {
    float n = perlin(
      s + static_cast<std::uint64_t>(o) * 0x632BE59BD9B4E019ULL,
      (x + layer.offset_x) * frequency,
      (y + layer.offset_y) * frequency);
    switch (layer.kind)
    {
    case NoiseKind::kPerlin:
      n = 0.5F * (n + 1.0F);
      break;
    case NoiseKind::kRidged:
      n = 1.0F - std::abs(n);
      n *= n;
      break;
    case NoiseKind::kBillow:
      n = std::abs(n);
      break;
    }
    sum += amplitude * n;
    norm += amplitude;
    frequency *= layer.lacunarity;
    amplitude *= layer.gain;
  }
  return layer.amplitude * (norm > 0.0F ? sum / norm : 0.0F);
}

float sample_image(const image::Image& img, level::Extents extents, float x, float y)
{
  // Grid cell centers mapped onto pixel centers
  const float px = (x + 0.5F) * static_cast<float>(img.width) / static_cast<float>(extents.width) - 0.5F;
  const float py = (y + 0.5F) * static_cast<float>(img.height) / static_cast<float>(extents.height) - 0.5F;
  const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, img.width - 1);
  const int y0 = std::clamp(static_cast<int>(std::floor(py)), 0, img.height - 1);
  const int x1 = std::min(x0 + 1, img.width - 1);
  const int y1 = std::min(y0 + 1, img.height - 1);
  const float tx = std::clamp(px - static_cast<float>(x0), 0.0F, 1.0F);
  const float ty = std::clamp(py - static_cast<float>(y0), 0.0F, 1.0F);
  const float a = lerp(luminance(img.at(x0, y0)), luminance(img.at(x1, y0)), tx);
  const float b = lerp(luminance(img.at(x0, y1)), luminance(img.at(x1, y1)), tx);
  return lerp(a, b, ty);
}

void generate_heightmap(
  const HeightmapParams& params,
  std::uint64_t seed,
  level::Grid<float>& out,
  const image::Image* seed_image,
  std::size_t workers,
  jobs::Progress* progress)
{
  const level::Extents e = out.extents;
  const bool use_image = (seed_image != nullptr) and !seed_image->empty() and params.seed_weight > 0.0F;
  const float noise_weight = use_image ? (1.0F - std::clamp(params.seed_weight, 0.0F, 1.0F)) : 1.0F;
  const std::size_t layer_count = std::min(params.layer_count, kMaxNoiseLayers);
  if (progress != nullptr)
  {
    progress->reset(static_cast<std::size_t>(e.height) + 1);
    progress->stage("Generating noise");
  }

  jobs::parallel_for(static_cast<std::size_t>(e.height), workers, [&](std::size_t row, std::size_t) {
    const int y = static_cast<int>(row);
    for (int x = 0; x < e.width; ++x)
    {
      float h = 0.0F;
      bool first = true;
      for (std::size_t l = 0; l < layer_count; ++l)
      {
        const NoiseLayer& layer = params.layers[l];
        if (!layer.enabled)
        {
          continue;
        }
        const float n = sample(layer, seed, static_cast<float>(x), static_cast<float>(y));
        h = first ? n : combine(layer.blend, h, n);
        first = false;
      }
      h *= noise_weight;
      if (use_image)
      {
        float lum = sample_image(*seed_image, e, static_cast<float>(x), static_cast<float>(y));
        lum = params.seed_invert ? 1.0F - lum : lum;
        h += std::clamp(params.seed_weight, 0.0F, 1.0F) * lum;
      }
      if (params.falloff > 0.0F)
      {
        const float nx = 2.0F * (static_cast<float>(x) + 0.5F) / static_cast<float>(e.width) - 1.0F;
        const float ny = 2.0F * (static_cast<float>(y) + 0.5F) / static_cast<float>(e.height) - 1.0F;
        const float d = std::min(1.0F, std::sqrt(nx * nx + ny * ny) / 1.41421356F);
        h *= 1.0F - std::clamp(params.falloff, 0.0F, 1.0F) * d * d;
      }
      out.values[e.index(x, y)] = h;
    }
    if (progress != nullptr)
    {
      progress->advance();
    }
  });

  if (params.normalize and !out.values.empty())
  {
    const auto [lo, hi] = std::minmax_element(out.values.begin(), out.values.end());
    const float min = *lo;
    const float range = std::max(*hi - *lo, std::numeric_limits<float>::epsilon());
    const float exponent = std::max(params.exponent, 0.01F);
    for (float& h : out.values)
    {
      h = (h - min) / range;
      if (exponent != 1.0F)
      {
        h = std::pow(h, exponent);
      }
    }
  }
  if (progress != nullptr)
  {
    progress->advance();
  }
}

bool load(json::Reader& r, const Json& j, NoiseLayer& v)
{
  r.object(j, "noise layer");
  r.get_optional(j, "enabled", v.enabled);
  r.get_optional(j, "kind", v.kind);
  r.get_optional(j, "blend", v.blend);
  r.get_optional(j, "frequency", v.frequency);
  r.get_optional(j, "amplitude", v.amplitude);
  r.get_optional(j, "octaves", v.octaves);
  r.get_optional(j, "lacunarity", v.lacunarity);
  r.get_optional(j, "gain", v.gain);
  r.get_optional(j, "offset_x", v.offset_x);
  r.get_optional(j, "offset_y", v.offset_y);
  r.get_optional(j, "seed_offset", v.seed_offset);
  return r.ok();
}

void save(Json& j, const NoiseLayer& v)
{
  j = Json::object();
  json::write(j, "enabled", v.enabled);
  json::write(j, "kind", v.kind);
  json::write(j, "blend", v.blend);
  json::write(j, "frequency", v.frequency);
  json::write(j, "amplitude", v.amplitude);
  json::write(j, "octaves", v.octaves);
  json::write(j, "lacunarity", v.lacunarity);
  json::write(j, "gain", v.gain);
  json::write(j, "offset_x", v.offset_x);
  json::write(j, "offset_y", v.offset_y);
  json::write(j, "seed_offset", v.seed_offset);
}

bool load(json::Reader& r, const Json& j, HeightmapParams& v)
{
  r.object(j, "heightmap");
  if (const auto layers = j.find("layers"); layers != j.end() and r.array(*layers, "layers"))
  {
    v.layer_count = std::min(layers->size(), kMaxNoiseLayers);
    for (std::size_t i = 0; i < v.layer_count; ++i)
    {
      r.convert((*layers)[i], v.layers[i], "layers");
    }
  }
  r.get_optional(j, "seed_weight", v.seed_weight);
  r.get_optional(j, "seed_invert", v.seed_invert);
  r.get_optional(j, "falloff", v.falloff);
  r.get_optional(j, "normalize", v.normalize);
  r.get_optional(j, "exponent", v.exponent);
  return r.ok();
}

void save(Json& j, const HeightmapParams& v)
{
  j = Json::object();
  Json layers = Json::array();
  for (std::size_t i = 0; i < v.layer_count; ++i)
  {
    Json e;
    save(e, v.layers[i]);
    layers.push_back(std::move(e));
  }
  j["layers"] = std::move(layers);
  json::write(j, "seed_weight", v.seed_weight);
  json::write(j, "seed_invert", v.seed_invert);
  json::write(j, "falloff", v.falloff);
  json::write(j, "normalize", v.normalize);
  json::write(j, "exponent", v.exponent);
}

}  // namespace lvl::procgen
