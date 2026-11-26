/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io_common.hpp
 *
 * Internals shared by the JSON and binary level formats.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/json.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/io.hpp"
#include "lvl/level/level.hpp"

namespace lvl::level
{

template <typename S> void serialize(S& s, Position& p)
{
  s.value4b(p.x);
  s.value4b(p.y);
}

template <typename S> void serialize(S& s, Extent& e)
{
  s.value4b(e.width);
  s.value4b(e.height);
}

template <typename S> void serialize(S& s, PathGraph& g)
{
  s.object(g.name);
  s.value1b(g.kind);
  s.object(g.color);
  s.value1b(g.visible);
}

}  // namespace lvl::level

namespace lvl::level::io
{

/// Most workers a level's layers are spread over (per-worker bookkeeping is a fixed array)
inline constexpr std::size_t kMaxIoWorkers = 64;

/// The arena for buffers sized by the level rather than by the worker count (see IoContext::shared)
inline memory::Arena& shared_arena(IoContext& ctx)
{
  return (ctx.shared != nullptr) ? *ctx.shared : ctx.workers[0].scratch;
}

/// Format/kind names written into JSON files
inline constexpr std::string_view kLevelFormat = "lvl-level";
inline constexpr std::string_view kTileSetFormat = "lvl-tileset";
inline constexpr std::string_view kWfcFormat = "lvl-wfc";
inline constexpr std::string_view kScatterFormat = "lvl-scatter";
inline constexpr std::string_view kPathsFormat = "lvl-paths";

/// A layer without its data plane
struct LayerHeader
{
  Name name;
  LayerKind kind = LayerKind::kTiles;
  bool visible = true;
  bool locked = false;
  float opacity = 1.0F;
  Preview preview = Preview::kNatural;
  float range_min = 0.0F;
  float range_max = 1.0F;
  float contour_interval = 0.1F;
};

LayerHeader header_of(const Layer& layer);
void apply_header(Layer& layer, const LayerHeader& header);

bool load(json::Reader& r, const Json& j, LayerHeader& h);
void save(Json& j, const LayerHeader& h);

template <typename S> void serialize(S& s, LayerHeader& h)
{
  s.object(h.name);
  s.value1b(h.kind);
  s.value1b(h.visible);
  s.value1b(h.locked);
  s.value4b(h.opacity);
  s.value1b(h.preview);
  s.value4b(h.range_min);
  s.value4b(h.range_max);
  s.value4b(h.contour_interval);
}

/// Size in bytes of one cell of a layer of <code>kind</code>
std::size_t cell_size(LayerKind kind);

/// Raw pointer to a layer plane's cells
const void* cells(const Layer& layer);
void* cells(Layer& layer);

/// Portals/markers/path graphs as plain records, with entity references as indices
struct PortalRecord
{
  Name name;
  Position position;
  Extent extent;
  Portal portal;
  /**
   * @brief The destination level as the file carries it, which may be a path
   *
   * <code>Portal::target_level</code> is a member's name, but it was a path before projects, and a
   * path does not fit a <code>Name</code>. So the wire field stays this wide -- writing a name into
   * it is what every file written from now on does -- and the loaders migrate it into the portal
   * (<code>migrate_portal_targets</code>) once they have the project whose members it names.
   */
  Path target_level_field;
};

struct MarkerRecord
{
  Name name;
  Name type;
  Position position;
};

struct NodeRecord
{
  GraphId graph = 0;
  Position position;
  float wait = 0.0F;
};

struct EdgeRecord
{
  /// Indices into the node records
  std::uint32_t from = 0;
  std::uint32_t to = 0;
  float cost = 1.0F;
  bool bidirectional = true;
};

struct ObjectRecords
{
  explicit ObjectRecords(memory::Arena& arena) :
      portals{memory::ArenaAllocator<PortalRecord>{arena}},
      markers{memory::ArenaAllocator<MarkerRecord>{arena}},
      nodes{memory::ArenaAllocator<NodeRecord>{arena}},
      edges{memory::ArenaAllocator<EdgeRecord>{arena}}
  {}

