/**
 * @copyright 2026-present Brian Cairl
 *
 * @file steps.hpp
 *
 * Terracing: a continuous heightmap cut into whole height levels, and the tiles that draw the steps
 * between them.
 *
 * Two passes, kept apart because the first is the one that matters. <code>terrace</code> turns the
 * height layer into integer levels, which is what the game is really given -- gft reads a height
 * per cell and works out the tiling itself. <code>tile_steps</code> then draws those levels with a
 * step terrain's eighteen tiles (lvl::level::StepRules), so the editor shows what the game will
 * draw, and marks the collision layer where a step would be walked through.
 *
 * Both are plain passes over the planes, with no state between cells, so re-running after an edit
 * gives the same answer.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/memory.hpp"
#include "lvl/level/layer.hpp"
#include "lvl/level/step_rules.hpp"
#include "lvl/level/types.hpp"

namespace lvl::procgen
{

/// How the height range is divided into levels
enum class TerraceMode : std::uint8_t
{
  /// Into <code>levels</code> bands of equal height, whatever the range turns out to be
  kCount,
  /// Into bands <code>rate</code> units tall, so a steeper map simply gets more of them
  kRate,
};

struct TerraceParams
{
  TerraceMode mode = TerraceMode::kCount;
  /// Bands to cut the range into (kCount); at least 1
  int levels = 4;
  /// Height per band (kRate); must be above 0
  float rate = 0.25F;
  /// Height mapped to level 0, and the top of the range; equal values are taken from the data
  float lo = 0.0F;
  float hi = 0.0F;
  /// Levels are clamped to this, so a stray spike cannot run off the top
  int max_level = 255;
};

struct TerraceResult
{
  /// Levels actually used, so <code>top</code> is the highest level any cell holds
  int top = 0;
  /// Cells whose level changed
  std::size_t changed = 0;
  /// Height the bands came out at
  float rate = 0.0F;
};

/**
 * @brief Cuts <code>height</code> into whole levels
 *
 * With <code>lo</code> and <code>hi</code> equal, the range is taken from the heightmap itself, so
 * a map terraces sensibly without anyone reading its numbers first. A cell exactly on a band's
 * boundary belongs to the upper band, and the very top of the range belongs to the last one rather
 * than to a band of its own.
 */
TerraceResult terrace(const level::ScalarGrid& height, const TerraceParams& params, level::IdGrid& levels);

/// How a level layer is tidied so every terrace and every hole is at least so many cells across
struct TidyParams
{
  /// Narrowest feature to keep, in cells. 2 is the one that matters (see <code>tidy_levels</code>)
  int min_feature = 2;
  /// Shave off spurs narrower than that (a morphological opening of each terrace)
  bool remove_spurs = true;
  /// Fill notches and holes narrower than that (a closing)
  bool fill_notches = true;
  /// Square off terraces that meet only at a corner, which would put a 45-degree edge in the map
  bool square_corners = true;
  /// Rounds of the three; it stops early as soon as a round changes nothing
  int passes = 8;
};

struct TidyResult
{
  /// Cells whose level changed
  std::size_t changed = 0;
  /// Rounds actually run
  int passes = 0;
  /// Whether the last round changed nothing, so the result is settled
  bool stable = false;
};

/**
 * @brief Widens every terrace and every hole in a level layer to <code>min_feature</code> cells
 *
 * A one-cell spur or notch in the levels has no tile: gft's table names 20 of the 255 ways a cell's
 * neighbors can stand below it, and *every* arrangement it does not name has some feature only one
 * cell across. So a level layer whose terraces and holes are all at least two cells wide is exactly
 * a level layer the step tiles can draw completely, and this is the pass that gets one there.
 * <code>tile_steps</code> counts what is left over as <code>unexpressed</code>.
 *
 * The levels are ordered, so this works on the *upper sets* -- the cells at level k or above, for
 * each k -- rather than on each level on its own: an opening shaves the spurs off a terrace, a
 * closing fills its notches, and a cell's new level is the highest upper set it still belongs to.
 * Going through the upper sets is what keeps the result a sensible heightmap instead of a patchwork
 * of levels that disagree about who owns a cell.
 *
 * The two operations undo a little of each other's work, so they are run in rounds until a round
 * changes nothing (<code>passes</code> caps it, and <code>stable</code> says whether it got there).
 * Cells off the grid count as copies of the nearest cell on it, so nothing is shaved off the map's
 * own edge. <code>scratch</code> holds a few planes over the grid, and is rewound.
 */
