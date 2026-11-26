/**
 * @copyright 2026-present Brian Cairl
 *
 * @file scatter.hpp
 *
 * Objects scattered over an area: a set of tile groups (houses, trees, rocks) dropped at random
 * positions that meet the same kind of criteria a masked fill uses, each placement whole and clear
 * of the others.
 *
 * This is the object counterpart of <code>fill_masked</code>. A fill decides every cell
 * independently, which is right for ground cover and wrong for a thing: a house is several tiles
 * that only mean something together, so a scatter places a whole <code>level::TileGroup</code> at a
 * time and refuses a position where any of its cells would not fit. What "fit" means is the mask --
 * a height band, a ground type, the collision mask, the cells that already have a tile -- plus the
 * room the objects already placed have taken, which is why this cannot be a per-cell function and
 * has to walk candidate positions instead.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/memory.hpp"
#include "lvl/level/layer.hpp"
#include "lvl/level/tileset.hpp"
#include "lvl/level/types.hpp"
#include "lvl/procgen/wfc.hpp"

namespace lvl::procgen
{

/**
 * @brief One kind of object a scatter may place
 *
 * The tileset and the group are ids rather than names because this is the data form: the editor's
 * profiles name them, since a name survives a tileset being removed or a group being renumbered,
 * and resolve them to ids on the way in.
 */
struct ScatterObject
{
  level::TileSetId tileset = level::kNoTileSet;
  level::GroupId group = level::kNoGroup;
  /// Share of the placements this object takes, relative to the others; 0 keeps it out
  float weight = 1.0F;
};

/**
 * @brief Where a scatter may put an object
 *
 * Every test is optional and **every cell of a placement** must pass all of them, which is the
 * difference from <code>FillMask</code>: half a house on the wrong ground is worse than no house.
 * The two layer tests are by plane rather than by layer kind, so the band may be a heightmap or
 * water depth and the ids a semantic or a material layer; every plane given must have the same
 * extents as the tiles being written.
 */
struct ScatterMask
{
  /// Scalar layer whose value must fall in [<code>lo</code>, <code>hi</code>] (null: no band test)
  const level::ScalarGrid* band = nullptr;
  float lo = 0.0F;
  float hi = 1.0F;
  /// Id layer which must hold <code>id</code> (null: no id test)
  const level::IdGrid* ids = nullptr;
  std::uint16_t id = 0;
  /// Collision layer no placement may touch (null: no collision test)
  const level::MaskGrid* collision = nullptr;
  /// Which bits of that mask block a placement
  std::uint8_t blocked = level::collision::kSolid;
  /// Cells outside this rectangle are left alone (zero width or height: the whole grid)
  Region region;
  /// Only place where every cell of the object has no tile yet, so what is drawn survives
  bool only_empty = true;
};

struct ScatterParams
{
  /// How many objects to place
  int count = 24;
  /// Cells of clearance kept between placements; 0 lets two objects touch
  int spacing = 0;
  /// Candidate positions tried per object before that one is given up on
  int attempts = 64;
};

struct ScatterResult
{
  /// Objects placed
  std::size_t placed = 0;
  /// Cells written (a hole in a group writes nothing)
  std::size_t cells = 0;
  /// Candidate positions the mask or another placement refused
  std::size_t rejected = 0;
  /// Objects given up on after <code>attempts</code> positions, so the area had no room left
  std::size_t unplaced = 0;
};

/**
 * @brief Places <code>params.count</code> of <code>objects</code> at random positions <code>mask</code> allows
 *
 * A candidate position is a cell of the region; the object's top-left corner goes there and every
 * cell the group is not a hole at must pass the mask and be clear of what is already placed. A
 * position that fails is tried again with another, up to <code>params.attempts</code> times per
 * object, after which that object counts as <code>unplaced</code> -- so a full area ends the run
 * rather than looping.
 *
 * Which object a placement takes is decided by <code>weight</code>, relative to the others: twice
 * the weight, twice the share. A weight of 0 keeps an object out, negative weights count as 0, and
 * weights that are all 0 stand for a weight of 1 each, so an untouched profile scatters evenly.
 *
 * The run is a function of <code>seed</code> alone -- the generator is drawn from in a fixed order
 * -- so scattering twice over the same map writes the same objects and only the seed moves them.
 * Unlike a fill it is *not* stable under a changed mask, because where an object fits depends on
 * where the others went; a scatter is something you re-run, not something you narrow.
 *
 * No cell is ever written twice: a candidate's solid cells must be clear of every box already
 * placed, gaps included, so nothing is dropped into a tree's gap. A candidate's own gaps are not
 * tested, though, so a later object's gap may sit over an earlier object's tile -- two trees
 * interlocking through each other's gaps, which is what you want of them.
 *
 * <code>scratch</code> holds one byte per cell (the room taken so far) and is rewound on return.
 * Objects naming a tileset or group that is not there, and ones whose group is larger than the
 * region, are skipped.
 */
ScatterResult scatter_groups(
  const level::TileSets& tilesets,
  const ScatterObject* objects,
  std::size_t object_count,
  const ScatterMask& mask,
  const ScatterParams& params,
  std::uint64_t seed,
  memory::Arena& scratch,
  level::Grid<level::TileRef>& out);

}  // namespace lvl::procgen
