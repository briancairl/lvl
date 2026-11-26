/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io_json.cpp
 *
 * JSON level format. Layout of a level file:
 *
 * @code
 * {
 *   "format": "lvl-level", "version": 100,
 *   "settings": {...}, "semantics": [...], "materials": [...], "seasons": [...],
 *   "tilesets": [...], "wfc": {...}, "auto_tiles": [...], "steps": [...], "paths": [...], "portals": [...], "markers":
 * [...], "layers": [ {"name": ..., "kind": ..., ..., "data": [...]}, ... ]
 * }
 * @endcode
 *
 * "layers" is always written last, one layer per line. On load, a scanner finds the text of each
 * layer object; the rest of the document is parsed with "layers" replaced by [] on this thread,
 * while the layers are parsed in parallel by IoWorkers.
 */

// C++ Standard Library
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

// LVL
#include "io_common.hpp"
#include "lvl/core/build.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/visitor.hpp"

namespace lvl::level
{

bool load(json::Reader& r, const Json& j, Settings& s)
{
  r.object(j, "settings");
  r.get(j, "name", s.name);
  r.get(j, "size", s.extents);
  r.get_optional(j, "cell_width", s.cell_width);
  r.get_optional(j, "cell_height", s.cell_height);
  r.get_optional(j, "seed", s.seed);
  if (r.ok() and (s.cell_width <= 0 or s.cell_height <= 0))
  {
    return r.fail("cell size must be positive");
  }
  return r.ok();
}

void save(Json& j, const Settings& s)
{
  j = Json::object();
  json::write(j, "name", s.name);
  json::write(j, "size", s.extents);
  json::write(j, "cell_width", s.cell_width);
  json::write(j, "cell_height", s.cell_height);
  json::write(j, "seed", s.seed);
}

}  // namespace lvl::level

