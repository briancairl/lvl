/**
 * @copyright 2026-present Brian Cairl
 *
 * @file level.hpp
 *
 * The level document: grid settings, layers, path graphs
 * rules, path graphs and objects. Everything it holds is allocated from one document arena; <code>reset()</code>
 * rewinds that arena wholesale (new/load), so there is no per-object freeing to get wrong.
 */
#pragma once

// C++ Standard Library
#include <algorithm>
#include <cstddef>
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
  /**
   * @brief Which of its project's rule sets this level auto-tiles with, by name
   *
   * Not part of <code>Settings</code>, because it is a reference to the project rather than a
   * setting of the level, and because the two formats carry it differently: a file that carries its
   * rules already names them (the rules' own name), so only a project-scope file has to say which
   * set it means. Empty resolves to the project's first set (see <code>project::rules_of</code>),
   * which is what every file written before rule sets were plural gets.
   */
  Name rules;
  /// Layers, bottom to top
  memory::Vector<Layer> layers;
  memory::Vector<PathGraph> graphs;
  Registry registry;
};

/**
 * @brief Destroys everything in <code>level</code>, rewinds its arena, and re-creates it empty
 *
 * The catalogs it draws on belong to its project.
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

/**
 * @brief Clears or renumbers every tile cell of <code>level</code> naming tileset <code>id</code>
 *
 * Cells that held it become empty and later tilesets shift down. The tileset itself belongs to the
 * project, as do the rule sets that name it, each of which has its own overload (see
 * <code>wfc_rules.hpp</code>, <code>auto_tile_rules.hpp</code>, <code>step_rules.hpp</code>);
 * <code>project::remove_tileset</code> drives them all.
 */
void clear_tileset_refs(Level& level, TileSetId id);

/// Applies <code>fn(std::uint16_t&)</code> to every cell of every layer of <code>kind</code>
template <typename Fn> void for_each_id_cell(Level& level, LayerKind kind, Fn&& fn)
{
  for (auto& layer : level.layers)
  {
    if (layer.kind == kind)
    {
      for (auto& v : plane<IdGrid>(layer).values)
      {
        fn(v);
      }
    }
  }
}

/// Counts the cells of every layer of <code>kind</code> that <code>match</code> accepts
template <typename Fn> std::size_t count_id_cells(const Level& level, LayerKind kind, Fn&& match)
{
  std::size_t n = 0;
  for (const auto& layer : level.layers)
  {
    if (layer.kind == kind)
    {
      n += static_cast<std::size_t>(
        std::count_if(plane<IdGrid>(layer).values.begin(), plane<IdGrid>(layer).values.end(), match));
    }
  }
  return n;
}

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
void derive_collision(const TileSets& tilesets, Level& level, std::size_t collision_layer, std::string_view season);

/// As derive_collision, for a material layer (topmost non-empty tile with a material wins)
void derive_materials(const TileSets& tilesets, Level& level, std::size_t material_layer, std::string_view season);

}  // namespace lvl::level
