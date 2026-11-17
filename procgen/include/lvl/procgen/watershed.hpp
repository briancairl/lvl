/**
 * @copyright 2026-present Brian Cairl
 *
 * @file watershed.hpp
 *
 * Iterative water-shedding over a heightmap: hydraulic (droplet) erosion, then drainage via
 * priority-flood routing (every cell drains to the grid border), flow accumulation, and extraction
 * of rivers, lakes and tunnels. Each iteration carves the rivers found into the terrain, so later
 * iterations follow (and deepen) established channels.
 *
 * Depressions are either filled (becoming lakes) or breached: the drainage path is cut through the
 * rim. Cuts deeper than <code>tunnel_threshold</code> become tunnels (water passes under the terrain,
 * which keeps its height); shallower cuts are carved as gorges.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/jobs.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/grid.hpp"

namespace lvl::procgen
{

/// Droplet-based hydraulic erosion
struct ErosionParams
{
  /// Droplets simulated (per watershed iteration)
  std::uint32_t droplets = 20000;
  int max_lifetime = 40;
  /// How much a droplet keeps its direction (0 = always downhill)
  float inertia = 0.05F;
  float capacity = 4.0F;
  float min_capacity = 0.01F;
  /// Fraction of excess sediment dropped per step
  float deposition = 0.3F;
  /// Fraction of free capacity eroded per step
  float erosion = 0.3F;
  float evaporation = 0.02F;
  float gravity = 4.0F;
  /// Erosion brush radius, in cells
  int radius = 2;
};

enum class DepressionMode : std::uint8_t
{
  /// Depressions fill with water (lakes)
  kFillLakes,
  /// Drainage cuts through depression rims (gorges, and tunnels where the cut is deep)
  kBreach,
};

struct WatershedParams
{
  /// Erode/route/carve passes
  int iterations = 3;
  bool erode = true;
  ErosionParams erosion;
  /// Water contributed by each cell
  float rainfall = 1.0F;
  /// Accumulated flow (in cells' worth of rain) above which a cell is a river
  float river_threshold = 60.0F;
  /// Water depth of a river at the threshold (grows logarithmically with flow)
  float river_depth = 0.02F;
  /// Terrain lowered per unit of river depth, per iteration
  float carve = 0.5F;
  DepressionMode mode = DepressionMode::kFillLakes;
  /// Minimum fill depth to count as lake
  float lake_min_depth = 0.002F;
  /// Breach cuts deeper than this become tunnels
  float tunnel_threshold = 0.05F;
};

/// Optional outputs alongside the water depth
struct WatershedOutputs
{
  /// Water depth per cell (required)
  level::Grid<float>* water = nullptr;
  /// If set, river/lake/tunnel cells get these ids (0 = leave unchanged)
  level::Grid<std::uint16_t>* semantic = nullptr;
  std::uint16_t river_id = 0;
  std::uint16_t lake_id = 0;
  std::uint16_t tunnel_id = 0;
  /// If set, accumulated flow per cell (for previews)
  level::Grid<float>* flow = nullptr;
};

struct WatershedResult
{
  std::size_t river_cells = 0;
  std::size_t lake_cells = 0;
  std::size_t tunnel_cells = 0;
};

/// Runs droplet erosion on <code>height</code> (deterministic per seed)
void erode(
  level::Grid<float>& height,
  const ErosionParams& params,
  std::uint64_t seed,
  jobs::Progress* progress = nullptr);

/**
 * @brief Runs iterative water-shedding, modifying <code>height</code> and writing <code>outputs</code>
 *
 * Working memory comes from <code>scratch</code> (rewound afterwards).
 */
WatershedResult run_watershed(
  level::Grid<float>& height,
  const WatershedOutputs& outputs,
  const WatershedParams& params,
  std::uint64_t seed,
  memory::Arena& scratch,
  jobs::Progress* progress = nullptr);

bool load(json::Reader& r, const Json& j, ErosionParams& v);
void save(Json& j, const ErosionParams& v);
bool load(json::Reader& r, const Json& j, WatershedParams& v);
void save(Json& j, const WatershedParams& v);

}  // namespace lvl::procgen

template <> struct lvl::EnumNames<lvl::procgen::DepressionMode>
{
  static constexpr std::array<std::string_view, 2> kNames = {"fill_lakes", "breach"};
};
