/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io_binary.cpp
 *
 * Binary level format: a binary::Header, the document metadata (everything but layer cells) as one
 * Bitsery object, then one chunk per layer: {u64 byte count, u64 FNV-1a checksum, raw cells}. Cells
 * are stored in the in-memory layout (little-endian), so saving writes straight from the document
 * and loading copies straight into it; checksums are computed/verified in parallel.
 *
 * Catalog, tileset and graph references are plain indices: a binary level file is self-contained.
 */

// C++ Standard Library
#include <algorithm>
#include <cstring>
#include <type_traits>

// LVL
#include "io_common.hpp"
#include "lvl/core/binary.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/logging.hpp"

namespace lvl::level
{

template <typename S> void serialize(S& s, Settings& v)
{
  s.object(v.name);
  s.object(v.extents);
  s.value4b(v.cell_width);
  s.value4b(v.cell_height);
  s.value8b(v.seed);
}

template <typename S> void serialize(S& s, TileInfo& v)
{
  s.object(v.name);
  s.value2b(v.semantic);
  s.value2b(v.material);
  s.value1b(v.collision);
  s.value1b(v.hidden);
}

template <typename S> void serialize(S& s, TileProperty& v)
{
  s.value2b(v.tile);
  s.object(v.key);
  s.object(v.value);
}

template <typename S> void serialize(S& s, SeasonalVariant& v)
{
  s.value2b(v.tile);
  s.object(v.season);
  s.value2b(v.alternate);
  s.value2b(v.material);
}

template <typename S> void serialize(S& s, TileSet& v)
{
  s.object(v.name);
  s.object(v.image);
  s.object(v.slice);
  s.value4b(v.image_width);
  s.value4b(v.image_height);
  s.container(v.tiles, 0xFFFF);
  s.container(v.properties, 1 << 20);
  s.container(v.seasonal, 1 << 20);
}

template <typename S> void serialize(S& s, WfcTile& v)
{
  s.object(v.tile);
  s.value4b(v.weight);
  s.value4b(v.height_min);
  s.value4b(v.height_max);
  s.value1b(v.water);
  s.value2b(v.semantic);
}

template <typename S> void serialize(S& s, WfcAdjacency& v)
{
  s.value2b(v.a);
  s.value2b(v.b);
  s.value1b(v.direction);
}

template <typename S> void serialize(S& s, WfcRules& v)
{
  s.object(v.name);
  s.value1b(v.diagonals);
  s.container(v.tiles, 0xFFFF);
  s.container(v.adjacency, 1 << 24);
}

namespace io
{
namespace
{

constexpr std::size_t kMaxList = 1 << 20;

template <typename S> constexpr bool is_reader = std::is_same_v<S, binary::Reader>;

/// Tilesets aren't default-constructible (they need the arena), so the list is (de)serialized by hand
template <typename S> void serialize_tilesets(S& s, Level& level)
{
  auto count = static_cast<std::uint32_t>(level.tilesets.size());
  s.value4b(count);
  if constexpr (is_reader<S>)
  {
    if (count >= kNoTileSet)
    {
      s.adapter().error(bitsery::ReaderError::InvalidData);
      return;
    }
    level.tilesets.clear();
    for (std::uint32_t i = 0; i < count and s.adapter().error() == bitsery::ReaderError::NoError; ++i)
    {
      level.tilesets.emplace_back(*level.arena);
      s.object(level.tilesets.back());
    }
  }
  else
  {
    for (auto& ts : level.tilesets)
    {
      s.object(ts);
    }
  }
}

/// Document metadata: the level minus layer cells, with objects as records
struct Meta
{
  Level* level;
  ObjectRecords* records;
  memory::Vector<LayerHeader>* layers;
};

template <typename S> void serialize(S& s, Meta& m)
{
  Level& level = *m.level;
  s.object(level.settings);
  s.container(level.semantics, kMaxList);
  s.container(level.materials, kMaxList);
  s.container(level.seasons, 256);
  serialize_tilesets(s, level);
  s.object(level.wfc);
  s.container(level.graphs, kMaxList);
  s.container(m.records->portals, kMaxList);
  s.container(m.records->markers, kMaxList);
  s.container(m.records->nodes, kMaxList);
  s.container(m.records->edges, kMaxList);
  s.container(*m.layers, 1024);
}

std::uint64_t fnv1a(const void* data, std::size_t size)
{
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t h = 0xCBF29CE484222325ULL;
  for (std::size_t i = 0; i < size; ++i)
  {
    h = (h ^ bytes[i]) * 0x100000001B3ULL;
  }
  return h;
}

struct ChunkHeader
{
  std::uint64_t size = 0;
  std::uint64_t checksum = 0;
};

/// Reads a file into worker 0's scratch and deserializes <code>value</code> (of <code>kind</code>) from it
template <typename T> Status read_binary(const char* path, IoContext& ctx, binary::Kind kind, T& value)
{
  return binary::read_file(path, ctx.workers[0].scratch, kind, value);
}

}  // namespace

Status save_level_binary(const char* path, const Level& level_in, IoContext& ctx)
{
  // Bitsery's serializer takes non-const references; nothing is modified when writing
  Level& level = const_cast<Level&>(level_in);
  IoWorker& main = ctx.workers[0];
  memory::Scope scope{main.scratch};
  const std::size_t layer_count = level.layers.size();
  progress_reset(ctx, layer_count * 2 + 1);
  progress_stage(ctx, "Encoding");

  ObjectRecords records{main.scratch};
  collect(level.registry, records);
  auto headers = memory::make_vector<LayerHeader>(main.scratch);
  for (const auto& layer : level.layers)
  {
    headers.push_back(header_of(layer));
  }
  Meta meta{&level, &records, &headers};
  auto buffer = memory::make_vector<std::uint8_t>(main.scratch);
  const std::size_t meta_size = binary::serialize(buffer, binary::Kind::kLevel, meta);
  progress_advance(ctx);

  // Checksums in parallel
  auto chunks = memory::make_vector<ChunkHeader>(main.scratch, layer_count, ChunkHeader{});
  jobs::parallel_for(layer_count, ctx.worker_count, [&](std::size_t i, std::size_t) {
    const Layer& layer = level.layers[i];
    chunks[i].size = extents(layer).count() * cell_size(layer.kind);
    chunks[i].checksum = fnv1a(cells(layer), chunks[i].size);
    progress_advance(ctx);
  });

  progress_stage(ctx, "Writing");
  file::Writer writer;
  LVL_RETURN_IF_ERROR(writer.open(path));
  writer.write(buffer.data(), meta_size);
  for (std::size_t i = 0; i < layer_count; ++i)
  {
    writer.write(&chunks[i], sizeof(ChunkHeader));
    writer.write(cells(level.layers[i]), chunks[i].size);
    progress_advance(ctx);
  }
  return writer.commit();
}

Status load_level_binary(const char* path, Level& level, IoContext& ctx)
{
  IoWorker& main = ctx.workers[0];
  memory::Scope scope{main.scratch};
  progress_reset(ctx, 3);
  progress_stage(ctx, "Reading");

  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, main.scratch, data, size));
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  progress_advance(ctx);