namespace lvl::level::io
{

void save(Json& j, const LayerHeader& h)
{
  j = Json::object();
  json::write(j, "name", h.name);
  json::write(j, "kind", h.kind);
  json::write(j, "visible", h.visible);
  json::write(j, "locked", h.locked);
  json::write(j, "opacity", h.opacity);
  json::write(j, "preview", h.preview);
  json::write(j, "range", std::array<float, 2>{h.range_min, h.range_max});
  json::write(j, "contour_interval", h.contour_interval);
}

bool load(json::Reader& r, const Json& j, LayerHeader& h)
{
  std::array<float, 2> range = {h.range_min, h.range_max};
  r.object(j, "layer");
  r.get(j, "name", h.name);
  r.get(j, "kind", h.kind);
  r.get_optional(j, "visible", h.visible);
  r.get_optional(j, "locked", h.locked);
  r.get_optional(j, "opacity", h.opacity);
  r.get_optional(j, "preview", h.preview);
  r.get_optional(j, "range", range);
  r.get_optional(j, "contour_interval", h.contour_interval);
  h.range_min = range[0];
  h.range_max = range[1];
  return r.ok();
}

namespace
{

using CharVector = memory::Vector<char>;

void append(CharVector& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

// -------------------------------------------------------------------------------------------------
// Tile cell encoding: -1 empty, else tileset * 65536 + tile
// -------------------------------------------------------------------------------------------------

constexpr std::int64_t encode(TileRef t)
{
  return t.empty() ? -1 : static_cast<std::int64_t>(t.tileset) * 65536 + t.tile;
}

bool decode(std::int64_t v, TileRef& t)
{
  if (v == -1)
  {
    t = kEmptyTile;
    return true;
  }
  if (v < 0 or v >= static_cast<std::int64_t>(kNoTileSet) * 65536)
  {
    return false;
  }
  t = TileRef{static_cast<TileSetId>(v / 65536), static_cast<TileIndex>(v % 65536)};
  return true;
}

// -------------------------------------------------------------------------------------------------
// Settings, positions
// -------------------------------------------------------------------------------------------------

bool read_position(json::Reader& r, const Json& j, std::string_view key, Position& p)
{
  std::array<float, 2> xy = {p.x, p.y};
  if (r.get(j, key, xy))
  {
    p = Position{xy[0], xy[1]};
  }
  return r.ok();
}

void write_position(Json& j, std::string_view key, Position p) { json::write(j, key, std::array<float, 2>{p.x, p.y}); }

// -------------------------------------------------------------------------------------------------
// Catalogs (all entries, by value)
// -------------------------------------------------------------------------------------------------

template <typename T>
void save_list(Json& j, std::string_view key, const memory::Vector<T>& values, std::size_t first = 0)
{
  Json array = Json::array();
  for (std::size_t i = first; i < values.size(); ++i)
  {
    Json element;
    json::write(element, values[i]);
    array.push_back(std::move(element));
  }
  j[JsonString{key}] = std::move(array);
}

/// Reads catalog entries, interning them by name into <code>level</code> (properties of new
/// entries are taken from the file)
template <typename T, typename InternFn>
bool load_catalog(json::Reader& r, const Json& j, std::string_view key, memory::Vector<T>& catalog, InternFn&& intern)
{
  const auto itr = j.find(key);
  if (itr == j.end())
  {
    return r.ok();
  }
  if (!r.array(*itr, key))
  {
    return false;
  }
  for (const auto& element : *itr)
  {
    T entry;
    if (!r.convert(element, entry, key))
    {
      return false;
    }
    const std::size_t before = catalog.size();
    const auto id = intern(entry.name.view());
    if (catalog.size() > before)
    {
      catalog[id] = entry;
    }
  }
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// Tilesets (catalog references by name)
// -------------------------------------------------------------------------------------------------

void save_collision(Json& j, std::uint8_t mask)
{
  j = Json::array();
  for (std::size_t bit = 0; bit < collision::kFlagNames.size(); ++bit)
  {
    if ((mask & (1U << bit)) != 0)
    {
      j.push_back(JsonString{collision::kFlagNames[bit]});
    }
  }
}

bool load_collision(json::Reader& r, const Json& j, std::uint8_t& mask)
{
  if (!r.array(j, "collision"))
  {
    return false;
  }
  mask = 0;
  for (const auto& flag : j)
  {
    Name name;
    if (!r.convert(flag, name, "collision"))
    {
      return false;
    }
    const auto itr = std::find(collision::kFlagNames.begin(), collision::kFlagNames.end(), name.view());
    if (itr == collision::kFlagNames.end())
    {
      return r.fail("unknown collision flag '%s'", name.c_str());
    }
    mask |= static_cast<std::uint8_t>(1U << (itr - collision::kFlagNames.begin()));
  }
  return true;
}

void save_tileset(Json& j, const project::Project& project, const TileSet& ts)
{
  j = Json::object();
  json::write(j, "name", ts.name);
  json::write(j, "image", ts.image);
  json::write(j, "image_width", ts.image_width);
  json::write(j, "image_height", ts.image_height);
  json::write(j, "slice", ts.slice);
  json::write(j, "tile_count", static_cast<std::uint32_t>(ts.tiles.size()));
  // Sparse: only tiles with non-default metadata
  Json tiles = Json::array();
  for (std::size_t i = 0; i < ts.tiles.size(); ++i)
  {
    const TileInfo& info = ts.tiles[i];
    if (
      info.name.empty() and info.semantic == kNoSemantic and info.material == kNoMaterial and
      info.collision == collision::kNone and !info.hidden and info.scatter_weight == 1.0F)
    {
      continue;
    }
    Json t = Json::object();
    json::write(t, "index", static_cast<std::uint32_t>(i));
    if (!info.name.empty())
    {
      json::write(t, "name", info.name);
    }
    if (info.semantic != kNoSemantic)
    {
      json::write(t, "semantic", project.semantics[info.semantic].name);
    }
    if (info.material != kNoMaterial)
    {
      json::write(t, "material", project.materials[info.material].name);
    }
    if (info.collision != collision::kNone)
    {
      save_collision(t["collision"], info.collision);
    }
    if (info.hidden)
    {
      json::write(t, "hidden", true);
    }
    if (info.scatter_weight != 1.0F)
    {
      json::write(t, "scatter_weight", info.scatter_weight);
    }
    tiles.push_back(std::move(t));
  }
  j["tiles"] = std::move(tiles);

  Json properties = Json::array();
  for (const auto& p : ts.properties)
  {
    Json e = Json::object();
    json::write(e, "tile", p.tile);
    json::write(e, "key", p.key);
    json::write(e, "value", p.value);
    properties.push_back(std::move(e));
  }
  j["properties"] = std::move(properties);

  Json seasonal = Json::array();
  for (const auto& v : ts.seasonal)
  {
    Json e = Json::object();
    json::write(e, "tile", v.tile);
    json::write(e, "season", v.season);
    json::write(e, "alternate", v.alternate);
    if (v.material != kNoMaterial)
    {
      json::write(e, "material", project.materials[v.material].name);
    }
    seasonal.push_back(std::move(e));
  }
  j["seasonal"] = std::move(seasonal);

  // Tile groups: a footprint and its cells, row-major, -1 for a hole (as a layer's empty cells are)
  if (!ts.groups.empty())
  {
    Json groups = Json::array();
    for (const TileGroup& g : ts.groups)
    {
      Json e = Json::object();
      json::write(e, "name", g.name);
      if (!g.folder.empty())
      {
        json::write(e, "folder", g.folder);
      }
      json::write(e, "width", g.size.width);
      json::write(e, "height", g.size.height);
      Json cells = Json::array();
      for (std::size_t i = 0; i < g.size.count(); ++i)
      {
        const TileIndex tile = group_cell(
          ts,
          g,
          static_cast<int>(i % static_cast<std::size_t>(g.size.width)),
          static_cast<int>(i / static_cast<std::size_t>(g.size.width)));
        cells.push_back((tile == kNoTileIndex) ? -1 : static_cast<std::int64_t>(tile));
      }
      e["cells"] = std::move(cells);
      groups.push_back(std::move(e));
    }
    j["groups"] = std::move(groups);
  }
}

bool load_tileset(json::Reader& r, const Json& j, project::Project& project, TileSet& ts)
{
  r.object(j, "tileset");
  r.get(j, "name", ts.name);
  r.get(j, "image", ts.image);
  r.get_optional(j, "image_width", ts.image_width);
  r.get_optional(j, "image_height", ts.image_height);
  r.get(j, "slice", ts.slice);
  std::uint32_t tile_count = 0;
  r.get(j, "tile_count", tile_count);
  if (!r.ok())
  {
    return false;
  }
  if (tile_count > 0xFFFF)
  {
    return r.fail("too many tiles (%u)", tile_count);
  }
  ts.tiles.assign(tile_count, TileInfo{});

  const auto read_name_id = [&r](const Json& e, std::string_view key, auto&& intern) -> std::uint16_t {
    Name name;
    if (e.contains(key) and r.get(e, key, name))
    {
      return static_cast<std::uint16_t>(intern(name.view()));
    }
    return 0;
  };
  const auto semantic = [&project](std::string_view name) { return project::intern_semantic(project, name); };
  const auto material = [&project](std::string_view name) { return project::intern_material(project, name); };

  const Json empty = Json::array();
  const auto list = [&](std::string_view key) -> const Json& {
    const auto itr = j.find(key);
    return (itr == j.end()) ? empty : *itr;
  };

  for (const auto& t : list("tiles"))
  {
    std::uint32_t index = 0;
    if (!r.get(t, "index", index))
    {
      return false;
    }
    if (index >= tile_count)
    {
      return r.fail("tile index %u out of range (tile_count %u)", index, tile_count);
    }
    TileInfo& info = ts.tiles[index];
    r.get_optional(t, "name", info.name);
    info.semantic = read_name_id(t, "semantic", semantic);
    info.material = read_name_id(t, "material", material);
    if (t.contains("collision"))
    {
      load_collision(r, t["collision"], info.collision);
    }
    r.get_optional(t, "hidden", info.hidden);
    r.get_optional(t, "scatter_weight", info.scatter_weight);
    if (!r.ok())
    {
      return false;
    }
  }
  for (const auto& e : list("properties"))
  {
    TileProperty p;
    r.get(e, "tile", p.tile);
    r.get(e, "key", p.key);
    r.get(e, "value", p.value);
    if (!r.ok())
    {
      return false;
    }
    if (p.tile >= tile_count)
    {
      return r.fail("property tile %u out of range", p.tile);
    }
    ts.properties.push_back(p);
  }
  for (const auto& e : list("seasonal"))
  {
    SeasonalVariant v;
    r.get(e, "tile", v.tile);
    r.get(e, "season", v.season);
    r.get(e, "alternate", v.alternate);
    v.material = read_name_id(e, "material", material);
    if (!r.ok())
    {
      return false;
    }
    if (v.tile >= tile_count or v.alternate >= tile_count)
    {
      return r.fail("seasonal variant tile %u/%u out of range", v.tile, v.alternate);
    }
    ts.seasonal.push_back(v);
  }
  for (const auto& e : list("groups"))
  {
    Name name;
    Name folder;
    int width = 0;
    int height = 0;
    r.get(e, "name", name);
    r.get_optional(e, "folder", folder);
    r.get(e, "width", width);
    r.get(e, "height", height);
    if (!r.ok())
    {
      return false;
    }
    if (width <= 0 or height <= 0 or width > kMaxGroupExtent or height > kMaxGroupExtent)
    {
      return r.fail("tile group '%s' has an invalid size %dx%d", name.c_str(), width, height);
    }
    const auto cells = e.find("cells");
    if (cells == e.end() or !r.array(*cells, "cells"))
    {
      return false;
    }
    const Extents size{width, height};
    if (cells->size() != size.count())
    {
      return r.fail("tile group '%s' has %zu cells, expected %zu", name.c_str(), cells->size(), size.count());
    }
    // A group is at most kMaxGroupExtent squared cells, so its cells are read on the stack and
    // handed to set_group, which is what keeps the tileset's cell table packed and in order
    std::array<TileIndex, static_cast<std::size_t>(kMaxGroupExtent) * kMaxGroupExtent> buffer{};
    std::size_t index = 0;
    for (const auto& c : *cells)
    {
      std::int64_t tile = -1;
      if (!r.convert(c, tile, "cell"))
      {
        return false;
      }
      if (tile >= static_cast<std::int64_t>(tile_count))
      {
        return r.fail(
          "tile group '%s' names tile %lld, past the tileset's %u",
          name.c_str(),
          static_cast<long long>(tile),
          tile_count);
      }
      buffer[index++] = (tile < 0) ? kNoTileIndex : static_cast<TileIndex>(tile);
    }
    const GroupId added = set_group(ts, name.view(), size, buffer.data());
    if (added == kNoGroup)
    {
      return r.fail("tile group '%s' holds no tiles", name.c_str());
    }
    ts.groups[added].folder = folder;
  }
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// WFC rules (tiles referenced by tileset name)
// -------------------------------------------------------------------------------------------------

void save_wfc(Json& j, const project::Project& project, const WfcRules& rules)
{
  j = Json::object();
  json::write(j, "name", rules.name);
  json::write(j, "diagonals", rules.diagonals);
  Json tiles = Json::array();
  for (const auto& t : rules.tiles)
  {
    Json e = Json::object();
    json::write(e, "tileset", project.tilesets[t.tile.tileset].name);
    json::write(e, "tile", t.tile.tile);
    json::write(e, "weight", t.weight);
    json::write(e, "height", std::array<float, 2>{t.height_min, t.height_max});
    json::write(e, "water", t.water);
    if (t.semantic != kNoSemantic)
    {
      json::write(e, "semantic", project.semantics[t.semantic].name);
    }
    tiles.push_back(std::move(e));
  }
  j["tiles"] = std::move(tiles);
  Json adjacency = Json::array();
  for (const auto& a : rules.adjacency)
  {
    Json e = Json::array();
    e.push_back(a.a);
    e.push_back(JsonString{enum_name(a.direction)});
    e.push_back(a.b);
    adjacency.push_back(std::move(e));
  }
  j["adjacency"] = std::move(adjacency);
}

bool load_wfc(json::Reader& r, const Json& j, project::Project& project, WfcRules& rules)
{
  rules.tiles.clear();
  rules.adjacency.clear();
  r.object(j, "wfc");
  r.get_optional(j, "name", rules.name);
  rules.diagonals = false;
  r.get_optional(j, "diagonals", rules.diagonals);
  const auto tiles = j.find("tiles");
  const auto adjacency = j.find("adjacency");
  if (tiles == j.end() or adjacency == j.end())
  {
    return r.fail("WFC rules need 'tiles' and 'adjacency'");
  }
  if (!r.array(*tiles, "tiles") or !r.array(*adjacency, "adjacency"))
  {
    return false;
  }
  for (const auto& e : *tiles)
  {
    WfcTile t;
    Name tileset;
    std::array<float, 2> band = {0.0F, 1.0F};
    Name semantic;
    r.get(e, "tileset", tileset);
    r.get(e, "tile", t.tile.tile);
    r.get_optional(e, "weight", t.weight);
    r.get_optional(e, "height", band);
    r.get_optional(e, "water", t.water);
    r.get_optional(e, "semantic", semantic);
    if (!r.ok())
    {
      return false;
    }
    const auto ts = std::find_if(
      project.tilesets.begin(), project.tilesets.end(), [&tileset](const TileSet& s) { return s.name == tileset; });
    if (ts == project.tilesets.end())
    {
      return r.fail("WFC tile refers to unknown tileset '%s'", tileset.c_str());
    }
    t.tile.tileset = static_cast<TileSetId>(ts - project.tilesets.begin());
    t.height_min = band[0];
    t.height_max = band[1];
    t.semantic = semantic.empty() ? kNoSemantic : project::intern_semantic(project, semantic.view());
    rules.tiles.push_back(t);
  }
  for (const auto& e : *adjacency)
  {
    if (!e.is_array() or e.size() != 3)
    {
      return r.fail("adjacency entries are [a, direction, b]");
    }
    WfcAdjacency a;
    r.convert(e[0], a.a, "adjacency");
    r.convert(e[1], a.direction, "adjacency");
    r.convert(e[2], a.b, "adjacency");
    if (!r.ok())
    {
      return false;
    }
    if (a.a >= rules.tiles.size() or a.b >= rules.tiles.size())
    {
      return r.fail("adjacency refers to WFC tile %u/%u of %zu", a.a, a.b, rules.tiles.size());
    }
    set_allowed(rules, a.a, a.b, a.direction, true);
  }
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// Auto-tiling rules (terrains; tiles referenced by tileset name)
// -------------------------------------------------------------------------------------------------

/// Index of the tileset named <code>name</code>, or kNoTileSet
TileSetId tileset_named(const TileSets& tilesets, const Name& name)
{
  for (std::size_t i = 0; i < tilesets.size(); ++i)
  {
    if (tilesets[i].name == name)
    {
      return static_cast<TileSetId>(i);
    }
  }
  return kNoTileSet;
}

void save_auto_tiles(Json& j, const TileSets& tilesets, const AutoTileRules& rules)
{
  j = Json::array();
  for (const AutoTerrain& terrain : rules.terrains)
  {
    Json t = Json::object();
    json::write(t, "name", terrain.name);
    json::write(t, "neighborhood", terrain.neighborhood);
    json::write(t, "expand", terrain.expand);
    Json variants = Json::array();
    for (const AutoVariant& v : terrain.variants)
    {
      Json e = Json::object();
      // The pattern reads as the directions the terrain continues in
      Json neighbors = Json::array();
      for (const Direction d : kNeighbors)
      {
        if (has(v.mask, d))
        {
          neighbors.push_back(JsonString{enum_name(d)});
        }
      }
      e["neighbors"] = std::move(neighbors);
      json::write(e, "tileset", tilesets[v.tile.tileset].name);
      json::write(e, "tile", v.tile.tile);
      variants.push_back(std::move(e));
    }
    t["variants"] = std::move(variants);
    j.push_back(std::move(t));
  }
}

bool load_auto_tiles(
  json::Reader& r,
  const Json& j,
  const TileSets& tilesets,
  memory::Arena& arena,
  AutoTileRules& rules)
{
  rules.terrains.clear();
  if (!r.array(j, "auto_tiles"))
  {
    return false;
  }
  for (const auto& t : j)
  {
    Name name;
    r.get(t, "name", name);
    if (!r.ok())
    {
      return false;
    }
    AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, name.view())];
    r.get_optional(t, "neighborhood", terrain.neighborhood);
    r.get_optional(t, "expand", terrain.expand);
    if (!r.ok())
    {
      return false;
    }
    if (terrain.expand < 0 or terrain.expand > 4)
    {
      return r.fail("terrain '%s' has an expand of %d, outside 0..4", name.c_str(), terrain.expand);
    }
    const auto variants = t.find("variants");
    if (variants == t.end())
    {
      continue;
    }
    if (!r.array(*variants, "variants"))
    {
      return false;
    }
    for (const auto& e : *variants)
    {
      Name tileset;
      TileRef tile;
      r.get(e, "tileset", tileset);
      r.get(e, "tile", tile.tile);
      const auto neighbors = e.find("neighbors");
      if (!r.ok())
      {
        return false;
      }
      if (neighbors == e.end() or !r.array(*neighbors, "neighbors"))
      {
        return r.fail("terrain variants need 'neighbors'");
      }
      NeighborMask mask = kNoNeighbors;
      for (const auto& n : *neighbors)
      {
        Direction d = Direction::kNorth;
        r.convert(n, d, "neighbors");
        mask = static_cast<NeighborMask>(mask | mask_of(d));
      }
      tile.tileset = tileset_named(tilesets, tileset);
      if (!r.ok())
      {
        return false;
      }
      if (tile.tileset == kNoTileSet)
      {
        return r.fail("terrain tile refers to unknown tileset '%s'", tileset.c_str());
      }
      set_variant(terrain, mask, tile);
    }
  }
  return r.ok();
}

void save_steps(Json& j, const TileSets& tilesets, const StepRules& rules)
{
  j = Json::array();
  for (const StepTerrain& terrain : rules.terrains)
  {
    Json t = Json::object();
    json::write(t, "name", terrain.name);
    json::write(t, "level_lo", terrain.level_lo);
    json::write(t, "level_hi", terrain.level_hi);
    // One entry per step type it has a tile for, named rather than positional so the file survives
    // the table growing a row
    Json tiles = Json::array();
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      const TileRef tile = terrain.tiles[i];
      if (tile.empty())
      {
        continue;
      }
      Json e = Json::object();
      json::write(e, "type", static_cast<StepType>(i));
      json::write(e, "tileset", tilesets[tile.tileset].name);
      json::write(e, "tile", tile.tile);
      tiles.push_back(std::move(e));
    }
    t["tiles"] = std::move(tiles);
    j.push_back(std::move(t));
  }
}

bool load_steps(json::Reader& r, const Json& j, const TileSets& tilesets, StepRules& rules)
{
  rules.terrains.clear();
  if (!r.array(j, "steps"))
  {
    return false;
  }
  for (const auto& t : j)
  {
    Name name;
    r.get(t, "name", name);
    if (!r.ok())
    {
      return false;
    }
    StepTerrain& terrain = rules.terrains[add_step_terrain(rules, name.view())];
    r.get_optional(t, "level_lo", terrain.level_lo);
    r.get_optional(t, "level_hi", terrain.level_hi);
    if (!r.ok())
    {
      return false;
    }
    const auto tiles = t.find("tiles");
    if (tiles == t.end())
    {
      continue;
    }
    if (!r.array(*tiles, "tiles"))
    {
      return false;
    }
    for (const auto& e : *tiles)
    {
      Name tileset;
      TileRef tile;
      StepType type = StepType::kFlat;
      r.get(e, "type", type);
      r.get(e, "tileset", tileset);
      r.get(e, "tile", tile.tile);
      tile.tileset = tileset_named(tilesets, tileset);
      if (!r.ok())
      {
        return false;
      }
      if (tile.tileset == kNoTileSet)
      {
        return r.fail("step tile refers to unknown tileset '%s'", tileset.c_str());
      }
      terrain.tiles[static_cast<std::size_t>(type)] = tile;
    }
  }
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// Objects
// -------------------------------------------------------------------------------------------------

void save_paths(Json& j, const Level& level, const ObjectRecords& records)
{
  j = Json::array();
  for (std::size_t g = 0; g < level.graphs.size(); ++g)
  {
    const PathGraph& graph = level.graphs[g];
    Json e = Json::object();
    json::write(e, "name", graph.name);
    json::write(e, "kind", graph.kind);
    json::write(e, "color", graph.color);
    json::write(e, "visible", graph.visible);
    // Nodes are numbered within their graph
    auto local = memory::make_vector<std::uint32_t>(*records.nodes.get_allocator().arena(), records.nodes.size(), 0U);
    Json nodes = Json::array();
    std::uint32_t count = 0;
    for (std::size_t n = 0; n < records.nodes.size(); ++n)
    {
      if (records.nodes[n].graph != g)
      {
        continue;
      }
      local[n] = count++;
      Json node = Json::object();
      write_position(node, "position", records.nodes[n].position);
      json::write(node, "wait", records.nodes[n].wait);
      nodes.push_back(std::move(node));
    }
    e["nodes"] = std::move(nodes);
    Json edges = Json::array();
    for (const auto& edge : records.edges)
    {
      if (records.nodes[edge.from].graph != g)
      {
        continue;
      }
      Json je = Json::object();
      json::write(je, "from", local[edge.from]);
      json::write(je, "to", local[edge.to]);
      json::write(je, "cost", edge.cost);
      json::write(je, "bidirectional", edge.bidirectional);
      edges.push_back(std::move(je));
    }
    e["edges"] = std::move(edges);
    j.push_back(std::move(e));
  }
}

bool load_paths(json::Reader& r, const Json& j, Level& level, ObjectRecords& records)
{
  if (!r.array(j, "paths"))
  {
    return false;
  }
  for (const auto& e : j)
  {
    PathGraph graph;
    r.get(e, "name", graph.name);
    r.get_optional(e, "kind", graph.kind);
    r.get_optional(e, "color", graph.color);
    r.get_optional(e, "visible", graph.visible);
    if (!r.ok())
    {
      return false;
    }
    const auto g = static_cast<GraphId>(level.graphs.size());
    level.graphs.push_back(graph);
    const auto first = static_cast<std::uint32_t>(records.nodes.size());
    if (const auto nodes = e.find("nodes"); nodes != e.end() and r.array(*nodes, "nodes"))
    {
      for (const auto& n : *nodes)
      {
        NodeRecord node{.graph = g};
        read_position(r, n, "position", node.position);
        r.get_optional(n, "wait", node.wait);
        records.nodes.push_back(node);
      }
    }
    const auto count = static_cast<std::uint32_t>(records.nodes.size()) - first;
    if (const auto edges = e.find("edges"); edges != e.end() and r.array(*edges, "edges"))
    {
      for (const auto& je : *edges)
      {
        EdgeRecord edge;
        r.get(je, "from", edge.from);
        r.get(je, "to", edge.to);
        r.get_optional(je, "cost", edge.cost);
        r.get_optional(je, "bidirectional", edge.bidirectional);
        if (r.ok() and (edge.from >= count or edge.to >= count))
        {
          return r.fail("path '%s' edge refers to node %u/%u of %u", graph.name.c_str(), edge.from, edge.to, count);
        }
        edge.from += first;
        edge.to += first;
        records.edges.push_back(edge);
      }
    }
    if (!r.ok())
    {
      return false;
    }
  }
  return r.ok();
}

void save_objects(Json& j, const ObjectRecords& records)
{
  Json portals = Json::array();
  for (const auto& p : records.portals)
  {
    Json e = Json::object();
    json::write(e, "name", p.name);
    write_position(e, "position", p.position);
    json::write(e, "extent", std::array<float, 2>{p.extent.width, p.extent.height});
    json::write(e, "target_level", p.target_level_field);
    json::write(e, "target_portal", p.portal.target_portal);
    json::write(e, "exit", p.portal.exit);
    json::write(e, "interact", p.portal.interact);
    portals.push_back(std::move(e));
  }
  j["portals"] = std::move(portals);
  Json markers = Json::array();
  for (const auto& m : records.markers)
  {
    Json e = Json::object();
    json::write(e, "name", m.name);
    json::write(e, "type", m.type);
    write_position(e, "position", m.position);
    markers.push_back(std::move(e));
  }
  j["markers"] = std::move(markers);
}

bool load_objects(json::Reader& r, const Json& j, ObjectRecords& records)
{
  if (const auto portals = j.find("portals"); portals != j.end() and r.array(*portals, "portals"))
  {
    for (const auto& e : *portals)
    {
      PortalRecord p;
      std::array<float, 2> extent = {1.0F, 1.0F};
      r.get(e, "name", p.name);
      read_position(r, e, "position", p.position);
      r.get_optional(e, "extent", extent);
      // Read wide, because a file written before projects holds a path here
      r.get_optional(e, "target_level", p.target_level_field);
      r.get_optional(e, "target_portal", p.portal.target_portal);
      r.get_optional(e, "exit", p.portal.exit);
      r.get_optional(e, "interact", p.portal.interact);
      p.extent = Extent{extent[0], extent[1]};
      records.portals.push_back(p);
    }
  }
  if (const auto markers = j.find("markers"); markers != j.end() and r.array(*markers, "markers"))
  {
    for (const auto& e : *markers)
    {
      MarkerRecord m;
      r.get(e, "name", m.name);
      r.get(e, "type", m.type);
      read_position(r, e, "position", m.position);
      records.markers.push_back(m);
    }
  }
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// Layers
// -------------------------------------------------------------------------------------------------

/// Widest text one cell of a layer of <code>kind</code> encodes to, comma included
std::size_t cell_width(const TileSets& tilesets, LayerKind kind)
{
  switch (plane_index(kind))
  {
  case 0: {
    // "-1" for an empty cell, else tileset * 65536 + tile, so as wide as the last tileset's cells
    const std::int64_t widest = tilesets.empty()
      ? 0
      : static_cast<std::int64_t>(tilesets.size() - 1) * 65536 + std::numeric_limits<TileIndex>::max();
    std::size_t digits = 1;
    for (std::int64_t v = widest; v >= 10; v /= 10)
    {
      ++digits;
    }
    // At least the two of "-1"
    return std::max<std::size_t>(digits, 2) + 1;
  }
  case 1:
    // 65535
    return 5 + 1;
  case 2:
    // The widest a float's shortest round-trip form gets, e.g. "-1.12854834e-32"
    return 15 + 1;
  default:
    // 255
    return 3 + 1;
  }
}

/**
 * @brief Upper bound on the text <code>layer</code> encodes to, header and data array included
 *
 * The buffer is reserved from this, so it must never be short: the header's keys, values and
 * punctuation run to a little over 300 bytes with every field at its widest, and a name's every
 * character could need escaping.
 */
std::size_t encoded_bound(const TileSets& tilesets, const Layer& layer)
{
  return 512 + 6 * layer.name.size() + extents(layer).count() * cell_width(tilesets, layer.kind);
}

/**
 * @brief Writes a layer's text straight to the file through a small fixed buffer
 *
 * The sink for a layer whose text is too big to hold in memory at all (see encoded_bound). It takes
 * the same operations a CharVector does, so encode_layer writes to either; it is serial, because the
 * writer is, and there is only ever one of them.
 */
class StreamSink
{
public:
  explicit StreamSink(file::Writer& writer) : writer_{&writer} {}

  void push_back(char c)
  {
    if (size_ == kBlock)
    {
      flush();
    }
    buffer_[size_++] = c;
  }

  void insert_at_end(std::string_view text)
  {
    while (!text.empty())
    {
      if (size_ == kBlock)
      {
        flush();
      }
      const std::size_t n = std::min(text.size(), kBlock - size_);
      std::memcpy(buffer_ + size_, text.data(), n);
      size_ += n;
      text.remove_prefix(n);
    }
  }

  void flush()
  {
    writer_->write(buffer_, size_);
    size_ = 0;
  }

private:
  static constexpr std::size_t kBlock = 8UL << 10;
  file::Writer* writer_;
  std::size_t size_ = 0;
  char buffer_[kBlock];
};

void append(StreamSink& out, std::string_view text) { out.insert_at_end(text); }

/// Encodes one layer as a single-line JSON object into <code>out</code>, a CharVector holding
/// <code>encoded_bound</code> bytes (so the buffer never has to grow) or a StreamSink
template <typename Sink> void encode_layer(const Layer& layer, IoWorker& worker, Sink& out)
{
  // Header via the JSON DOM (in the worker's JSON arena) ...
  with_json(worker, [&](IoWorker&) {
    json::Scope scope;
    Json header;
    save(header, header_of(layer));
    const JsonString text = json::dump(header, -1);
    // ... minus its closing brace, then the data array written directly (no DOM for the bulk)
    append(out, std::string_view{text.data(), text.size() - 1});
  });
  append(out, ",\"data\":[");
  char number[32];
  const auto emit = [&](std::size_t i, int n) {
    if (i > 0)
    {
      out.push_back(',');
    }
    append(out, std::string_view{number, static_cast<std::size_t>(n)});
  };
  std::visit(
    overloaded{
      [&](const TileGrid& g) {
        for (std::size_t i = 0; i < g.values.size(); ++i)
        {
          emit(i, std::snprintf(number, sizeof(number), "%" PRId64, encode(g.values[i])));
        }
      },
      [&](const IdGrid& g) {
        for (std::size_t i = 0; i < g.values.size(); ++i)
        {
          emit(i, std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(g.values[i])));
        }
      },
      [&](const ScalarGrid& g) {
        for (std::size_t i = 0; i < g.values.size(); ++i)
        {
          emit(i, json::shortest(g.values[i], number, sizeof(number)));
        }
      },
      [&](const MaskGrid& g) {
        for (std::size_t i = 0; i < g.values.size(); ++i)
        {
          emit(i, std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(g.values[i])));
        }
      },
    },
    layer.data);
  append(out, "]}");
}

/**
 * @brief Finds top-level member <code>key</code> of a JSON object's text, which must be an array,
 *        and its span; <code>elements</code>, when given, collects the text span of each element
 *
 * A minimal scanner (strings with escapes, nesting depth); the full parse later validates the text.
 */
bool find_array_member(
  std::string_view text,
  std::string_view key,
  std::size_t& array_begin,
  std::size_t& array_end,
  memory::Vector<std::string_view>* elements = nullptr)
{
  int depth = 0;
  std::size_t i = 0;
  const std::size_t n = text.size();
  const auto skip_string = [&]() {
    // at opening quote
    for (++i; i < n; ++i)
    {
      if (text[i] == '\\')
      {
        ++i;
      }
      else if (text[i] == '"')
      {
        return true;
      }
    }
    return false;
  };
  for (; i < n; ++i)
  {
    const char c = text[i];
    if (c == '"')
    {
      const std::size_t start = i;
      if (!skip_string())
      {
        return false;
      }
      if (depth != 1 or text.substr(start + 1, i - start - 1) != key)
      {
        continue;
      }
      // Key at top level: expect ':' then '['
      std::size_t k = i + 1;
      while (k < n and (text[k] == ' ' or text[k] == '\n' or text[k] == '\r' or text[k] == '\t' or text[k] == ':'))
      {
        ++k;
      }
      if (k >= n or text[k] != '[')
      {
        return false;
      }
      array_begin = k;
      // Scan elements at depth 1 within the array
      int d = 0;
      std::size_t element_start = k + 1;
      for (i = k; i < n; ++i)
      {
        const char e = text[i];
        if (e == '"')
        {
          if (!skip_string())
          {
            return false;
          }
        }
        else if (e == '[' or e == '{')
        {
          ++d;
        }
        else if (e == ']' or e == '}')
        {
          if (--d == 0)
          {
            const auto element = text.substr(element_start, i - element_start);
            if (elements != nullptr and element.find_first_not_of(" \n\r\t") != std::string_view::npos)
            {
              elements->push_back(element);
            }
            array_end = i + 1;
            return true;
          }
        }
        else if (e == ',' and d == 1)
        {
          if (elements != nullptr)
          {
            elements->push_back(text.substr(element_start, i - element_start));
          }
          element_start = i + 1;
        }
      }
      return false;
    }
    if (c == '{' or c == '[')
    {
      ++depth;
    }
    else if (c == '}' or c == ']')
    {
      --depth;
    }
  }
  return false;
}

/// Characters a JSON number is made of
bool is_number_char(char c)
{
  return (c >= '0' and c <= '9') or c == '-' or c == '+' or c == '.' or c == 'e' or c == 'E';
}

bool is_space(char c) { return c == ' ' or c == '\n' or c == '\r' or c == '\t'; }

/// Elements of a JSON array's text (brackets included), counted without parsing them
std::size_t count_elements(std::string_view array)
{
  const std::string_view inner = array.substr(1, array.size() - 2);
  if (inner.find_first_not_of(" \n\r\t") == std::string_view::npos)
  {
    return 0;
  }
  return static_cast<std::size_t>(std::count(inner.begin(), inner.end(), ',')) + 1;
}

/**
 * @brief Copies the next number of <code>array</code> (brackets included) from <code>i</code> into
 *        <code>buffer</code>, NUL-terminated so strtof/strtoll can read it
 *
 * The data arrays hold millions of cells, so they are scanned straight from the text: a DOM of one
 * would cost several times what the cells themselves do (see encode_layer, which writes them the
 * same way).
 */
bool next_number(std::string_view array, std::size_t& i, char (&buffer)[40])
{
  while (i < array.size() and (array[i] == ',' or is_space(array[i])))
  {
    ++i;
  }
  const std::size_t start = i;
  while (i < array.size() and is_number_char(array[i]))
  {
    ++i;
  }
  const std::size_t size = i - start;
  if (size == 0 or size >= sizeof(buffer))
  {
    return false;
  }
  std::memcpy(buffer, array.data() + start, size);
  buffer[size] = '\0';
  return true;
}

/// Reads <code>buffer</code> as an integer, rejecting trailing characters
bool read_integer(const char* buffer, std::int64_t& value)
{
  char* end = nullptr;
  value = std::strtoll(buffer, &end, 10);
  return end != buffer and *end == '\0';
}

/// Reads <code>buffer</code> as a float, rejecting trailing characters
bool read_float(const char* buffer, float& value)
{
  char* end = nullptr;
  value = std::strtof(buffer, &end);
  return end != buffer and *end == '\0';
}

/// Reads an integer of <code>buffer</code> that must be in [0, max]
template <typename T> bool read_bounded(const char* buffer, T& value)
{
  std::int64_t v = 0;
  if (!read_integer(buffer, v) or v < 0 or v > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
  {
    return false;
  }
  value = static_cast<T>(v);
  return true;
}

/**
 * @brief Reads a layer's cells from its "data" array's text straight into the layer's plane
 *
 * Safe to run on a worker: the plane is already allocated (that happens on the calling thread, which
 * owns the document arena) and no two layers share one.
 */
Status decode_cells(std::string_view array, Layer& layer)
{
  const char* const name = layer.name.c_str();
  const std::size_t n = extents(layer).count();
  if (const std::size_t count = count_elements(array); count != n)
  {
    return Status::error("layer '%s': 'data' has %zu cells, expected %zu", name, count, n);
  }
  void* const values = cells(layer);
  const std::size_t plane = plane_index(layer.kind);
  char buffer[40];
  std::size_t i = 1;
  for (std::size_t k = 0; k < n; ++k)
  {
    if (!next_number(array, i, buffer))
    {
      return Status::error("layer '%s': cell %zu is not a number", name, k);
    }
    bool ok = false;
    switch (plane)
    {
    case 0: {
      std::int64_t v = 0;
      ok = read_integer(buffer, v) and decode(v, static_cast<TileRef*>(values)[k]);
      break;
    }
    case 1:
      ok = read_bounded(buffer, static_cast<std::uint16_t*>(values)[k]);
      break;
    case 2:
      ok = read_float(buffer, static_cast<float*>(values)[k]);
      break;
    default:
      ok = read_bounded(buffer, static_cast<std::uint8_t*>(values)[k]);
      break;
    }
    if (!ok)
    {
      return Status::error("layer '%s': invalid value '%s' at cell %zu", name, buffer, k);
    }
  }
  return Status::okay();
}

/**
 * @brief Reads one layer object's header, and finds the text of its "data" array
 *
 * Only the header goes through the DOM, parsed from the object with "data": [] in place of the
 * cells: a DOM of the cells would cost several times what the cells themselves do.
 */
Status decode_layer_header(std::string_view text, IoWorker& worker, LayerHeader& header, std::string_view& array)
{
  std::size_t array_begin = 0;
  std::size_t array_end = 0;
  if (!find_array_member(text, "data", array_begin, array_end))
  {
    return Status::error("layer: missing 'data' array");
  }
  array = text.substr(array_begin, array_end - array_begin);
  memory::Scope scope{worker.scratch};
  CharVector head = memory::make_vector<char>(worker.scratch);
  head.reserve(text.size() - array.size() + 2);
  append(head, text.substr(0, array_begin));
  append(head, "[]");
  append(head, text.substr(array_end));
  return with_json(worker, [&](IoWorker&) -> Status {
    json::Scope json_scope;
    Json j;
    LVL_RETURN_IF_ERROR(json::parse(std::string_view{head.data(), head.size()}, j));
    json::Reader r;
    if (!r.convert(j, header, "layer"))
    {
      return r.status;
    }
    return Status::okay();
  });
}

/// Reads and checks the "format"/"version" members
// -------------------------------------------------------------------------------------------------
// Scatter profiles (objects named by tileset and group; the ground type by name)
// -------------------------------------------------------------------------------------------------

void save_scatter_profile(Json& j, const project::Project& project, const project::ScatterProfile& profile)
{
  j = Json::object();
  json::write(j, "name", profile.name);
  const project::ScatterSettings& st = profile.settings;
  json::write(j, "count", st.count);
  json::write(j, "spacing", st.spacing);
  json::write(j, "attempts", st.attempts);
  // Every optional test is omitted when it is off, so a profile that uses none is a few lines
  if (st.use_band)
  {
    json::write(j, "low", st.lo);
    json::write(j, "high", st.hi);
  }
  if (st.use_semantic and st.semantic < project.semantics.size())
  {
    json::write(j, "semantic", project.semantics[st.semantic].name);
  }
  if (st.use_collision)
  {
    save_collision(j["blocked"], st.blocked);
  }
  json::write(j, "only_empty", st.only_empty);
  json::write(j, "region", st.region);
  Json objects = Json::array();
  const project::ScatterObject* entries = project::objects_of(project, profile);
  for (std::uint32_t i = 0; entries != nullptr and i < profile.count; ++i)
  {
    Json object = Json::object();
    json::write(object, "tileset", entries[i].tileset);
    json::write(object, "group", entries[i].group);
    json::write(object, "weight", entries[i].weight);
    objects.push_back(std::move(object));
  }
  j["objects"] = std::move(objects);
}

/// Reads one profile into <code>index</code>, whose objects the caller has already cleared
bool load_scatter_profile(json::Reader& r, const Json& j, project::Project& project, std::size_t index)
{
  if (!r.object(j, "scatter"))
  {
    return false;
  }
  project::ScatterSettings& st = project.scatter[index].settings;
  r.get_optional(j, "count", st.count);
  r.get_optional(j, "spacing", st.spacing);
  r.get_optional(j, "attempts", st.attempts);
  st.use_band = j.contains("low") or j.contains("high");
  r.get_optional(j, "low", st.lo);
  r.get_optional(j, "high", st.hi);
  if (const auto semantic = j.find("semantic"); semantic != j.end())
  {
    Name entry;
    if (!r.convert(*semantic, entry, "semantic"))
    {
      return false;
    }
    st.use_semantic = true;
    st.semantic = project::intern_semantic(project, entry.view());
  }
  st.use_collision = j.contains("blocked");
  if (st.use_collision and !load_collision(r, j["blocked"], st.blocked))
  {
    return false;
  }
  r.get_optional(j, "only_empty", st.only_empty);
  r.get_optional(j, "region", st.region);
  if (!r.ok())
  {
    return false;
  }
  const auto objects = j.find("objects");
  if (objects == j.end())
  {
    return r.ok();
  }
  if (!r.array(*objects, "objects"))
  {
    return false;
  }
  for (const auto& entry : *objects)
  {
    project::ScatterObject object;
    r.get(entry, "tileset", object.tileset);
    r.get(entry, "group", object.group);
    r.get_optional(entry, "weight", object.weight);
    if (!r.ok())
    {
      return false;
    }
    // The tileset and group are names, not indices, so an object naming one this project does not
    // have is kept as written and shows as dangling -- a profile outlives what it was written against
    if (!project::add_scatter_object(project, index, object))
    {
      return r.fail("the scatter profiles name more than %zu objects between them", project::kMaxScatterObjects);
    }
  }
  return r.ok();
}

bool check_format(json::Reader& r, const Json& j, std::string_view expected)
{
  Name format;
  int version = 0;
  r.get(j, "format", format);
  r.get(j, "version", version);
  if (!r.ok())
  {
    return false;
  }
  if (format != expected)
  {
    return r.fail(
      "file holds '%s', expected '%.*s'", format.c_str(), static_cast<int>(expected.size()), expected.data());
  }
  if (version > kVersion)
  {
    return r.fail("file written by a newer editor (version %d)", version);
  }
  return true;
}

void write_format(Json& j, std::string_view format)
{
  json::write(j, "format", Name{format});
  json::write(j, "version", kVersion);
}

/// Writes a whole JSON document built by <code>build(Json&)</code> using worker 0
template <typename BuildFn> Status write_document(const char* path, IoContext& ctx, BuildFn&& build)
{
  return with_json(ctx.workers[0], [&](IoWorker&) { return json::write_file(path, build, 2); });
}

/// Reads a whole JSON document with <code>read(json::Reader&, const Json&)</code> using worker 0
template <typename ReadFn> Status read_document(const char* path, IoContext& ctx, ReadFn&& read)
{
  return with_json(ctx.workers[0], [&](IoWorker&) {
    return json::read_file(path, [&](const Json& j) {
      json::Reader r;
      read(r, j);
      return r.status;
    });
  });
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// Level
// -------------------------------------------------------------------------------------------------

Status
save_level_json(const char* path, const project::Project& project, const Level& level, IoContext& ctx, LevelScope scope)
{
  const std::size_t layer_count = level.layers.size();
  progress_reset(ctx, layer_count * 2 + 2);
  progress_stage(ctx, "Writing document");

  IoWorker& main = ctx.workers[0];
  memory::Arena& shared = shared_arena(ctx);
  memory::Scope shared_scope{shared};
  ObjectRecords records{shared};
  collect(level.registry, records);

  file::Writer writer;
  LVL_RETURN_IF_ERROR(writer.open(path));
  LVL_RETURN_IF_ERROR(with_json(main, [&](IoWorker&) -> Status {
    json::Scope json_scope;
    Json j = Json::object();
    write_format(j, kLevelFormat);
    json::write(j, "settings", level.settings);
    const project::RuleSet& rules = project.rules[project::rules_of(project, level)];
    if (scope == LevelScope::kStandalone)
    {
      save_list(j, "semantics", project.semantics, 1);
      save_list(j, "materials", project.materials, 1);
      save_list(j, "seasons", project.seasons);
      Json tilesets = Json::array();
      for (const auto& ts : project.tilesets)
      {
        Json e;
        save_tileset(e, project, ts);
        tilesets.push_back(std::move(e));
      }
      j["tilesets"] = std::move(tilesets);
      // The one set this level uses, which names itself, so the file need not say which it is
      save_wfc(j["wfc"], project, rules.wfc);
      save_auto_tiles(j["auto_tiles"], project.tilesets, rules.auto_tiles);
      save_steps(j["steps"], project.tilesets, rules.steps);
    }
    else
    {
      // Nothing here names the set, so the level has to
      json::write(j, "rules", rules.wfc.name);
    }
    save_paths(j["paths"], level, records);
    save_objects(j, records);
    const JsonString text = json::dump(j, 2);
    // Everything but the closing brace, then the layers
    writer.write(std::string_view{text.data(), text.rfind('}')});
    return Status::okay();
  }));
  progress_advance(ctx);

  // Layers, a wave at a time: one sub-arena per layer of the wave, carved here (so bump allocation
  // stays on this thread), filled in parallel, then written in order and reclaimed. Peak use is one
  // wave's text, not the whole level's, and a layer too big to buffer at all is streamed instead --
  // so no level is too big to save.
  progress_stage(ctx, "Encoding layers");
  writer.write(",\n  \"layers\": [\n");
  memory::Arena buffers[kMaxIoWorkers];
  std::string_view texts[kMaxIoWorkers];
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, kMaxIoWorkers);
  for (std::size_t first = 0; first < layer_count;)
  {
    memory::Scope wave_scope{shared};
    // As many of the next layers as fit side by side
    std::size_t count = 0;
    while (count < workers and first + count < layer_count)
    {
      const std::size_t bound = encoded_bound(project.tilesets, level.layers[first + count]);
      if (bound + 2UL * memory::kSubArenaAlignment > memory::available(shared))
      {
        break;
      }
      memory::push(shared, buffers[count], bound, "layer-text");
      ++count;
    }
    if (count == 0)
    {
      // This one layer's text would not fit on its own: encode it straight to the file
      StreamSink sink{writer};
      writer.write("    ");
      encode_layer(level.layers[first], main, sink);
      sink.flush();
      progress_advance(ctx);
      writer.write((first + 1 < layer_count) ? ",\n" : "\n");
      progress_advance(ctx);
      ++first;
      continue;
    }
    jobs::parallel_for(count, count, [&](std::size_t k, std::size_t w) {
      CharVector out = memory::make_vector<char>(buffers[k]);
      // The sub-arena is this text's alone: take all of it, so the buffer never has to grow
      out.reserve(buffers[k].capacity);
      encode_layer(level.layers[first + k], ctx.workers[w], out);
      texts[k] = std::string_view{out.data(), out.size()};
      // Keep the text: detach from the vector without letting it release the block
      new (&out) CharVector{memory::ArenaAllocator<char>{}};
      progress_advance(ctx);
    });
    for (std::size_t k = 0; k < count; ++k)
    {
      writer.write("    ");
      writer.write(texts[k]);
      writer.write((first + k + 1 < layer_count) ? ",\n" : "\n");
      progress_advance(ctx);
    }
    for (std::size_t k = 0; k < count; ++k)
    {
      memory::reset(buffers[k]);
    }
    first += count;
  }
  writer.write("  ]\n}\n");
  const Status status = writer.commit();
  progress_advance(ctx);
  return status;
}

Status load_level_json(const char* path, project::Project& project, Level& level, IoContext& ctx)
{
  IoWorker& main = ctx.workers[0];
  memory::Arena& shared = shared_arena(ctx);
  memory::Scope shared_scope{shared};
  // read + document + build, plus one per layer once their number is known
  progress_reset(ctx, 3);
  progress_stage(ctx, "Reading");

  char* text = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, shared, text, size));
  const std::string_view document{text, size};
  progress_advance(ctx);

  // Split out the layers
  auto layer_texts = memory::make_vector<std::string_view>(shared);
  std::size_t array_begin = 0;
  std::size_t array_end = 0;
  const bool has_layers = find_array_member(document, "layers", array_begin, array_end, &layer_texts);
  CharVector head = memory::make_vector<char>(shared);
  if (has_layers)
  {
    // Everything but the layers, which are the bulk of the document
    head.reserve(array_begin + 2 + (size - array_end));
    append(head, document.substr(0, array_begin));
    append(head, "[]");
    append(head, document.substr(array_end));
  }
  const std::string_view head_text = has_layers ? std::string_view{head.data(), head.size()} : document;

  progress_stage(ctx, "Parsing document");
  ObjectRecords records{shared};
  const Status status = with_json(main, [&](IoWorker&) -> Status {
    json::Scope scope;
    Json j;
    if (Status s = json::parse(head_text, j); !s.ok)
    {
      return Status::error("%s: %s", path, s.message.c_str());
    }
    json::Reader r;
    check_format(r, j, kLevelFormat);
    r.get(j, "settings", level.settings);
    load_catalog(r, j, "semantics", project.semantics, [&project](std::string_view n) {
      return project::intern_semantic(project, n);
    });
    load_catalog(r, j, "materials", project.materials, [&project](std::string_view n) {
      return project::intern_material(project, n);
    });
    if (const auto seasons = j.find("seasons"); r.ok() and seasons != j.end() and r.array(*seasons, "seasons"))
    {
      project.seasons.clear();
      for (const auto& e : *seasons)
      {
        Season season;
        r.convert(e, season, "seasons");
        project.seasons.push_back(season);
      }
      if (project.seasons.empty())
      {
        project.seasons.push_back(Season{.name = "default"});
      }
    }
    if (const auto tilesets = j.find("tilesets"); r.ok() and tilesets != j.end() and r.array(*tilesets, "tilesets"))
    {
      // A level file that carries tilesets carries all of them, so it replaces the project's rather
      // than adding to them -- as the seasons and the rule sets already do, and as binary does
      project.tilesets.clear();
      for (const auto& e : *tilesets)
      {
        const TileSetId id = project::add_tileset(project, "");
        if (!load_tileset(r, e, project, project.tilesets[id]))
        {
          break;
        }
      }
    }
    // A file that carries its rules carries exactly one set, the one its cells were tiled with, so
    // it replaces the project's sets as the tilesets and seasons above do. One that does not names
    // the set it means, which must already be there.
    if (const auto wfc = j.find("wfc"); r.ok() and wfc != j.end())
    {
      project.rules.clear();
      project::add_rules(project, "");
      project::RuleSet& rules = project.rules[0];
      load_wfc(r, *wfc, project, rules.wfc);
      if (const auto terrains = j.find("auto_tiles"); r.ok() and terrains != j.end())
      {
        load_auto_tiles(r, *terrains, project.tilesets, *project.arena, rules.auto_tiles);
      }
      if (const auto steps = j.find("steps"); r.ok() and steps != j.end())
      {
        load_steps(r, *steps, project.tilesets, rules.steps);
      }
      // The set names itself, so the level takes its name from what it carried
      level.rules = rules.wfc.name;
    }
    else
    {
      r.get_optional(j, "rules", level.rules);
    }
    if (const auto paths = j.find("paths"); r.ok() and paths != j.end())
    {
      load_paths(r, *paths, level, records);
    }
    if (r.ok())
    {
      load_objects(r, j, records);
    }
    if (!r.ok())
    {
      return Status::error("%s: %s", path, r.status.message.c_str());
    }
    return Status::okay();
  });
  LVL_RETURN_IF_ERROR(status);
  // A file written before projects names its destination level by path; a portal points at a member
  migrate_portal_targets(project, path, records);
  if (Status s = restore(records, level.registry, level.graphs.size()); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  progress_advance(ctx);

  // Layer headers on this thread, so that the planes are allocated from the document arena (which
  // is single-threaded) before the workers fill them
  progress_stage(ctx, "Parsing layers");
  const std::size_t layer_count = layer_texts.size();
  if (ctx.progress != nullptr)
  {
    ctx.progress->add_total(layer_count);
  }
  auto arrays = memory::make_vector<std::string_view>(shared, layer_count, std::string_view{});
  const std::size_t base = level.layers.size();
  for (std::size_t i = 0; i < layer_count; ++i)
  {
    LayerHeader header;
    if (Status s = decode_layer_header(layer_texts[i], main, header, arrays[i]); !s.ok)
    {
      return Status::error("%s: %s", path, s.message.c_str());
    }
    const std::size_t index = add_layer(level, header.kind, header.name.view());
    apply_header(level.layers[index], header);
  }

  // Then their cells, in parallel, straight into those planes: no staging copy, so nothing here is
  // sized by the level but the file's own text
  auto results = memory::make_vector<Status>(shared, layer_count, Status::okay());
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, kMaxIoWorkers);
  jobs::parallel_for(layer_count, workers, [&](std::size_t i, std::size_t) {
    results[i] = decode_cells(arrays[i], level.layers[base + i]);
    progress_advance(ctx);
  });
  for (const auto& result : results)
  {
    if (!result.ok)
    {
      return Status::error("%s: %s", path, result.message.c_str());
    }
  }
  progress_advance(ctx);
  return Status::okay();
}

// -------------------------------------------------------------------------------------------------
// Standalone files
// -------------------------------------------------------------------------------------------------

Status save_tileset_json(const char* path, const project::Project& project, TileSetId id, IoContext& ctx)
{
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kTileSetFormat);
    // The catalog entries the tileset references, so another level can intern them
    save_list(j, "semantics", project.semantics, 1);
    save_list(j, "materials", project.materials, 1);
    save_tileset(j["tileset"], project, project.tilesets[id]);
  });
}

