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
 *   "tilesets": [...], "wfc": {...}, "paths": [...], "portals": [...], "markers": [...],
 *   "layers": [ {"name": ..., "kind": ..., ..., "data": [...]}, ... ]
 * }
 * @endcode
 *
 * "layers" is always written last, one layer per line. On load, a scanner finds the text of each
 * layer object; the rest of the document is parsed with "layers" replaced by [] on this thread,
 * while the layers are parsed in parallel by IoWorkers.
 */

// C++ Standard Library
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

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

void save_tileset(Json& j, const Level& level, const TileSet& ts)
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
      info.collision == collision::kNone and !info.hidden)
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
      json::write(t, "semantic", level.semantics[info.semantic].name);
    }
    if (info.material != kNoMaterial)
    {
      json::write(t, "material", level.materials[info.material].name);
    }
    if (info.collision != collision::kNone)
    {
      save_collision(t["collision"], info.collision);
    }
    if (info.hidden)
    {
      json::write(t, "hidden", true);
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
      json::write(e, "material", level.materials[v.material].name);
    }
    seasonal.push_back(std::move(e));
  }
  j["seasonal"] = std::move(seasonal);
}

bool load_tileset(json::Reader& r, const Json& j, Level& level, TileSet& ts)
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
  const auto semantic = [&level](std::string_view name) { return intern_semantic(level, name); };
  const auto material = [&level](std::string_view name) { return intern_material(level, name); };

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
  return r.ok();
}

// -------------------------------------------------------------------------------------------------
// WFC rules (tiles referenced by tileset name)
// -------------------------------------------------------------------------------------------------

