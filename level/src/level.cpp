/**
 * @copyright 2026-present Brian Cairl
 *
 * @file level.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <new>
#include <utility>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/level/level.hpp"

namespace lvl::level
{

Level::Level(memory::Arena& a) :
    arena{&a},
    semantics{memory::ArenaAllocator<SemanticType>{a}},
    materials{memory::ArenaAllocator<Material>{a}},
    seasons{memory::ArenaAllocator<Season>{a}},
    tilesets{memory::ArenaAllocator<TileSet>{a}},
    layers{memory::ArenaAllocator<Layer>{a}},
    wfc{a},
    graphs{memory::ArenaAllocator<PathGraph>{a}},
    registry{memory::ArenaAllocator<Entity>{a}}
{
  semantics.push_back(SemanticType{.name = "none", .color = {0, 0, 0, 0}});
  materials.push_back(Material{.name = "none", .color = {0, 0, 0, 0}});
  seasons.push_back(Season{.name = "default"});
}

void reset(Level& level)
{
  memory::Arena& arena = *level.arena;
  level.~Level();
  memory::clear(arena);
  new (&level) Level{arena};
}

void create_default(Level& level, const Settings& settings)
{
  reset(level);
  level.settings = settings;
  add_layer(level, LayerKind::kHeight, "height");
  add_layer(level, LayerKind::kTiles, "ground");
  add_layer(level, LayerKind::kTiles, "detail");
  add_layer(level, LayerKind::kCollision, "collision");
  level.layers.front().visible = false;
  level.wfc.name = "rules";
}

void resize(Level& level, Extents extents)
{
  LVL_ASSERT_GT(extents.width, 0);
  LVL_ASSERT_GT(extents.height, 0);
  level.settings.extents = extents;
  for (auto& layer : level.layers)
  {
    resize(layer, extents);
  }
}

std::size_t add_layer(Level& level, LayerKind kind, std::string_view name)
{
  level.layers.push_back(make_layer(*level.arena, kind, name, level.settings.extents));
  return level.layers.size() - 1;
}

void remove_layer(Level& level, std::size_t index)
{
  LVL_ASSERT_LT(index, level.layers.size());
  level.layers.erase(level.layers.begin() + static_cast<std::ptrdiff_t>(index));
}

void move_layer(Level& level, std::size_t from, std::size_t to)
{
  LVL_ASSERT_LT(from, level.layers.size());
  LVL_ASSERT_LT(to, level.layers.size());
  if (from < to)
  {
    std::rotate(
      level.layers.begin() + static_cast<std::ptrdiff_t>(from),
      level.layers.begin() + static_cast<std::ptrdiff_t>(from) + 1,
      level.layers.begin() + static_cast<std::ptrdiff_t>(to) + 1);
  }
  else if (from > to)
  {
    std::rotate(
      level.layers.begin() + static_cast<std::ptrdiff_t>(to),
      level.layers.begin() + static_cast<std::ptrdiff_t>(from),
      level.layers.begin() + static_cast<std::ptrdiff_t>(from) + 1);
  }
}

std::size_t find_layer(const Level& level, LayerKind kind)
{
  const auto itr =
    std::find_if(level.layers.begin(), level.layers.end(), [kind](const Layer& l) { return l.kind == kind; });
  return static_cast<std::size_t>(itr - level.layers.begin());
}

TileSetId add_tileset(Level& level, std::string_view name)
{
  LVL_ASSERT_LT(level.tilesets.size(), static_cast<std::size_t>(kNoTileSet)) << "too many tilesets";
  level.tilesets.emplace_back(*level.arena);
  level.tilesets.back().name.assign(name);
  return static_cast<TileSetId>(level.tilesets.size() - 1);
}

void remove_tileset(Level& level, TileSetId id)
{
  LVL_ASSERT_LT(id, level.tilesets.size());
  level.tilesets.erase(level.tilesets.begin() + id);
  const auto fix = [id](TileRef& t) {
    if (t.tileset == id)
    {
      t = kEmptyTile;
    }
    else if (!t.empty() and t.tileset > id)
    {
      --t.tileset;
    }
  };
  for (auto& layer : level.layers)
  {
    if (layer.kind == LayerKind::kTiles)
    {
      for (auto& t : plane<TileGrid>(layer).values)
      {
        fix(t);
      }
    }
  }
  // WFC tiles referencing the removed tileset go too
  for (std::size_t i = level.wfc.tiles.size(); i-- > 0;)
  {
    if (level.wfc.tiles[i].tile.tileset == id)
    {
      remove_tile(level.wfc, static_cast<WfcTileId>(i));
    }
    else
    {
      fix(level.wfc.tiles[i].tile);
    }
  }
}

SemanticId intern_semantic(Level& level, std::string_view name)
{
  for (std::size_t i = 0; i < level.semantics.size(); ++i)
  {
    if (level.semantics[i].name == name)
    {
      return static_cast<SemanticId>(i);
    }
  }
  level.semantics.push_back(SemanticType{.name = Name{name}});
  return static_cast<SemanticId>(level.semantics.size() - 1);
}

MaterialId intern_material(Level& level, std::string_view name)
{
  for (std::size_t i = 0; i < level.materials.size(); ++i)
  {
    if (level.materials[i].name == name)
    {
      return static_cast<MaterialId>(i);
    }
  }
  level.materials.push_back(Material{.name = Name{name}});
  return static_cast<MaterialId>(level.materials.size() - 1);
}

SeasonId find_season(const Level& level, std::string_view name)
{
  for (std::size_t i = 0; i < level.seasons.size(); ++i)
  {
    if (level.seasons[i].name == name)
    {
      return static_cast<SeasonId>(i);
    }
  }
  return static_cast<SeasonId>(level.seasons.size());
}

GraphId add_graph(Level& level, std::string_view name, GraphKind kind)
{
  level.graphs.push_back(PathGraph{.name = Name{name}, .kind = kind});
  return static_cast<GraphId>(level.graphs.size() - 1);
}

void remove_graph(Level& level, GraphId id)
{
  LVL_ASSERT_LT(id, level.graphs.size());
  destroy_path_graph(level.registry, id);
  level.graphs.erase(level.graphs.begin() + id);
}

namespace
{

/// Calls <code>fn(cell index, tileset, placed tile, drawn tile)</code> for every non-empty tile, bottom layer first
template <typename Fn> void for_each_visible_tile(const Level& level, std::string_view season, Fn&& fn)
{
  for (const auto& layer : level.layers)
  {
    if (layer.kind != LayerKind::kTiles or !layer.visible)
    {
      continue;
    }
    const auto& g = plane<TileGrid>(layer);
    for (std::size_t i = 0; i < g.values.size(); ++i)
    {
      const TileRef t = g.values[i];
      if (t.empty() or t.tileset >= level.tilesets.size())
      {
        continue;
      }
      const auto& ts = level.tilesets[t.tileset];
      const TileIndex drawn = seasonal_tile(ts, t.tile, season);
      if (drawn < ts.tiles.size())
      {
        fn(i, ts, t.tile, drawn);
      }
    }
  }
}

}  // namespace

void derive_collision(Level& level, std::size_t collision_layer, std::string_view season)
{
  LVL_ASSERT_LT(collision_layer, level.layers.size());
  LVL_ASSERT(level.layers[collision_layer].kind == LayerKind::kCollision);
  auto& out = plane<MaskGrid>(level.layers[collision_layer]);
  out.fill(collision::kNone);
  for_each_visible_tile(level, season, [&out](std::size_t i, const TileSet& ts, TileIndex, TileIndex drawn) {
    out.values[i] |= ts.tiles[drawn].collision;
  });
}

void derive_materials(Level& level, std::size_t material_layer, std::string_view season)
{
  LVL_ASSERT_LT(material_layer, level.layers.size());
  LVL_ASSERT(level.layers[material_layer].kind == LayerKind::kMaterial);
  auto& out = plane<IdGrid>(level.layers[material_layer]);
  out.fill(kNoMaterial);
  for_each_visible_tile(level, season, [&out, season](std::size_t i, const TileSet& ts, TileIndex tile, TileIndex) {
    if (const MaterialId m = seasonal_material(ts, tile, season); m != kNoMaterial)
    {
      out.values[i] = m;
    }
  });
}

}  // namespace lvl::level