Status load_tileset_json(const char* path, project::Project& project, IoContext& ctx, TileSetId& id)
{
  id = kNoTileSet;
  return read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kTileSetFormat);
    load_catalog(r, j, "semantics", project.semantics, [&project](std::string_view n) {
      return project::intern_semantic(project, n);
    });
    load_catalog(r, j, "materials", project.materials, [&project](std::string_view n) {
      return project::intern_material(project, n);
    });
    const auto ts = j.find("tileset");
    if (!r.ok() or ts == j.end())
    {
      r.fail("missing 'tileset'");
      return;
    }
    id = project::add_tileset(project, "");
    if (!load_tileset(r, *ts, project, project.tilesets[id]))
    {
      project.tilesets.pop_back();
      id = kNoTileSet;
    }
  });
}

Status save_wfc_json(const char* path, const project::Project& project, std::size_t index, IoContext& ctx)
{
  const project::RuleSet& rules = project.rules[index];
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kWfcFormat);
    save_list(j, "semantics", project.semantics, 1);
    save_wfc(j["wfc"], project, rules.wfc);
    save_auto_tiles(j["auto_tiles"], project.tilesets, rules.auto_tiles);
    save_steps(j["steps"], project.tilesets, rules.steps);
  });
}

Status load_wfc_json(const char* path, project::Project& project, IoContext& ctx, std::size_t& index)
{
  index = project::kNoRules;
  return read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kWfcFormat);
    load_catalog(r, j, "semantics", project.semantics, [&project](std::string_view n) {
      return project::intern_semantic(project, n);
    });
    const auto wfc = j.find("wfc");
    if (!r.ok() or wfc == j.end())
    {
      r.fail("missing 'wfc'");
      return;
    }
    // The file carries one set, named; it replaces the project's set of that name or becomes a new
    // one, so importing the same file twice does not leave two
    Name name;
    r.get_optional(*wfc, "name", name);
    index = project::intern_rules(project, name.view());
    project::RuleSet& rules = project.rules[index];
    load_wfc(r, *wfc, project, rules.wfc);
    // Auto-tiling terrains travel in the same file; older ones have none
    rules.auto_tiles.terrains.clear();
    if (const auto terrains = j.find("auto_tiles"); r.ok() and terrains != j.end())
    {
      load_auto_tiles(r, *terrains, project.tilesets, *project.arena, rules.auto_tiles);
    }
    rules.steps.terrains.clear();
    if (const auto steps = j.find("steps"); r.ok() and steps != j.end())
    {
      load_steps(r, *steps, project.tilesets, rules.steps);
    }
  });
}