void save_wfc(Json& j, const Level& level)
{
  const WfcRules& rules = level.wfc;
  j = Json::object();
  json::write(j, "name", rules.name);
  json::write(j, "diagonals", rules.diagonals);
  Json tiles = Json::array();
  for (const auto& t : rules.tiles)
  {
    Json e = Json::object();
    json::write(e, "tileset", level.tilesets[t.tile.tileset].name);
    json::write(e, "tile", t.tile.tile);
    json::write(e, "weight", t.weight);
    json::write(e, "height", std::array<float, 2>{t.height_min, t.height_max});
    json::write(e, "water", t.water);
    if (t.semantic != kNoSemantic)
    {
      json::write(e, "semantic", level.semantics[t.semantic].name);
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

bool load_wfc(json::Reader& r, const Json& j, Level& level)
{
  WfcRules& rules = level.wfc;
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
      level.tilesets.begin(), level.tilesets.end(), [&tileset](const TileSet& s) { return s.name == tileset; });
    if (ts == level.tilesets.end())
    {
      return r.fail("WFC tile refers to unknown tileset '%s'", tileset.c_str());
    }
    t.tile.tileset = static_cast<TileSetId>(ts - level.tilesets.begin());
    t.height_min = band[0];
    t.height_max = band[1];
    t.semantic = semantic.empty() ? kNoSemantic : intern_semantic(level, semantic.view());
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
    json::write(e, "target_level", p.portal.target_level);
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
      r.get_optional(e, "target_level", p.portal.target_level);
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

/// Encodes one layer as a single-line JSON object into <code>out</code> (in the worker's scratch)
void encode_layer(const Layer& layer, IoWorker& worker, CharVector& out)
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

/// Parses one layer object's text into <code>out</code> (values in the worker's scratch arena)
void decode_layer(std::string_view text, Extents extents, IoWorker& worker, DecodedLayer& out)
{
  out.extents = extents;
  out.status = with_json(worker, [&](IoWorker&) -> Status {
    json::Scope scope;
    Json j;
    LVL_RETURN_IF_ERROR(json::parse(text, j));
    json::Reader r;
    if (!r.convert(j, out.header, "layer"))
    {
      return r.status;
    }
    const auto data = j.find("data");
    if (data == j.end() or !data->is_array())
    {
      return Status::error("layer '%s': missing 'data' array", out.header.name.c_str());
    }
    if (data->size() != extents.count())
    {
      return Status::error(
        "layer '%s': 'data' has %zu cells, expected %zu", out.header.name.c_str(), data->size(), extents.count());
    }
    const std::size_t n = extents.count();
    out.values = memory::allocate_bytes(worker.scratch, n * cell_size(out.header.kind), alignof(float));
    switch (plane_index(out.header.kind))
    {
    case 0: {
      auto* values = static_cast<TileRef*>(out.values);
      for (std::size_t i = 0; i < n; ++i)
      {
        std::int64_t v = 0;
        if (!r.convert((*data)[i], v, "data") or !decode(v, values[i]))
        {
          return Status::error("layer '%s': invalid tile at cell %zu", out.header.name.c_str(), i);
        }
      }
      break;
    }
    case 1: {
      auto* values = static_cast<std::uint16_t*>(out.values);
      for (std::size_t i = 0; i < n and r.convert((*data)[i], values[i], "data"); ++i)
      {
      }
      break;
    }
    case 2: {
      auto* values = static_cast<float*>(out.values);
      for (std::size_t i = 0; i < n and r.convert((*data)[i], values[i], "data"); ++i)
      {
      }
      break;
    }
    default: {
      auto* values = static_cast<std::uint8_t*>(out.values);
      for (std::size_t i = 0; i < n and r.convert((*data)[i], values[i], "data"); ++i)
      {
      }
      break;
    }
    }
    if (!r.ok())
    {
      return Status::error("layer '%s': %s", out.header.name.c_str(), r.status.message.c_str());
    }
    return Status::okay();
  });
}

/**
 * @brief Finds top-level member <code>key</code> of a JSON object's text, which must be an array,
 *        and the text span of each of its elements
 *
 * A minimal scanner (strings with escapes, nesting depth); the full parse later validates the text.
 */
bool find_array_member(
  std::string_view text,
  std::string_view key,
  std::size_t& array_begin,
  std::size_t& array_end,
  memory::Vector<std::string_view>& elements)
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
            if (element.find_first_not_of(" \n\r\t") != std::string_view::npos)
            {
              elements.push_back(element);
            }
            array_end = i + 1;
            return true;
          }
        }
        else if (e == ',' and d == 1)
        {
          elements.push_back(text.substr(element_start, i - element_start));
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

/// Reads and checks the "format"/"version" members
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

Status save_level_json(const char* path, const Level& level, IoContext& ctx)
{
  const std::size_t layer_count = level.layers.size();
  progress_reset(ctx, layer_count * 2 + 2);
  progress_stage(ctx, "Encoding layers");

  // Layers, in parallel, each into its worker's scratch arena
  IoWorker& main = ctx.workers[0];
  memory::Scope main_scope{main.scratch};
  auto texts = memory::make_vector<std::string_view>(main.scratch, layer_count, std::string_view{});
  // Scratch marks for every worker are restored when we're done
  memory::Arena* scratches[64];
  std::size_t marks[64];
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, 64);
  for (std::size_t w = 0; w < workers; ++w)
  {
    scratches[w] = &ctx.workers[w].scratch;
    marks[w] = ctx.workers[w].scratch.utilized;
  }
  jobs::parallel_for(layer_count, workers, [&](std::size_t i, std::size_t w) {
    IoWorker& worker = ctx.workers[w];
    const Layer& layer = level.layers[i];
    CharVector out = memory::make_vector<char>(worker.scratch);
    // Reserve up front so the buffer never moves (stays the newest block, no wasted copies)
    out.reserve(256 + layer.name.size() + extents(layer).count() * (is_scalar(layer.kind) ? 16 : 8));
    encode_layer(layer, worker, out);
    texts[i] = std::string_view{out.data(), out.size()};
    // Keep the text: detach from the vector without letting it release the block
    new (&out) CharVector{memory::ArenaAllocator<char>{}};
    progress_advance(ctx);
  });

  progress_stage(ctx, "Writing");
  ObjectRecords records{main.scratch};
  collect(level.registry, records);

  file::Writer writer;
  Status status = writer.open(path);
  if (status.ok)
  {
    status = with_json(main, [&](IoWorker&) -> Status {
      json::Scope scope;
      Json j = Json::object();
      write_format(j, kLevelFormat);
      json::write(j, "settings", level.settings);
      save_list(j, "semantics", level.semantics, 1);
      save_list(j, "materials", level.materials, 1);
      save_list(j, "seasons", level.seasons);
      Json tilesets = Json::array();
      for (const auto& ts : level.tilesets)
      {
        Json e;
        save_tileset(e, level, ts);
        tilesets.push_back(std::move(e));
      }
      j["tilesets"] = std::move(tilesets);
      save_wfc(j["wfc"], level);
      save_paths(j["paths"], level, records);
      save_objects(j, records);
      const JsonString text = json::dump(j, 2);
      // Everything but the closing brace, then the layers
      writer.write(std::string_view{text.data(), text.rfind('}')});
      return Status::okay();
    });
    progress_advance(ctx);
    writer.write(",\n  \"layers\": [\n");
    for (std::size_t i = 0; i < layer_count; ++i)
    {
      writer.write("    ");
      writer.write(texts[i]);
      writer.write((i + 1 < layer_count) ? ",\n" : "\n");
      progress_advance(ctx);
    }
    writer.write("  ]\n}\n");
    status = writer.commit();
  }
  for (std::size_t w = 1; w < workers; ++w)
  {
    scratches[w]->utilized = marks[w];
    scratches[w]->last = marks[w];
  }
  progress_advance(ctx);
  return status;
}

Status load_level_json(const char* path, Level& level, IoContext& ctx)
{
  IoWorker& main = ctx.workers[0];
  memory::Scope main_scope{main.scratch};
  // read + document + build, plus one per layer once their number is known
  progress_reset(ctx, 3);
  progress_stage(ctx, "Reading");

  char* text = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, main.scratch, text, size));
  const std::string_view document{text, size};
  progress_advance(ctx);

  // Split out the layers
  auto layer_texts = memory::make_vector<std::string_view>(main.scratch);
  std::size_t array_begin = 0;
  std::size_t array_end = 0;
  const bool has_layers = find_array_member(document, "layers", array_begin, array_end, layer_texts);
  CharVector head = memory::make_vector<char>(main.scratch);
  if (has_layers)
  {
    head.reserve(size);
    append(head, document.substr(0, array_begin));
    append(head, "[]");
    append(head, document.substr(array_end));
  }
  const std::string_view head_text = has_layers ? std::string_view{head.data(), head.size()} : document;

  progress_stage(ctx, "Parsing document");
  ObjectRecords records{main.scratch};
  Status status = with_json(main, [&](IoWorker&) -> Status {
    json::Scope scope;
    Json j;
    if (Status s = json::parse(head_text, j); !s.ok)
    {
      return Status::error("%s: %s", path, s.message.c_str());
    }
    json::Reader r;
    check_format(r, j, kLevelFormat);
    r.get(j, "settings", level.settings);
    load_catalog(
      r, j, "semantics", level.semantics, [&level](std::string_view n) { return intern_semantic(level, n); });
    load_catalog(
      r, j, "materials", level.materials, [&level](std::string_view n) { return intern_material(level, n); });
    if (const auto seasons = j.find("seasons"); r.ok() and seasons != j.end() and r.array(*seasons, "seasons"))
    {
      level.seasons.clear();
      for (const auto& e : *seasons)
      {
        Season season;
        r.convert(e, season, "seasons");
        level.seasons.push_back(season);
      }
      if (level.seasons.empty())
      {
        level.seasons.push_back(Season{.name = "default"});
      }
    }
    if (const auto tilesets = j.find("tilesets"); r.ok() and tilesets != j.end() and r.array(*tilesets, "tilesets"))
    {
      for (const auto& e : *tilesets)
      {
        const TileSetId id = add_tileset(level, "");
        if (!load_tileset(r, e, level, level.tilesets[id]))
        {
          break;
        }
      }
    }
    if (const auto wfc = j.find("wfc"); r.ok() and wfc != j.end())
    {
      load_wfc(r, *wfc, level);
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
  if (Status s = restore(records, level.registry, level.graphs.size()); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  progress_advance(ctx);

  // Layers, in parallel, into worker scratch; then copied into the document on this thread
  progress_stage(ctx, "Parsing layers");
  const std::size_t layer_count = layer_texts.size();
  if (ctx.progress != nullptr)
  {
    ctx.progress->add_total(layer_count);
  }
  auto decoded = memory::make_vector<DecodedLayer>(main.scratch, layer_count, DecodedLayer{});
  std::size_t marks[64];
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, 64);
  for (std::size_t w = 0; w < workers; ++w)
  {
    marks[w] = ctx.workers[w].scratch.utilized;
  }
  const Extents extents = level.settings.extents;
  jobs::parallel_for(layer_count, workers, [&](std::size_t i, std::size_t w) {
    decode_layer(layer_texts[i], extents, ctx.workers[w], decoded[i]);
    progress_advance(ctx);
  });

  progress_stage(ctx, "Building layers");
  status = Status::okay();
  for (auto& d : decoded)
  {
    if (!d.status.ok)
    {
      status = Status::error("%s: %s", path, d.status.message.c_str());
      break;
    }
    const std::size_t index = add_layer(level, d.header.kind, d.header.name.view());
    Layer& layer = level.layers[index];
    apply_header(layer, d.header);
    std::memcpy(cells(layer), d.values, extents.count() * cell_size(d.header.kind));
  }
  for (std::size_t w = 1; w < workers; ++w)
  {
    ctx.workers[w].scratch.utilized = marks[w];
    ctx.workers[w].scratch.last = marks[w];
  }
  progress_advance(ctx);
  return status;
}

// -------------------------------------------------------------------------------------------------
// Standalone files
// -------------------------------------------------------------------------------------------------

Status save_tileset_json(const char* path, const Level& level, TileSetId id, IoContext& ctx)
{
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kTileSetFormat);
    // The catalog entries the tileset references, so another level can intern them
    save_list(j, "semantics", level.semantics, 1);
    save_list(j, "materials", level.materials, 1);
    save_tileset(j["tileset"], level, level.tilesets[id]);
  });
}

