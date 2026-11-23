/**
 * @copyright 2026-present Brian Cairl
 *
 * @file level.hpp
 *
 * The level document: grid settings, catalogs, tilesets, layers, seasons, WFC and auto-tiling
 * rules, path graphs and objects. Everything it holds is allocated from one document arena; <code>reset()</code>
 * rewinds that arena wholesale (new/load), so there is no per-object freeing to get wrong.
 */
#pragma once

// C++ Standard Library
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/auto_tile_rules.hpp"
#include "lvl/level/layer.hpp"
#include "lvl/level/objects.hpp"
#include "lvl/level/step_rules.hpp"
#include "lvl/level/tileset.hpp"
#include "lvl/level/types.hpp"
#include "lvl/level/wfc_rules.hpp"

namespace lvl::level
{

struct Settings
{
  Name name = "untitled";
  /// Grid size, in cells
  Extents extents = {64, 64};
  /// Cell size, in pixels (drawing/export scale)
  int cell_width = 16;
  int cell_height = 16;
  /// Seed for procedural generation
  std::uint64_t seed = 1;
};

bool load(json::Reader& r, const Json& j, Settings& s);
void save(Json& j, const Settings& s);

struct Level
{
  explicit Level(memory::Arena& arena);

  Level(const Level&) = delete;
  Level& operator=(const Level&) = delete;

  /// Arena all of this level's data lives in
  memory::Arena* arena;

  Settings settings;
  /// Semantic types; index 0 is the reserved "none" entry
  memory::Vector<SemanticType> semantics;
  /// Materials; index 0 is the reserved "none" entry
  memory::Vector<Material> materials;
  /// Seasons, in yearly order (at least one)
  memory::Vector<Season> seasons;
  memory::Vector<TileSet> tilesets;
  /// Layers, bottom to top
  memory::Vector<Layer> layers;
  WfcRules wfc;
  /// Auto-tiling terrains (the other kind of tile adjacency rules; see auto_tile_rules.hpp)
  AutoTileRules auto_tiles;
  /// Height-step terrains, keyed on the difference in level between cells (see step_rules.hpp)
  StepRules steps;
  memory::Vector<PathGraph> graphs;
  Registry registry;
};

/**
 * @brief Destroys everything in <code>level</code>, rewinds its arena, and re-creates it empty
 *
 * The result has the default catalogs (a "none" semantic type and material) and one season.
 */
void reset(Level& level);

/// A new level of the given settings with a starter set of layers (tiles, height, collision)
void create_default(Level& level, const Settings& settings);

/// Resizes the level grid and every layer (keeping overlapping cells)
void resize(Level& level, Extents extents);

/// Appends a layer; returns its index
std::size_t add_layer(Level& level, LayerKind kind, std::string_view name);

/// Removes layer <code>index</code>
void remove_layer(Level& level, std::size_t index);

/// Moves layer <code>from</code> to position <code>to</code>
void move_layer(Level& level, std::size_t from, std::size_t to);

/// Index of the first layer of <code>kind</code>, or <code>level.layers.size()</code>
std::size_t find_layer(const Level& level, LayerKind kind);

/// Appends a tileset; returns its id
TileSetId add_tileset(Level& level, std::string_view name);

/// Removes a tileset, clearing tile cells that referenced it and renumbering later tilesets
void remove_tileset(Level& level, TileSetId id);

/// Index of the catalog entry named <code>name</code> (adding it if missing)
SemanticId intern_semantic(Level& level, std::string_view name);
MaterialId intern_material(Level& level, std::string_view name);

/**
 * @brief Removes a catalog entry; everything that used it becomes "none" (id 0)
 *
 * Remapped: semantic/material layer cells, tile metadata, seasonal variants (materials) and WFC
 * tiles (semantics); ids after the removed one shift down. Id 0 ("none") can't be removed.
 */
void remove_semantic(Level& level, SemanticId id);
void remove_material(Level& level, MaterialId id);

/// How many cells, tiles, seasonal variants and WFC tiles use a catalog entry
std::size_t semantic_uses(const Level& level, SemanticId id);
std::size_t material_uses(const Level& level, MaterialId id);

/// Removes a season (a level keeps at least one); tile variants for it stay in their tilesets
void remove_season(Level& level, SeasonId id);

/// Index of the season named <code>name</code>, or <code>level.seasons.size()</code>
SeasonId find_season(const Level& level, std::string_view name);

/// Adds a path graph; returns its id
GraphId add_graph(Level& level, std::string_view name, GraphKind kind);

/// Removes a path graph and its nodes/edges
void remove_graph(Level& level, GraphId id);

/**
 * @brief Recomputes a collision layer from the tile layers' tile metadata
 *
 * Each cell gets the union of the collision masks of the tiles on every visible tile layer at that
 * cell, as drawn during <code>season</code>.
 */
void derive_collision(Level& level, std::size_t collision_layer, std::string_view season);

/// As derive_collision, for a material layer (topmost non-empty tile with a material wins)
void derive_materials(Level& level, std::size_t material_layer, std::string_view season);

}  // namespace lvl::level
