/**
 * @copyright 2026-present Brian Cairl
 *
 * @file wfc.hpp
 *
 * Wave-function-collapse (simple tiled model) over a tile layer, constrained by WfcRules:
 *
 * - adjacency: which tiles may neighbor which, per direction;
 * - per-tile layer constraints, seeded from other layers: a height band (against the normalized
 *   height layer), a water rule (against the water layer) and a semantic type;
 * - existing tiles: cells already holding a rule tile can be kept fixed, so hand-placed tiles steer
 *   generation, and a region can be regenerated inside a finished map.
 *
 * Contradictions restart the attempt with a derived seed, up to <code>max_attempts</code>.
 */
#pragma once

// C++ Standard Library
#include <cstdint>

// LVL
#include "lvl/core/jobs.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"
#include "lvl/level/wfc_rules.hpp"

namespace lvl::procgen
{

/// Cell rectangle (width/height 0 = the whole grid)
struct Region
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

/// Layers the generation is constrained by (each optional)
struct WfcConstraints
{
  const level::Grid<float>* height = nullptr;
  /// Height values mapped to [0, 1] against the tiles' height bands
  float height_lo = 0.0F;
  float height_hi = 1.0F;
  const level::Grid<float>* water = nullptr;
  /// Water depth above which a cell is wet
  float water_threshold = 1e-3F;
  const level::Grid<std::uint16_t>* semantic = nullptr;
  /// Tiles already placed (cells holding a rule tile are kept when <code>keep_existing</code>)
  const level::Grid<level::TileRef>* existing = nullptr;
};

struct WfcParams
{
  std::uint64_t seed = 1;
  int max_attempts = 10;
  bool keep_existing = true;
  Region region;
};

struct WfcResult
{
  Status status;
  int attempts = 0;
  std::size_t contradictions = 0;
  std::size_t collapsed = 0;
};

/**
 * @brief Generates tiles into <code>out</code> within <code>params.region</code>
 *
 * Cells outside the region are left unchanged (and, when they hold rule tiles, constrain the
 * region's border). Working memory comes from <code>scratch</code> (rewound afterwards).
 */
WfcResult solve(
  const level::WfcRules& rules,
  const WfcConstraints& constraints,
  const WfcParams& params,
  level::Grid<level::TileRef>& out,
  memory::Arena& scratch,
  jobs::Progress* progress = nullptr);

}  // namespace lvl::procgen