Status load_tileset_json(const char* path, Level& level, IoContext& ctx, TileSetId& id)
{
  id = kNoTileSet;
  return read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kTileSetFormat);
    load_catalog(
      r, j, "semantics", level.semantics, [&level](std::string_view n) { return intern_semantic(level, n); });
    load_catalog(
      r, j, "materials", level.materials, [&level](std::string_view n) { return intern_material(level, n); });
    const auto ts = j.find("tileset");
    if (!r.ok() or ts == j.end())
    {
      r.fail("missing 'tileset'");
      return;
    }
    id = add_tileset(level, "");
    if (!load_tileset(r, *ts, level, level.tilesets[id]))
    {
      level.tilesets.pop_back();
      id = kNoTileSet;
    }
  });
}

Status save_wfc_json(const char* path, const Level& level, IoContext& ctx)
{
  return write_document(path, ctx, [&](Json& j) {
    j = Json::object();
    write_format(j, kWfcFormat);
    save_list(j, "semantics", level.semantics, 1);
    save_wfc(j["wfc"], level);
  });
}

Status load_wfc_json(const char* path, Level& level, IoContext& ctx)
{
  return read_document(path, ctx, [&](json::Reader& r, const Json& j) {
    check_format(r, j, kWfcFormat);
    load_catalog(
      r, j, "semantics", level.semantics, [&level](std::string_view n) { return intern_semantic(level, n); });
    const auto wfc = j.find("wfc");
    if (!r.ok() or wfc == j.end())
    {
      r.fail("missing 'wfc'");
      return;
    }
    load_wfc(r, *wfc, level);
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