  memory::Vector<PortalRecord> portals;
  memory::Vector<MarkerRecord> markers;
  memory::Vector<NodeRecord> nodes;
  memory::Vector<EdgeRecord> edges;
};

/// Snapshot of the registry's objects (stable order: by entity creation)
void collect(const Registry& registry, ObjectRecords& out, bool paths_only = false);

/// Creates entities for the records (nodes/edges get graph ids offset by nothing)
Status restore(const ObjectRecords& records, Registry& registry, std::size_t graph_count, bool paths_only = false);

/**
 * @brief Turns each record's destination level into the name of a member of <code>project</code>
 *
 * A file written before projects names its destination by path, so it is matched to the member
 * whose file has that name. One that is already a member's name is kept as it is. Anything else is
 * reduced to the file name's stem, which is what the level would have been called, and logged as a
 * warning rather than an error: a portal that does not resolve is a thing to fix in the editor, not
 * a reason to refuse the file. Call it before <code>restore</code>.
 */
void migrate_portal_targets(const project::Project& project, const char* path, ObjectRecords& records);

template <typename S> void serialize(S& s, PortalRecord& r)
{
  s.object(r.name);
  s.object(r.position);
  s.object(r.extent);
  s.object(r.target_level_field);
  s.object(r.portal.target_portal);
  s.value1b(r.portal.exit);
  s.value1b(r.portal.interact);
}

template <typename S> void serialize(S& s, MarkerRecord& r)
{
  s.object(r.name);
  s.object(r.type);
  s.object(r.position);
}

template <typename S> void serialize(S& s, NodeRecord& r)
{
  s.value2b(r.graph);
  s.object(r.position);
  s.value4b(r.wait);
}

template <typename S> void serialize(S& s, EdgeRecord& r)
{
  s.value4b(r.from);
  s.value4b(r.to);
  s.value4b(r.cost);
  s.value1b(r.bidirectional);
}

/// Runs <code>fn(IoWorker&)</code> with the worker's JSON arena installed on this thread (or the
/// thread's existing JSON arena, if it already has one)
template <typename Fn> decltype(auto) with_json(IoWorker& worker, Fn&& fn)
{
  if (json::is_installed())
  {
    return fn(worker);
  }
  json::Installation installation{worker.json};
  return fn(worker);
}

/// Progress helpers (progress is optional)
inline void progress_reset(IoContext& ctx, std::size_t total)
{
  if (ctx.progress != nullptr)
  {
    ctx.progress->reset(total);
  }
}

inline void progress_stage(IoContext& ctx, const char* stage)
{
  if (ctx.progress != nullptr)
  {
    ctx.progress->stage(stage);
  }
}

inline void progress_advance(IoContext& ctx)
{
  if (ctx.progress != nullptr)
  {
    ctx.progress->advance();
  }
}

// JSON format (io_json.cpp)
Status save_level_json(
  const char* path,
  const project::Project& project,
  const Level& level,
  IoContext& ctx,
  LevelScope scope);
Status load_level_json(const char* path, project::Project& project, Level& level, IoContext& ctx);
Status save_tileset_json(const char* path, const project::Project& project, TileSetId id, IoContext& ctx);
Status load_tileset_json(const char* path, project::Project& project, IoContext& ctx, TileSetId& id);
Status save_wfc_json(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);
Status load_wfc_json(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);
Status save_scatter_json(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);
Status load_scatter_json(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);
Status save_paths_json(const char* path, const Level& level, IoContext& ctx);
Status load_paths_json(const char* path, Level& level, IoContext& ctx);

// Binary format (io_binary.cpp)
Status save_level_binary(
  const char* path,
  const project::Project& project,
  const Level& level,
  IoContext& ctx,
  LevelScope scope);
Status load_level_binary(const char* path, project::Project& project, Level& level, IoContext& ctx);
Status save_tileset_binary(const char* path, const project::Project& project, TileSetId id, IoContext& ctx);
Status load_tileset_binary(const char* path, project::Project& project, IoContext& ctx, TileSetId& id);
Status save_wfc_binary(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);
Status load_wfc_binary(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);
Status save_scatter_binary(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);
Status load_scatter_binary(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);
Status save_paths_binary(const char* path, const Level& level, IoContext& ctx);
Status load_paths_binary(const char* path, Level& level, IoContext& ctx);

}  // namespace lvl::level::io
