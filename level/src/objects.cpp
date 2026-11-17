/**
 * @copyright 2026-present Brian Cairl
 *
 * @file objects.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>
#include <limits>

// LVL
#include "lvl/level/objects.hpp"

namespace lvl::level
{

Entity create_portal(Registry& registry, std::string_view name, Position position, Extent extent, const Portal& portal)
{
  const Entity e = registry.create();
  registry.emplace<Position>(e, position);
  registry.emplace<Extent>(e, extent);
  registry.emplace<Label>(e, Label{Name{name}});
  registry.emplace<Portal>(e, portal);
  return e;
}

Entity create_path_node(Registry& registry, GraphId graph, Position position)
{
  const Entity e = registry.create();
  registry.emplace<Position>(e, position);
  registry.emplace<PathNode>(e, PathNode{.graph = graph});
  return e;
}

Entity create_path_edge(Registry& registry, Entity from, Entity to, float cost, bool bidirectional)
{
  LVL_ASSERT(registry.all_of<PathNode>(from) and registry.all_of<PathNode>(to)) << "edge endpoints must be path nodes";
  LVL_ASSERT_EQ(registry.get<PathNode>(from).graph, registry.get<PathNode>(to).graph)
    << "edge endpoints must belong to the same graph";
  const Entity e = registry.create();
  registry.emplace<PathEdge>(e, PathEdge{.from = from, .to = to, .cost = cost, .bidirectional = bidirectional});
  return e;
}

Entity create_marker(Registry& registry, std::string_view name, std::string_view type, Position position)
{
  const Entity e = registry.create();
  registry.emplace<Position>(e, position);
  registry.emplace<Label>(e, Label{Name{name}});
  registry.emplace<Marker>(e, Marker{Name{type}});
  return e;
}

void destroy_path_node(Registry& registry, Entity node)
{
  // Collect first: destroying while iterating a view of the same storage is not allowed
  constexpr std::size_t kMaxBatch = 256;
  Entity doomed[kMaxBatch];
  for (bool more = true; more;)
  {
    std::size_t n = 0;
    more = false;
    for (auto [e, edge] : registry.view<PathEdge>().each())
    {
      if (edge.from == node or edge.to == node)
      {
        if (n == kMaxBatch)
        {
          more = true;
          break;
        }
        doomed[n++] = e;
      }
    }
    registry.destroy(doomed, doomed + n);
  }
  registry.destroy(node);
}

void destroy_path_graph(Registry& registry, GraphId graph)
{
  constexpr std::size_t kMaxBatch = 256;
  Entity doomed[kMaxBatch];
  for (bool more = true; more;)
  {
    std::size_t n = 0;
    more = false;
    for (auto [e, node] : registry.view<PathNode>().each())
    {
      if (node.graph == graph)
      {
        if (n == kMaxBatch)
        {
          more = true;
          break;
        }
        doomed[n++] = e;
      }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
      destroy_path_node(registry, doomed[i]);
    }
  }
  for (auto [e, node] : registry.view<PathNode>().each())
  {
    if (node.graph > graph)
    {
      --node.graph;
    }
  }
}

Entity find_portal(const Registry& registry, std::string_view name)
{
  for (auto [e, label, portal] : registry.view<Label, Portal>().each())
  {
    if (label.name == name)
    {
      return e;
    }
  }
  return kNullEntity;
}

Entity pick(const Registry& registry, Position p, float radius)
{
  Entity best = kNullEntity;
  float best_d2 = radius * radius;
  // Area objects: inside their extent counts as distance 0
  for (auto [e, pos, ext] : registry.view<Position, Extent>().each())
  {
    if (p.x >= pos.x and p.y >= pos.y and p.x <= pos.x + ext.width and p.y <= pos.y + ext.height)
    {
      return e;
    }
  }
  for (auto [e, pos] : registry.view<Position>().each())
  {
    const float dx = pos.x - p.x;
    const float dy = pos.y - p.y;
    if (const float d2 = dx * dx + dy * dy; d2 <= best_d2)
    {
      best_d2 = d2;
      best = e;
    }
  }
  return best;
}

void shortest_path(const Registry& registry, Entity from, Entity to, memory::Vector<Entity>& out)
{
  out.clear();
  if (
    !registry.valid(from) or !registry.valid(to) or !registry.all_of<PathNode>(from) or !registry.all_of<PathNode>(to))
  {
    return;
  }
  // Dense indices for the graph's nodes
  const GraphId graph = registry.get<PathNode>(from).graph;
  memory::Vector<Entity> nodes{out.get_allocator()};
  for (auto [e, node] : registry.view<PathNode>().each())
  {
    if (node.graph == graph)
    {
      nodes.push_back(e);
    }
  }
  const auto index_of = [&nodes](Entity e) {
    return static_cast<std::size_t>(std::find(nodes.begin(), nodes.end(), e) - nodes.begin());
  };
  const std::size_t n = nodes.size();
  memory::Vector<float> dist(
    n, std::numeric_limits<float>::infinity(), memory::ArenaAllocator<float>{out.get_allocator()});
  memory::Vector<std::size_t> prev(n, n, memory::ArenaAllocator<std::size_t>{out.get_allocator()});
  memory::Vector<bool> done(n, false, memory::ArenaAllocator<bool>{out.get_allocator()});
  const std::size_t source = index_of(from);
  const std::size_t target = index_of(to);
  dist[source] = 0.0F;
  // O(n^2) Dijkstra: graphs are editor-sized (hundreds of nodes)
  for (std::size_t iter = 0; iter < n; ++iter)
  {
    std::size_t u = n;
    for (std::size_t i = 0; i < n; ++i)
    {
      if (!done[i] and (u == n or dist[i] < dist[u]))
      {
        u = i;
      }
    }
    if (u == n or std::isinf(dist[u]) or u == target)
    {
      break;
    }
    done[u] = true;
    for (auto [e, edge] : registry.view<PathEdge>().each())
    {
      std::size_t v = n;
      if (edge.from == nodes[u])
      {
        v = index_of(edge.to);
      }
      else if (edge.bidirectional and edge.to == nodes[u])
      {
        v = index_of(edge.from);
      }
      if (v >= n or done[v])
      {
        continue;
      }
      const auto& a = registry.get<Position>(nodes[u]);
      const auto& b = registry.get<Position>(nodes[v]);
      const float length = std::hypot(a.x - b.x, a.y - b.y) * edge.cost;
      if (dist[u] + length < dist[v])
      {
        dist[v] = dist[u] + length;
        prev[v] = u;
      }
    }
  }
  if (std::isinf(dist[target]))
  {
    return;
  }
  for (std::size_t i = target; i != n; i = prev[i])
  {
    out.push_back(nodes[i]);
  }
  std::reverse(out.begin(), out.end());
}

}  // namespace lvl::level
