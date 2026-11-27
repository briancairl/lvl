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
    layers{memory::ArenaAllocator<Layer>{a}},
    graphs{memory::ArenaAllocator<PathGraph>{a}},
    registry{memory::ArenaAllocator<Entity>{a}}
{}

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


/// Clears or renumbers a tile reference once tileset <code>id</code> is gone
void fix_ref(TileRef& t, TileSetId id)
{
  if (t.tileset == id)
  {
    t = kEmptyTile;
  }
  else if (!t.empty() and t.tileset > id)
  {
    --t.tileset;
  }
}

void clear_tileset_refs(Level& level, TileSetId id)
{
  for (auto& layer : level.layers)
  {
    if (layer.kind == LayerKind::kTiles)
    {
      for (auto& t : plane<TileGrid>(layer).values)
      {
        fix_ref(t, id);
      }
    }
  }
}

void clear_tileset_refs(WfcRules& rules, TileSetId id)
{
  // A rule tile naming the removed tileset goes with it
  for (std::size_t i = rules.tiles.size(); i-- > 0;)
  {
    if (rules.tiles[i].tile.tileset == id)
    {
      remove_tile(rules, static_cast<WfcTileId>(i));
    }
    else
    {
      fix_ref(rules.tiles[i].tile, id);
    }
  }
}

void clear_tileset_refs(AutoTileRules& rules, TileSetId id)
{
  for (auto& terrain : rules.terrains)
  {
    for (std::size_t i = terrain.variants.size(); i-- > 0;)
    {
      if (terrain.variants[i].tile.tileset == id)
      {
        terrain.variants.erase(terrain.variants.begin() + static_cast<std::ptrdiff_t>(i));
      }
      else
      {
        fix_ref(terrain.variants[i].tile, id);
      }
    }
  }
}

void clear_tileset_refs(StepRules& rules, TileSetId id)
{
  // A step terrain's eighteen are a fixed set, so a removed one is emptied in place
  for (auto& terrain : rules.terrains)
  {
    for (TileRef& tile : terrain.tiles)
    {
      fix_ref(tile, id);
    }
  }
}

namespace
{

/// Remaps one catalog id after entry <code>removed</code> is erased
}  // namespace

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
template <typename Fn>
void for_each_visible_tile(const TileSets& tilesets, const Level& level, std::string_view season, Fn&& fn)
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
      if (t.empty() or t.tileset >= tilesets.size())
      {
        continue;
      }
      const auto& ts = tilesets[t.tileset];
      const TileIndex drawn = seasonal_tile(ts, t.tile, season);
      if (drawn < ts.tiles.size())
      {
        fn(i, ts, t.tile, drawn);
      }
    }
  }
}

}  // namespace

std::size_t derive_collision(
  const TileSets& tilesets,
  Level& level,
  std::size_t collision_layer,
  std::string_view season,
  CollisionDerive mode,
  memory::Arena& scratch)
{
  LVL_ASSERT_LT(collision_layer, level.layers.size());
  LVL_ASSERT(level.layers[collision_layer].kind == LayerKind::kCollision);
  auto& out = plane<MaskGrid>(level.layers[collision_layer]);

  memory::Scope scope{scratch};
  // The visible tile layers' planes, so a cell's whole union is known before anything is written:
  // that is what lets the two modes share one pass and still report what really changed -- clearing
  // the layer first and ORing layer by layer would count a cell that ends up as it began
  auto planes = memory::make_vector<const TileGrid*>(scratch);
  planes.reserve(level.layers.size());
  for (const Layer& layer : level.layers)
  {
    if (layer.kind != LayerKind::kTiles or !layer.visible)
    {
      continue;
    }
    if (const auto& g = plane<TileGrid>(layer); g.extents == out.extents)
    {
      planes.push_back(&g);
    }
  }

  std::size_t changed = 0;
  for (std::size_t i = 0; i < out.values.size(); ++i)
  {
    std::uint8_t mask = (mode == CollisionDerive::kAdd) ? out.values[i] : collision::kNone;
    for (const TileGrid* g : planes)
    {
      const TileRef t = g->values[i];
      if (t.empty() or t.tileset >= tilesets.size())
      {
        continue;
      }
      const TileSet& ts = tilesets[t.tileset];
      const TileIndex drawn = seasonal_tile(ts, t.tile, season);
      if (drawn < ts.tiles.size())
      {
        mask = static_cast<std::uint8_t>(mask | ts.tiles[drawn].collision);
      }
    }
    changed += (mask != out.values[i]) ? 1 : 0;
    out.values[i] = mask;
  }
  return changed;
}

void derive_materials(const TileSets& tilesets, Level& level, std::size_t material_layer, std::string_view season)
{
  LVL_ASSERT_LT(material_layer, level.layers.size());
  LVL_ASSERT(level.layers[material_layer].kind == LayerKind::kMaterial);
  auto& out = plane<IdGrid>(level.layers[material_layer]);
  out.fill(kNoMaterial);
  for_each_visible_tile(
    tilesets, level, season, [&out, season](std::size_t i, const TileSet& ts, TileIndex tile, TileIndex) {
      if (const MaterialId m = seasonal_material(ts, tile, season); m != kNoMaterial)
      {
        out.values[i] = m;
      }
    });
}

std::size_t derive_semantics(
  const TileSets& tilesets,
  Level& level,
  std::size_t semantic_layer,
  const std::size_t* tile_layers,
  std::size_t count,
  std::string_view season,
  bool keep_unmatched,
  memory::Arena& scratch)
{
  LVL_ASSERT_LT(semantic_layer, level.layers.size());
  LVL_ASSERT(level.layers[semantic_layer].kind == LayerKind::kSemantic);
  auto& out = plane<IdGrid>(level.layers[semantic_layer]);

  memory::Scope scope{scratch};
  // The chosen tile layers' planes, top first, so a cell stops at the first tile that has a
  // semantic type. Built from the level's own order rather than the caller's list, which is what
  // makes "the layer on top wins" a property of the level and not of how the call was written
  auto planes = memory::make_vector<const TileGrid*>(scratch);
  planes.reserve(count);
  for (std::size_t n = level.layers.size(); n > 0; --n)
  {
    const std::size_t index = n - 1;
    const Layer& layer = level.layers[index];
    if (layer.kind != LayerKind::kTiles or index == semantic_layer)
    {
      continue;
    }
    if (std::find(tile_layers, tile_layers + count, index) == tile_layers + count)
    {
      continue;
    }
    const auto& g = plane<TileGrid>(layer);
    if (g.extents == out.extents)
    {
      planes.push_back(&g);
    }
  }

  std::size_t changed = 0;
  for (std::size_t i = 0; i < out.values.size(); ++i)
  {
    SemanticId value = kNoSemantic;
    for (const TileGrid* g : planes)
    {
      const TileRef t = g->values[i];
      if (t.empty() or t.tileset >= tilesets.size())
      {
        continue;
      }
      const TileSet& ts = tilesets[t.tileset];
      const TileIndex drawn = seasonal_tile(ts, t.tile, season);
      if (drawn >= ts.tiles.size())
      {
        continue;
      }
      if (const SemanticId s = ts.tiles[drawn].semantic; s != kNoSemantic)
      {
        value = s;
        break;
      }
    }
    if (value == kNoSemantic and keep_unmatched)
    {
      continue;
    }
    changed += (out.values[i] == value) ? 0U : 1U;
    out.values[i] = value;
  }
  return changed;
}

}  // namespace lvl::level
