/**
 * @copyright 2026-present Brian Cairl
 *
 * @file noise.hpp
 *
 * Heightmap creation from layered noise (Perlin fBm, ridged, billow) and/or a seed image.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/json.hpp"
#include "lvl/level/grid.hpp"

namespace lvl::procgen
{

enum class NoiseKind : std::uint8_t
{
  /// Smooth fractal Brownian motion of Perlin gradient noise
  kPerlin,
  /// 1 - |noise|, sharpened: mountain ridges
  kRidged,
  /// |noise|: rounded hills
  kBillow,
};

/// How a noise layer combines with the layers below it
enum class Blend : std::uint8_t
{
  kAdd,
  kMultiply,
  kMax,
  kMin,
};

struct NoiseLayer
{
  bool enabled = true;
  NoiseKind kind = NoiseKind::kPerlin;
  Blend blend = Blend::kAdd;
  /// Features per cell of the base octave (e.g. 0.02 = one feature every 50 cells)
  float frequency = 0.02F;
  float amplitude = 1.0F;
  int octaves = 5;
  /// Frequency multiplier per octave
  float lacunarity = 2.0F;
  /// Amplitude multiplier per octave
  float gain = 0.5F;
  /// Domain offset, in cells
  float offset_x = 0.0F;
  float offset_y = 0.0F;
  /// Added to the generation seed (layers with the same seed offset share a noise field)
  std::uint32_t seed_offset = 0;
};

inline constexpr std::size_t kMaxNoiseLayers = 8;

struct HeightmapParams
{
  std::array<NoiseLayer, kMaxNoiseLayers> layers = {};
  std::size_t layer_count = 1;
  /// Weight of the seed image (if one is given); 0 = noise only, 1 = image only
  float seed_weight = 0.5F;
  /// Invert the seed image's luminance
  bool seed_invert = false;
  /// Fade to 0 towards the grid edges (island shapes); 0 = off, 1 = strong
  float falloff = 0.0F;
  /// Remap the result to [0, 1]
  bool normalize = true;
  /// Raises values to this power after normalizing (>1 flattens lowlands, <1 raises them)
  float exponent = 1.0F;
};

/// 2D Perlin gradient noise in [-1, 1] (approximately), deterministic per seed
float perlin(std::uint64_t seed, float x, float y);

/// Fractal sum of one noise layer at (x, y)
float sample(const NoiseLayer& layer, std::uint64_t seed, float x, float y);

/**
 * @brief Generates a heightmap into <code>out</code> (rows in parallel on up to <code>workers</code> threads)
 *
 * @param seed_image optional image, bilinearly resampled to the grid; its luminance is blended with
 *        the noise by <code>params.seed_weight</code>
 */
void generate_heightmap(
  const HeightmapParams& params,
  std::uint64_t seed,
  level::Grid<float>& out,
  const image::Image* seed_image = nullptr,
  std::size_t workers = 1,
  jobs::Progress* progress = nullptr);

/// Normalized luminance of <code>img</code> at grid coordinates (bilinear), for a grid of <code>extents</code>
float sample_image(const image::Image& img, level::Extents extents, float x, float y);

bool load(json::Reader& r, const Json& j, NoiseLayer& v);
void save(Json& j, const NoiseLayer& v);
bool load(json::Reader& r, const Json& j, HeightmapParams& v);
void save(Json& j, const HeightmapParams& v);

}  // namespace lvl::procgen

template <> struct lvl::EnumNames<lvl::procgen::NoiseKind>
{
  static constexpr std::array<std::string_view, 3> kNames = {"perlin", "ridged", "billow"};
};

template <> struct lvl::EnumNames<lvl::procgen::Blend>
{
  static constexpr std::array<std::string_view, 4> kNames = {"add", "multiply", "max", "min"};
};