  progress_stage(ctx, "Decoding document");
  ObjectRecords records{main.scratch};
  auto headers = memory::make_vector<LayerHeader>(main.scratch);
  Meta meta{&level, &records, &headers};
  binary::Reader reader{binary::InputAdapter{bytes, size}};
  binary::Header header;
  reader.object(header);
  if (
    reader.adapter().error() != bitsery::ReaderError::NoError or header.magic != binary::kMagic or
    header.kind != binary::Kind::kLevel)
  {
    return Status::error("%s: not a lvl binary level", path);
  }
  if (header.version > static_cast<std::uint32_t>(kVersion))
  {
    return Status::error("%s: written by a newer editor (version %u)", path, static_cast<unsigned>(header.version));
  }
  reader.object(meta);
  if (reader.adapter().error() != bitsery::ReaderError::NoError)
  {
    return Status::error("%s: corrupt or truncated document data", path);
  }
  const Extents extents = level.settings.extents;
  if (extents.width <= 0 or extents.height <= 0 or extents.width > 16384 or extents.height > 16384)
  {
    return Status::error("%s: invalid grid size %dx%d", path, extents.width, extents.height);
  }
  if (level.seasons.empty())
  {
    level.seasons.push_back(Season{.name = "default"});
  }
  if (Status s = restore(records, level.registry, level.graphs.size()); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }

  // Locate the layer chunks
  std::size_t offset = reader.adapter().currentReadPos();
  const std::size_t layer_count = headers.size();
  auto chunk_data = memory::make_vector<const std::uint8_t*>(main.scratch, layer_count, nullptr);
  auto chunks = memory::make_vector<ChunkHeader>(main.scratch, layer_count, ChunkHeader{});
  for (std::size_t i = 0; i < layer_count; ++i)
  {
    if (offset + sizeof(ChunkHeader) > size)
    {
      return Status::error("%s: truncated at layer %zu", path, i);
    }
    std::memcpy(&chunks[i], bytes + offset, sizeof(ChunkHeader));
    offset += sizeof(ChunkHeader);
    const std::size_t expected = extents.count() * cell_size(headers[i].kind);
    if (chunks[i].size != expected or offset + expected > size)
    {
      return Status::error("%s: layer '%s' has a bad size", path, headers[i].name.c_str());
    }
    chunk_data[i] = bytes + offset;
    offset += expected;
  }
  if (offset != size)
  {
    return Status::error("%s: trailing bytes after the last layer", path);
  }
  progress_advance(ctx);

