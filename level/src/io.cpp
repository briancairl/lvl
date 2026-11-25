/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cstring>
#include <string_view>

// LVL
#include "io_common.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/project/paths.hpp"

namespace lvl::level
{

FileFormat format_of(const char* path)
{
  const std::string_view ext{file::extension(path)};
  return (ext == ".lvlb" or ext == ".bin") ? FileFormat::kBinary : FileFormat::kJson;
}

namespace
{

void check_context(const IoContext& ctx)
{
  LVL_ASSERT_NON_NULL(ctx.workers) << "IoContext needs at least one worker";
  LVL_ASSERT_GT(ctx.worker_count, 0UL) << "IoContext needs at least one worker";
}

/// Logs the outcome of an I/O operation
Status logged(const char* what, const char* path, Status status)
{
  if (status.ok)
  {
    LVL_LOG_INFO() << what << " '" << path << "'";
  }
  else
  {
    LVL_LOG_ERROR() << what << " failed: " << status.message;
  }
  return status;
}

}  // namespace

Status
save_level(const char* path, const project::Project& project, const Level& level, IoContext& ctx, LevelScope scope)
{
  check_context(ctx);
  return logged(
    "Saved level",
    path,
    format_of(path) == FileFormat::kBinary ? io::save_level_binary(path, project, level, ctx, scope)
                                           : io::save_level_json(path, project, level, ctx, scope));
}

Status load_level(const char* path, project::Project& project, Level& level, IoContext& ctx)
{
  check_context(ctx);
  reset(level);
  return logged(
    "Loaded level",
    path,
    format_of(path) == FileFormat::kBinary ? io::load_level_binary(path, project, level, ctx)
                                           : io::load_level_json(path, project, level, ctx));
}

Status save_tileset(const char* path, const project::Project& project, TileSetId id, IoContext& ctx)
{
  check_context(ctx);
  LVL_ASSERT_LT(id, project.tilesets.size());
  return logged(
    "Saved tileset",
    path,
    format_of(path) == FileFormat::kBinary ? io::save_tileset_binary(path, project, id, ctx)
                                           : io::save_tileset_json(path, project, id, ctx));
}

Status load_tileset(const char* path, project::Project& project, IoContext& ctx, TileSetId& id)
{
  check_context(ctx);
  return logged(
    "Loaded tileset",
    path,
    format_of(path) == FileFormat::kBinary ? io::load_tileset_binary(path, project, ctx, id)
                                           : io::load_tileset_json(path, project, ctx, id));
}

Status save_wfc_rules(const char* path, const project::Project& project, std::size_t index, IoContext& ctx)
{
  check_context(ctx);
  return logged(
    "Saved WFC rules",
    path,
    format_of(path) == FileFormat::kBinary ? io::save_wfc_binary(path, project, index, ctx)
                                           : io::save_wfc_json(path, project, index, ctx));
}

Status load_wfc_rules(const char* path, project::Project& project, IoContext& ctx, std::size_t& index)
{
  check_context(ctx);
  return logged(
    "Loaded WFC rules",
    path,
    format_of(path) == FileFormat::kBinary ? io::load_wfc_binary(path, project, ctx, index)
                                           : io::load_wfc_json(path, project, ctx, index));
}

Status save_paths(const char* path, const Level& level, IoContext& ctx)
{
  check_context(ctx);
  return logged(
    "Saved paths",
    path,
    format_of(path) == FileFormat::kBinary ? io::save_paths_binary(path, level, ctx)
                                           : io::save_paths_json(path, level, ctx));
}

Status load_paths(const char* path, Level& level, IoContext& ctx)
{
  check_context(ctx);
  return logged(
    "Loaded paths",
    path,
    format_of(path) == FileFormat::kBinary ? io::load_paths_binary(path, level, ctx)
                                           : io::load_paths_json(path, level, ctx));
}

namespace io
{

LayerHeader header_of(const Layer& layer)
{
  return LayerHeader{
    .name = layer.name,
    .kind = layer.kind,
    .visible = layer.visible,
    .locked = layer.locked,
    .opacity = layer.opacity,
    .preview = layer.preview,
    .range_min = layer.range_min,
    .range_max = layer.range_max,
    .contour_interval = layer.contour_interval};
}

void apply_header(Layer& layer, const LayerHeader& h)
{
  layer.name = h.name;
  layer.visible = h.visible;
  layer.locked = h.locked;
  layer.opacity = h.opacity;
  layer.preview = h.preview;
  layer.range_min = h.range_min;
  layer.range_max = h.range_max;
  layer.contour_interval = h.contour_interval;
}

std::size_t cell_size(LayerKind kind)
{
  switch (plane_index(kind))
  {
  case 0:
    return sizeof(TileRef);
  case 1:
    return sizeof(std::uint16_t);
  case 2:
    return sizeof(float);
  default:
    return sizeof(std::uint8_t);
  }
}

const void* cells(const Layer& layer)
{
  return std::visit([](const auto& g) { return static_cast<const void*>(g.values.data()); }, layer.data);
}

void* cells(Layer& layer)
{
  return std::visit([](auto& g) { return static_cast<void*>(g.values.data()); }, layer.data);
}

void collect(const Registry& registry, ObjectRecords& out, bool paths_only)
{
  // Visit live entities in a stable order (EnTT iterates newest-first; reverse that), so files are
  // stable across save/load
  memory::Arena& arena = *out.nodes.get_allocator().arena();
  auto order = memory::make_vector<Entity>(arena);
  if (const auto* entities = registry.storage<Entity>(); entities != nullptr)
  {
    for (auto [e] : entities->each())
    {
      order.push_back(e);
    }
  }
  std::reverse(order.begin(), order.end());
  auto node_index = memory::make_vector<Entity>(arena);
  for (const Entity e : order)
  {
    if (!paths_only and registry.all_of<Portal>(e))
    {
      const Portal& portal = registry.get<Portal>(e);
      out.portals.push_back(PortalRecord{
        .name = registry.get<Label>(e).name,
        .position = registry.get<Position>(e),
        .extent = registry.get<Extent>(e),
        .portal = portal,
        // What goes in the file: a name now, in a field wide enough for the paths older ones hold
        .target_level_field = Path{portal.target_level.view()}});
    }
    else if (!paths_only and registry.all_of<Marker>(e))
    {
      out.markers.push_back(MarkerRecord{
        .name = registry.get<Label>(e).name,
        .type = registry.get<Marker>(e).type,
        .position = registry.get<Position>(e)});
    }
    else if (registry.all_of<PathNode>(e))
    {
      const auto& node = registry.get<PathNode>(e);
      out.nodes.push_back(NodeRecord{.graph = node.graph, .position = registry.get<Position>(e), .wait = node.wait});
      node_index.push_back(e);
    }
  }
  const auto index_of = [&node_index](Entity e) {
    return static_cast<std::uint32_t>(std::find(node_index.begin(), node_index.end(), e) - node_index.begin());
  };
  for (const Entity e : order)
  {
    if (registry.all_of<PathEdge>(e))
    {
      const auto& edge = registry.get<PathEdge>(e);
      out.edges.push_back(EdgeRecord{
        .from = index_of(edge.from), .to = index_of(edge.to), .cost = edge.cost, .bidirectional = edge.bidirectional});
    }
  }
}

void migrate_portal_targets(const project::Project& project, const char* path, ObjectRecords& records)
{
  for (PortalRecord& r : records.portals)
  {
    const std::string_view field = r.target_level_field.view();
    r.portal.target_level.clear();
    if (field.empty())
    {
      // "this level", which only the caller can check, so there is nothing to resolve
      continue;
    }
    if (project::find_level(project, field) != project::kNoLevel)
    {
      r.portal.target_level.assign(field);
      continue;
    }
    // Written as a path, as they were before projects: the member whose file has that name
    if (const std::size_t member = project::find_level_by_file_name(project, field); member != project::kNoLevel)
    {
      r.portal.target_level = project.levels[member].name;
      continue;
    }
    // Neither: the file name's stem is what the level would have been called. Kept as a name that
    // does not resolve, and said so, rather than refusing the file over a portal the user can fix.
    const std::string_view guess = project::stem(project::file_name(field));
    if (!guess.empty() and guess.size() <= Name::kMaxLength)
    {
      r.portal.target_level.assign(guess);
    }
    LVL_LOG_WARN() << path << ": portal '" << r.name.c_str() << "' points at '" << r.target_level_field.c_str()
                   << "', which is no level of this project";
  }
}

Status
restore(const ObjectRecords& records, Registry& registry, std::size_t graph_count, [[maybe_unused]] bool paths_only)
{
  for (const auto& p : records.portals)
  {
    create_portal(registry, p.name.view(), p.position, p.extent, p.portal);
  }
  for (const auto& m : records.markers)
  {
    create_marker(registry, m.name.view(), m.type.view(), m.position);
  }
  auto nodes = memory::make_vector<Entity>(*records.nodes.get_allocator().arena());
  nodes.reserve(records.nodes.size());
  for (const auto& n : records.nodes)
  {
    if (n.graph >= graph_count)
    {
      return Status::error("path node refers to graph %u, but there are %zu graphs", n.graph, graph_count);
    }
    const Entity e = create_path_node(registry, n.graph, n.position);
    registry.get<PathNode>(e).wait = n.wait;
    nodes.push_back(e);
  }
  for (const auto& edge : records.edges)
  {
    if (edge.from >= nodes.size() or edge.to >= nodes.size())
    {
      return Status::error("path edge refers to node %u/%u, but there are %zu nodes", edge.from, edge.to, nodes.size());
    }
    if (registry.get<PathNode>(nodes[edge.from]).graph != registry.get<PathNode>(nodes[edge.to]).graph)
    {
      return Status::error("path edge %u-%u connects nodes of different graphs", edge.from, edge.to);
    }
    create_path_edge(registry, nodes[edge.from], nodes[edge.to], edge.cost, edge.bidirectional);
  }
  return Status::okay();
}

}  // namespace io
}  // namespace lvl::level
