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

template <typename S> constexpr bool is_reader = std::is_same_v<S, binary::Reader>;

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
  s.value4b(v.scatter_weight);
}

template <typename S> void serialize(S& s, StepTerrain& v)
{
  s.object(v.name);
  s.value4b(v.level_lo);
  s.value4b(v.level_hi);
  for (TileRef& tile : v.tiles)
  {
    s.object(tile);
  }
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

template <typename S> void serialize(S& s, TileGroup& v)
{
  s.object(v.name);
  s.object(v.size);
  s.value4b(v.first);
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
  // Appended in 0.3; a file written before that has no groups rather than bytes here
  if (binary::has_version(s, 300))
  {
    s.container(v.groups, 0xFFFF);
    s.container2b(v.group_cells, 1 << 22);
  }
  else if constexpr (is_reader<S>)
  {
    v.groups.clear();
    v.group_cells.clear();
  }
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

template <typename S> void serialize(S& s, AutoVariant& v)
{
  s.value1b(v.mask);
  s.object(v.tile);
}

template <typename S> void serialize(S& s, AutoTerrain& v)
{
  s.object(v.name);
  s.value1b(v.neighborhood);
  s.value4b(v.expand);
  s.container(v.variants, 1 << 12);
}

namespace io
{
namespace
{

constexpr std::size_t kMaxList = 1 << 20;

/// Terrains aren't default-constructible either (see serialize_tilesets below)
struct Terrains
{
  AutoTileRules* rules;
  memory::Arena* arena;
};

/// Tilesets aren't default-constructible (they need the arena), so the list is (de)serialized by hand
template <typename S> void serialize_tilesets(S& s, project::Project& project)
{
  auto count = static_cast<std::uint32_t>(project.tilesets.size());
  s.value4b(count);
  if constexpr (is_reader<S>)
  {
    if (count >= kNoTileSet)
    {
      s.adapter().error(bitsery::ReaderError::InvalidData);
      return;
    }
    project.tilesets.clear();
    for (std::uint32_t i = 0; i < count and s.adapter().error() == bitsery::ReaderError::NoError; ++i)
    {
      project.tilesets.emplace_back(*project.arena);
      s.object(project.tilesets.back());
      validate_groups(project.tilesets.back());
    }
  }
  else
  {
    for (auto& ts : project.tilesets)
    {
      s.object(ts);
    }
  }
}

template <typename S> void serialize(S& s, Terrains& t)
{
  auto count = static_cast<std::uint32_t>(t.rules->terrains.size());
  s.value4b(count);
  if constexpr (is_reader<S>)
  {
    if (count >= kNoTerrain)
    {
      s.adapter().error(bitsery::ReaderError::InvalidData);
      return;
    }
    t.rules->terrains.clear();
    for (std::uint32_t i = 0; i < count and s.adapter().error() == bitsery::ReaderError::NoError; ++i)
    {
      t.rules->terrains.emplace_back(*t.arena);
      s.object(t.rules->terrains.back());
    }
  }
  else
  {
    for (auto& terrain : t.rules->terrains)
    {
      s.object(terrain);
    }
  }
}

/// Document metadata: the level minus layer cells, with objects as records
struct Meta
{
  Level* level;
  /// Holds the catalogs, seasons, tilesets and rule sets, when this file carries them
  project::Project* project;
  ObjectRecords* records;
  memory::Vector<LayerHeader>* layers;
  /**
   * @brief The rule set this file carries, when it carries one
   *
   * Not serialized, and set by both sides before the block is touched, because the same code reads
   * and writes it: the writer points it at the set the level uses, the reader at the empty set it
   * made to read into.
   */
  project::RuleSet* rules = nullptr;
  /**
   * @brief Whether the shared sections are part of this block
   *
   * Not serialized: the header's <code>Kind</code> says which it is, so the writer sets it from the
   * scope and the reader from the kind, before the block is touched.
   */
  bool shared = true;
};

template <typename S> void serialize(S& s, Meta& m)
{
  Level& level = *m.level;
  project::Project& project = *m.project;
  s.object(level.settings);
  if (m.shared)
  {
    s.container(project.semantics, kMaxList);
    s.container(project.materials, kMaxList);
    s.container(project.seasons, 256);
    serialize_tilesets(s, project);
    s.object(m.rules->wfc);
    Terrains terrains{&m.rules->auto_tiles, project.arena};
    s.object(terrains);
    s.container(m.rules->steps.terrains, kMaxList);
  }
  else
  {
    // The rules are in the project, so the level has to name the set it means. Only a kProjectLevel
    // file reaches this branch, and that kind is new, so no existing file's bytes move.
    s.object(level.rules);
  }
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

Status save_level_binary(
  const char* path,
  const project::Project& project,
  const Level& level_in,
  IoContext& ctx,
  LevelScope scope)
{
  // Bitsery's serializer takes non-const references; nothing is modified when writing
  Level& level = const_cast<Level&>(level_in);
  project::Project& project_out = const_cast<project::Project&>(project);
  IoWorker& main = ctx.workers[0];
  memory::Scope scratch_scope{main.scratch};
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
  const bool shared = scope == LevelScope::kStandalone;
  // The one set the level uses: a standalone file carries it, a project-scope file names it
  project::RuleSet& rules = project_out.rules[project::rules_of(project_out, level)];
  Meta meta{&level, &project_out, &records, &headers, &rules, shared};
  auto buffer = memory::make_vector<std::uint8_t>(main.scratch);
  const std::size_t meta_size =
    binary::serialize(buffer, shared ? binary::Kind::kLevel : binary::Kind::kProjectLevel, meta);
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

Status load_level_binary(const char* path, project::Project& project, Level& level, IoContext& ctx)
{
  // The layer chunks are read in place, so the whole file stays resident in the shared arena
  memory::Arena& shared = shared_arena(ctx);
  memory::Scope scope{shared};
  progress_reset(ctx, 3);
  progress_stage(ctx, "Reading");

  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, shared, data, size));
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  progress_advance(ctx);

  progress_stage(ctx, "Decoding document");
  ObjectRecords records{shared};
  auto headers = memory::make_vector<LayerHeader>(shared);
  Meta meta{&level, &project, &records, &headers};
  binary::Version version;
  binary::Reader reader{version, binary::InputAdapter{bytes, size}};
  binary::Header header;
  reader.object(header);
  const bool level_kind = header.kind == binary::Kind::kLevel or header.kind == binary::Kind::kProjectLevel;
  if (reader.adapter().error() != bitsery::ReaderError::NoError or header.magic != binary::kMagic or !level_kind)
  {
    return Status::error("%s: not a lvl binary level", path);
  }
  // Which shape the metadata block has; a project-scope file leaves the project's own data alone
  meta.shared = header.kind == binary::Kind::kLevel;
  if (header.version > static_cast<std::uint32_t>(kVersion))
  {
    return Status::error("%s: written by a newer editor (version %u)", path, static_cast<unsigned>(header.version));
  }
  // The metadata block is read in the shape that editor wrote it in
  version.value = header.version;
  if (meta.shared)
  {
    // A file that carries its rules carries exactly one set, the one its cells were tiled with, so
    // it replaces the project's as its tilesets and seasons do
    project.rules.clear();
    project::add_rules(project, "");
    meta.rules = &project.rules[0];
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
  if (meta.shared)
  {
    if (project.seasons.empty())
    {
      project.seasons.push_back(Season{.name = "default"});
    }
    // The set it carried names itself, so the level takes its name from what it read
    level.rules = project.rules[0].wfc.name;
  }
  // A file written before projects names its destination level by path; a portal points at a member
  migrate_portal_targets(project, path, records);
  if (Status s = restore(records, level.registry, level.graphs.size()); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }

  // Locate the layer chunks
  std::size_t offset = reader.adapter().currentReadPos();
  const std::size_t layer_count = headers.size();
  auto chunk_data = memory::make_vector<const std::uint8_t*>(shared, layer_count, nullptr);
  auto chunks = memory::make_vector<ChunkHeader>(shared, layer_count, ChunkHeader{});
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
  auto ok = memory::make_vector<std::uint8_t>(shared, layer_count, std::uint8_t{0});
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
      if (!t.empty() and t.tileset >= project.tilesets.size())
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

/// The standalone rules file carries both kinds of tile adjacency rules
struct WfcFile
{
  memory::Vector<SemanticType>* semantics;
  memory::Vector<Name>* tilesets;
  WfcRules* rules;
  AutoTileRules* auto_tiles;
  memory::Vector<StepTerrain>* steps;
  memory::Arena* arena;
};

template <typename S> void serialize(S& s, WfcFile& f)
{
  s.container(*f.semantics, kMaxList);
  s.container(*f.tilesets, kMaxList);
  s.object(*f.rules);
  Terrains terrains{f.auto_tiles, f.arena};
  s.object(terrains);
  s.container(*f.steps, kMaxList);
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

Status save_tileset_binary(const char* path, const project::Project& project, TileSetId id, IoContext& ctx)
{
  TileSetFile file{
    const_cast<memory::Vector<SemanticType>*>(&project.semantics),
    const_cast<memory::Vector<Material>*>(&project.materials),
    const_cast<TileSet*>(&project.tilesets[id])};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kTileSet, file);
}

Status load_tileset_binary(const char* path, project::Project& project, IoContext& ctx, TileSetId& id)
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

  const auto semantic_map = remap(temp, semantics, project.semantics, [&project](std::string_view n) {
    return project::intern_semantic(project, n);
  });
  const auto material_map = remap(temp, materials, project.materials, [&project](std::string_view n) {
    return project::intern_material(project, n);
  });
  const auto mapped = [](const memory::Vector<std::uint16_t>& map, std::uint16_t v) -> std::uint16_t {
    return (v < map.size()) ? map[v] : 0;
  };

  id = project::add_tileset(project, tileset.name.view());
  TileSet& ts = project.tilesets[id];
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
  ts.groups.assign(tileset.groups.begin(), tileset.groups.end());
  ts.group_cells.assign(tileset.group_cells.begin(), tileset.group_cells.end());
  if (const std::size_t dropped = validate_groups(ts); dropped > 0)
  {
    LVL_LOG_WARN() << "Dropped " << dropped << " tile group(s) of '" << ts.name.c_str() << "': inconsistent cells";
  }
  for (SeasonalVariant v : tileset.seasonal)
  {
    v.material = mapped(material_map, v.material);
    ts.seasonal.push_back(v);
  }
  return Status::okay();
}

Status save_wfc_binary(const char* path, const project::Project& project, std::size_t index, IoContext& ctx)
{
  const project::RuleSet& set = project.rules[index];
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto tilesets = memory::make_vector<Name>(temp);
  for (const auto& ts : project.tilesets)
  {
    tilesets.push_back(ts.name);
  }
  WfcFile file{
    const_cast<memory::Vector<SemanticType>*>(&project.semantics),
    &tilesets,
    const_cast<WfcRules*>(&set.wfc),
    const_cast<AutoTileRules*>(&set.auto_tiles),
    const_cast<memory::Vector<StepTerrain>*>(&set.steps.terrains),
    &temp};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kWfcRules, file);
}

Status load_wfc_binary(const char* path, project::Project& project, IoContext& ctx, std::size_t& index)
{
  index = project::kNoRules;
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto semantics = memory::make_vector<SemanticType>(temp);
  auto tilesets = memory::make_vector<Name>(temp);
  WfcRules rules{temp};
  AutoTileRules auto_tiles{temp};
  auto steps = memory::make_vector<StepTerrain>(temp);
  WfcFile file{&semantics, &tilesets, &rules, &auto_tiles, &steps, &temp};
  LVL_RETURN_IF_ERROR(read_binary(path, ctx, binary::Kind::kWfcRules, file));

  auto tileset_map = memory::make_vector<TileSetId>(temp, tilesets.size(), kNoTileSet);
  for (std::size_t i = 0; i < tilesets.size(); ++i)
  {
    for (std::size_t k = 0; k < project.tilesets.size(); ++k)
    {
      if (project.tilesets[k].name == tilesets[i])
      {
        tileset_map[i] = static_cast<TileSetId>(k);
      }
    }
  }
  const auto semantic_map = remap(temp, semantics, project.semantics, [&project](std::string_view n) {
    return project::intern_semantic(project, n);
  });
  for (const auto& t : rules.tiles)
  {
    if (t.tile.tileset >= tileset_map.size() or tileset_map[t.tile.tileset] == kNoTileSet)
    {
      return Status::error(
        "%s: WFC tile refers to tileset '%s', which this project doesn't have",
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
  // The file carries one set, named; it replaces the project's set of that name or becomes a new
  // one, so importing the same file twice does not leave two
  index = project::intern_rules(project, rules.name.view());
  project::RuleSet& out_set = project.rules[index];
  out_set.wfc.name = rules.name;
  out_set.wfc.diagonals = rules.diagonals;
  out_set.wfc.tiles.clear();
  for (WfcTile t : rules.tiles)
  {
    t.tile.tileset = tileset_map[t.tile.tileset];
    t.semantic = (t.semantic < semantic_map.size()) ? semantic_map[t.semantic] : kNoSemantic;
    out_set.wfc.tiles.push_back(t);
  }
  out_set.wfc.adjacency.assign(rules.adjacency.begin(), rules.adjacency.end());
  // Terrains, with their tiles remapped the same way
  for (const AutoTerrain& terrain : auto_tiles.terrains)
  {
    for (const AutoVariant& v : terrain.variants)
    {
      if (v.tile.tileset >= tileset_map.size() or tileset_map[v.tile.tileset] == kNoTileSet)
      {
        return Status::error(
          "%s: terrain '%s' refers to tileset '%s', which this project doesn't have",
          path,
          terrain.name.c_str(),
          (v.tile.tileset < tilesets.size()) ? tilesets[v.tile.tileset].c_str() : "?");
      }
    }
  }
  out_set.auto_tiles.terrains.clear();
  for (const AutoTerrain& terrain : auto_tiles.terrains)
  {
    AutoTerrain& out =
      out_set.auto_tiles.terrains[add_terrain(out_set.auto_tiles, *project.arena, terrain.name.view())];
    out.neighborhood = terrain.neighborhood;
    out.expand = terrain.expand;
    for (AutoVariant v : terrain.variants)
    {
      v.tile.tileset = tileset_map[v.tile.tileset];
      set_variant(out, v.mask, v.tile);
    }
  }
  // Step terrains travel the same way; an empty slot stays empty rather than being remapped
  out_set.steps.terrains.clear();
  for (const StepTerrain& terrain : steps)
  {
    StepTerrain& out = out_set.steps.terrains[add_step_terrain(out_set.steps, terrain.name.view())];
    out.level_lo = terrain.level_lo;
    out.level_hi = terrain.level_hi;
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      const TileRef tile = terrain.tiles[i];
      if (tile.empty())
      {
        continue;
      }
      if (tile.tileset >= tileset_map.size() or tileset_map[tile.tileset] == kNoTileSet)
      {
        return Status::error(
          "%s: step terrain '%s' refers to tileset '%s', which this project doesn't have",
          path,
          terrain.name.c_str(),
          (tile.tileset < tilesets.size()) ? tilesets[tile.tileset].c_str() : "?");
      }
      out.tiles[i] = TileRef{tileset_map[tile.tileset], tile.tile};
    }
  }
  return Status::okay();
}

namespace
{

/// A scatter profile file: the catalog entry its ground type names, then the profile
struct ScatterFile
{
  memory::Vector<SemanticType>* semantics;
  Name* name;
  project::ScatterSettings* settings;
  memory::Vector<project::ScatterObject>* objects;
};

template <typename S> void serialize(S& s, ScatterFile& f)
{
  s.container(*f.semantics, kMaxList);
  s.object(*f.name);
  s.object(*f.settings);
  s.container(*f.objects, project::kMaxScatterObjects);
}

}  // namespace

Status save_scatter_binary(const char* path, const project::Project& project, std::size_t index, IoContext& ctx)
{
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  const project::ScatterProfile& profile = project.scatter[index];
  // The semantic catalog travels whole and ids keep their places, as a tileset file's does, so the
  // loader's remap() lines the file's ids up with the reading project's. The objects name their
  // tileset and group, so no tileset has to come along
  auto objects = memory::make_vector<project::ScatterObject>(temp);
  project::ScatterSettings settings = profile.settings;
  if (!settings.use_semantic or settings.semantic >= project.semantics.size())
  {
    settings.use_semantic = false;
    settings.semantic = kNoSemantic;
  }
  if (const project::ScatterObject* entries = project::objects_of(project, profile); entries != nullptr)
  {
    objects.assign(entries, entries + profile.count);
  }
  Name name = profile.name;
  ScatterFile file{const_cast<memory::Vector<SemanticType>*>(&project.semantics), &name, &settings, &objects};
  return binary::write_file(path, ctx.workers[0].scratch, binary::Kind::kScatter, file);
}

Status load_scatter_binary(const char* path, project::Project& project, IoContext& ctx, std::size_t& index)
{
  index = project::kNoScatterProfile;
  memory::Arena& temp = ctx.workers[ctx.worker_count > 1 ? 1 : 0].json;
  memory::Scope scope{temp};
  auto semantics = memory::make_vector<SemanticType>(temp);
  auto objects = memory::make_vector<project::ScatterObject>(temp);
  project::ScatterSettings settings;
  Name name;
  ScatterFile file{&semantics, &name, &settings, &objects};
  LVL_RETURN_IF_ERROR(read_binary(path, ctx, binary::Kind::kScatter, file));

  const auto semantic_map = remap(temp, semantics, project.semantics, [&project](std::string_view n) {
    return project::intern_semantic(project, n);
  });
  if (settings.use_semantic)
  {
    settings.semantic =
      (settings.semantic < semantic_map.size()) ? semantic_map[settings.semantic] : static_cast<SemanticId>(0);
  }
  index = project::intern_scatter(project, name.view());
  if (index == project::kNoScatterProfile)
  {
    return Status::error("%s: the project already holds %zu scatter profiles", path, project::kMaxScatterProfiles);
  }
  project.scatter[index].settings = settings;
  for (const project::ScatterObject& object : objects)
  {
    if (!project::add_scatter_object(project, index, object))
    {
      return Status::error(
        "%s: the scatter profiles name more than %zu objects between them", path, project::kMaxScatterObjects);
    }
  }
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