  // Allocate layers on this thread (the document arena is single-threaded), fill them in parallel
  progress_stage(ctx, "Decoding layers");
  if (ctx.progress != nullptr)
  {
    ctx.progress->add_total(layer_count);
  }
  for (const auto& h : headers)
  {
    apply_header(level.layers[add_layer(level, h.kind, h.name.view())], h);
  }
  auto ok = memory::make_vector<std::uint8_t>(main.scratch, layer_count, std::uint8_t{0});
  jobs::parallel_for(layer_count, ctx.worker_count, [&](std::size_t i, std::size_t) {
    ok[i] = (fnv1a(chunk_data[i], chunks[i].size) == chunks[i].checksum) ? 1 : 0;
    if (ok[i] != 0)
    {
      std::memcpy(cells(level.layers[i]), chunk_data[i], chunks[i].size);
    }
    progress_advance(ctx);
  });
  for (std::size_t i = 0; i < layer_count; ++i)
  {
    if (ok[i] == 0)
    {
      return Status::error("%s: layer '%s' failed its checksum", path, headers[i].name.c_str());
    }
  }
  // Validate tile references
  for (const auto& layer : level.layers)
  {
    if (layer.kind != LayerKind::kTiles)
    {
      continue;
    }
    for (const TileRef t : plane<TileGrid>(layer).values)
    {
      if (!t.empty() and t.tileset >= level.tilesets.size())
      {
        return Status::error("%s: layer '%s' references missing tileset %u", path, layer.name.c_str(), t.tileset);
      }
    }
  }
  progress_advance(ctx);
  return Status::okay();
}

// -------------------------------------------------------------------------------------------------
// Standalone files: catalogs travel along, and ids are remapped by name on load
// -------------------------------------------------------------------------------------------------

namespace
{

struct TileSetFile
{
  memory::Vector<SemanticType>* semantics;
  memory::Vector<Material>* materials;
  TileSet* tileset;
};

template <typename S> void serialize(S& s, TileSetFile& f)
{
  s.container(*f.semantics, kMaxList);
  s.container(*f.materials, kMaxList);
  s.object(*f.tileset);
}

struct WfcFile
{
  memory::Vector<SemanticType>* semantics;
  memory::Vector<Name>* tilesets;
  WfcRules* rules;
};

template <typename S> void serialize(S& s, WfcFile& f)
{
  s.container(*f.semantics, kMaxList);
  s.container(*f.tilesets, kMaxList);
  s.object(*f.rules);
}

struct PathsFile
{
  memory::Vector<PathGraph>* graphs;
  ObjectRecords* records;
};

template <typename S> void serialize(S& s, PathsFile& f)
{
  s.container(*f.graphs, kMaxList);
  s.container(f.records->nodes, kMaxList);
  s.container(f.records->edges, kMaxList);
}

/// Maps file catalog ids to level ids by interning names (new entries copy the file's properties)
template <typename T, typename InternFn>
memory::Vector<std::uint16_t>
remap(memory::Arena& arena, const memory::Vector<T>& file, memory::Vector<T>& catalog, InternFn&& intern)
{
  auto map = memory::make_vector<std::uint16_t>(arena, file.size(), std::uint16_t{0});
  for (std::size_t i = 1; i < file.size(); ++i)
  {
    const std::size_t before = catalog.size();
    map[i] = static_cast<std::uint16_t>(intern(file[i].name.view()));
    if (catalog.size() > before)
    {
      catalog[map[i]] = file[i];
    }
  }
  return map;
}

}  // namespace

Status save_tileset_binary(const char* path, const Level& level, TileSetId id, IoContext& ctx)
{
  TileSetFile file{
    const_cast<memory::Vector<SemanticType>*>(&level.semantics),
    const_cast<memory::Vector<Material>*>(&level.materials),
    const_cast<TileSet*>(&level.tilesets[id])};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kTileSet, file);
}