TidyResult tidy_levels(level::IdGrid& levels, const TidyParams& params, memory::Arena& scratch);

/// What a step fill should do to the collision layer
enum class StepCollision : std::uint8_t
{
  /// Leave it alone
  kNone,
  /// Block the cells at the foot of a step, so the cliff cannot be walked into from below
  kBelow,
  /// Block the cells at the top of a step, so its edge cannot be walked along
  kAbove,
};

struct StepFillParams
{
  StepCollision collision = StepCollision::kNone;
};

struct StepFillResult
{
  /// Cells given a tile, counted over every layer
  std::size_t tiled = 0;
  /// Cells whose tile changed
  std::size_t changed = 0;
  /// Cells no terrain reaches, so nothing is drawn there at all
  std::size_t untermed = 0;
  /// Cells whose terrain has no tile for the pattern they need
  std::size_t missing = 0;
  /// Cells marked solid (0 unless <code>collision</code> asks for it)
  std::size_t blocked = 0;
  /// Cells with ground below them and none above, that the table still has no tile for. These are
  /// the one-cell spurs and 45-degree edges <code>tidy_levels</code> exists to remove
  std::size_t unexpressed = 0;
};

/**
 * @brief Draws the terraces and the cliffs between them, a layer per height level
 *
 * <code>layers</code> holds one tile layer per height level: <code>layers[k]</code> is the terrace
 * at level k, and the array wants one entry per level the map uses, so a map cut into five levels
 * wants five layers. A null entry leaves that level undrawn.
 *
 * **They have to be separate layers.** A cliff tile is drawn with transparency, because what shows
 * through it is the ground of the terrace below -- that is how these tilesets are cut -- so the
 * terraces have to sit one on top of another and be composited, not flattened into one plane.
 * Stacked in order, level 0 at the bottom, they come out right.
 *
 * Layer k draws the cells at level k or above: the middle of that ground flat, its rim edges and
 * corners, and everything below it cleared, so re-running after the levels change leaves nothing
 * behind. Level 0 therefore covers the whole map and is flat throughout, which is the opaque base
 * the rest composite over. Within one layer the only question is whether a neighbor is part of the
 * same ground, so no cell is ever "above" another here; that is what the stacking expresses.
 *
 * Which *art* a level is drawn in comes from the terrain whose band holds it
 * (<code>level::step_terrain_at</code>), so a map can step from grass cliffs up to rock ones by
 * giving the bands different terrains -- and a single terrain covering every level is the ordinary
 * case, drawing every terrace in the one set of eighteen tiles. Cells at a level no terrain covers
 * are counted in <code>untermed</code> and left alone.
 *
 * Cells off the grid count as part of the same ground, so the map's edge is not a cliff.
 *
 * Only the tile *visual* is split this way. <code>collision</code> -- and any other per-cell data a
 * level keeps, semantics and materials among them -- stays a single flat layer, because it follows
 * from the levels rather than from any one terrace. When it is not <code>kNone</code> this sets or
 * clears <code>collision::kSolid</code> over the whole grid -- set where a step is, clear where it
 * is not -- leaving the other bits of the mask alone.
 */
StepFillResult tile_steps(
  const level::StepRules& rules,
  const level::IdGrid& levels,
  const StepFillParams& params,
  level::Grid<level::TileRef>* const* layers,
  std::size_t layer_count,
  level::MaskGrid* collision);

}  // namespace lvl::procgen