Status save_scatter_json(const char* path, const project::Project& project, std::size_t index, IoContext& ctx)
{
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kScatterFormat);
    // The catalog entry its ground type names, so another project can intern it
    save_list(j, "semantics", project.semantics, 1);
    save_scatter_profile(j["scatter"], project, project.scatter[index]);
  });
}

Status load_scatter_json(const char* path, project::Project& project, IoContext& ctx, std::size_t& index)
{
  index = project::kNoScatterProfile;
  return read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kScatterFormat);
    load_catalog(r, j, "semantics", project.semantics, [&project](std::string_view n) {
      return project::intern_semantic(project, n);
    });
    const auto scatter = j.find("scatter");
    if (!r.ok() or scatter == j.end())
    {
      r.fail("missing 'scatter'");
      return;
    }
    // The file names the profile it holds, so it replaces the project's profile of that name or
    // becomes a new one: importing the same file twice does not leave two
    Name name;
    r.get_optional(*scatter, "name", name);
    index = project::intern_scatter(project, name.view());
    if (index == project::kNoScatterProfile)
    {
      r.fail("the project already holds %zu scatter profiles", project::kMaxScatterProfiles);
      return;
    }
    load_scatter_profile(r, *scatter, project, index);
  });
}

Status save_paths_json(const char* path, const Level& level, IoContext& ctx)
{
  memory::Scope scope{ctx.workers[0].scratch};
  ObjectRecords records{ctx.workers[0].scratch};
  collect(level.registry, records, true);
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kPathsFormat);
    save_paths(j["paths"], level, records);
  });
}

Status load_paths_json(const char* path, Level& level, IoContext& ctx)
{
  memory::Scope scope{ctx.workers[0].scratch};
  ObjectRecords records{ctx.workers[0].scratch};
  // Parse into a scratch level-less list first, so a bad file leaves the level untouched
  auto graphs = memory::make_vector<PathGraph>(ctx.workers[0].scratch);
  Status status = read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kPathsFormat);
    const auto paths = j.find("paths");
    if (!r.ok() or paths == j.end())
    {
      r.fail("missing 'paths'");
      return;
    }
    // load_paths appends graphs to a level: use a throwaway graph list via a temporary swap
    auto saved = std::move(level.graphs);
    level.graphs = memory::make_vector<PathGraph>(*level.arena);
    load_paths(r, *paths, level, records);
    graphs.assign(level.graphs.begin(), level.graphs.end());
    level.graphs = std::move(saved);
  });
  LVL_RETURN_IF_ERROR(status);
  while (!level.graphs.empty())
  {
    remove_graph(level, static_cast<GraphId>(level.graphs.size() - 1));
  }
  level.graphs.assign(graphs.begin(), graphs.end());
  return restore(records, level.registry, level.graphs.size(), true);
}

}  // namespace lvl::level::io