Status load_tileset_binary(const char* path, Level& level, IoContext& ctx, TileSetId& id)
{
  id = kNoTileSet;
  // Decoded into worker 1's scratch if there is one (worker 0's holds the file bytes)
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto semantics = memory::make_vector<SemanticType>(temp);
  auto materials = memory::make_vector<Material>(temp);
  TileSet tileset{temp};
  TileSetFile file{&semantics, &materials, &tileset};
  LVL_RETURN_IF_ERROR(read_binary(path, ctx, binary::Kind::kTileSet, file));

  const auto semantic_map =
    remap(temp, semantics, level.semantics, [&level](std::string_view n) { return intern_semantic(level, n); });
  const auto material_map =
    remap(temp, materials, level.materials, [&level](std::string_view n) { return intern_material(level, n); });
  const auto mapped = [](const memory::Vector<std::uint16_t>& map, std::uint16_t v) -> std::uint16_t {
    return (v < map.size()) ? map[v] : 0;
  };

  id = add_tileset(level, tileset.name.view());
  TileSet& ts = level.tilesets[id];
  ts.image = tileset.image;
  ts.slice = tileset.slice;
  ts.image_width = tileset.image_width;
  ts.image_height = tileset.image_height;
  for (TileInfo info : tileset.tiles)
  {
    info.semantic = mapped(semantic_map, info.semantic);
    info.material = mapped(material_map, info.material);
    ts.tiles.push_back(info);
  }
  ts.properties.assign(tileset.properties.begin(), tileset.properties.end());
  for (SeasonalVariant v : tileset.seasonal)
  {
    v.material = mapped(material_map, v.material);
    ts.seasonal.push_back(v);
  }
  return Status::okay();
}

Status save_wfc_binary(const char* path, const Level& level, IoContext& ctx)
{
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto tilesets = memory::make_vector<Name>(temp);
  for (const auto& ts : level.tilesets)
  {
    tilesets.push_back(ts.name);
  }
  WfcFile file{
    const_cast<memory::Vector<SemanticType>*>(&level.semantics), &tilesets, const_cast<WfcRules*>(&level.wfc)};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kWfcRules, file);
}

Status load_wfc_binary(const char* path, Level& level, IoContext& ctx)
{
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto semantics = memory::make_vector<SemanticType>(temp);
  auto tilesets = memory::make_vector<Name>(temp);
  WfcRules rules{temp};
  WfcFile file{&semantics, &tilesets, &rules};
  LVL_RETURN_IF_ERROR(read_binary(path, ctx, binary::Kind::kWfcRules, file));

  auto tileset_map = memory::make_vector<TileSetId>(temp, tilesets.size(), kNoTileSet);
  for (std::size_t i = 0; i < tilesets.size(); ++i)
  {
    for (std::size_t k = 0; k < level.tilesets.size(); ++k)
    {
      if (level.tilesets[k].name == tilesets[i])
      {
        tileset_map[i] = static_cast<TileSetId>(k);
      }
    }
  }
  const auto semantic_map =
    remap(temp, semantics, level.semantics, [&level](std::string_view n) { return intern_semantic(level, n); });
  for (const auto& t : rules.tiles)
  {
    if (t.tile.tileset >= tileset_map.size() or tileset_map[t.tile.tileset] == kNoTileSet)
    {
      return Status::error(
        "%s: WFC tile refers to tileset '%s', which this level doesn't have",
        path,
        (t.tile.tileset < tilesets.size()) ? tilesets[t.tile.tileset].c_str() : "?");
    }
  }
  for (const auto& a : rules.adjacency)
  {
    if (a.a >= rules.tiles.size() or a.b >= rules.tiles.size())
    {
      return Status::error("%s: adjacency refers to a missing WFC tile", path);
    }
  }
  level.wfc.name = rules.name;
  level.wfc.diagonals = rules.diagonals;
  level.wfc.tiles.clear();
  for (WfcTile t : rules.tiles)
  {
    t.tile.tileset = tileset_map[t.tile.tileset];
    t.semantic = (t.semantic < semantic_map.size()) ? semantic_map[t.semantic] : kNoSemantic;
    level.wfc.tiles.push_back(t);
  }
  level.wfc.adjacency.assign(rules.adjacency.begin(), rules.adjacency.end());
  return Status::okay();
}

Status save_paths_binary(const char* path, const Level& level, IoContext& ctx)
{
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  ObjectRecords records{temp};
  collect(level.registry, records, true);
  PathsFile file{const_cast<memory::Vector<PathGraph>*>(&level.graphs), &records};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kPathGraph, file);
}

Status load_paths_binary(const char* path, Level& level, IoContext& ctx)
{
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto graphs = memory::make_vector<PathGraph>(temp);
  ObjectRecords records{temp};
  PathsFile file{&graphs, &records};
  LVL_RETURN_IF_ERROR(read_binary(path, ctx, binary::Kind::kPathGraph, file));
  for (const auto& n : records.nodes)
  {
    if (n.graph >= graphs.size())
    {
      return Status::error("%s: path node refers to a missing graph", path);
    }
  }
  while (!level.graphs.empty())
  {
    remove_graph(level, static_cast<GraphId>(level.graphs.size() - 1));
  }
  level.graphs.assign(graphs.begin(), graphs.end());
  if (Status s = restore(records, level.registry, level.graphs.size(), true); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  return Status::okay();
}

}  // namespace io
}  // namespace lvl::level
