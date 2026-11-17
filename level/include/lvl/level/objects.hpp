/**
 * @copyright 2026-present Brian Cairl
 *
 * @file objects.hpp
 *
 * Level objects live in an EnTT registry (whose storage is in the level's arena). Each kind of
 * object is a set of plain-data components:
 *
 * - portal:     Position + Extent + Label + Portal
 * - path node:  Position + PathNode
 * - path edge:  PathEdge (references two path-node entities)
 * - marker:     Position + Label + Marker (spawn points, item drops, ...)
 *
 * Systems (editing tools, serializers, renderers) are free functions over registry views.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>

// EnTT
#include <entt/entt.hpp>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"

namespace lvl::level
{

/// Entity registry with arena-backed storage
using Registry = entt::basic_registry<entt::entity, memory::ArenaAllocator<entt::entity>>;

using Entity = entt::entity;

inline constexpr Entity kNullEntity = entt::null;

/// Position in cell units (fractional: objects need not snap to cells)
struct Position
{
  float x = 0.0F;
  float y = 0.0F;
};

/// Size in cell units
struct Extent
{
  float width = 1.0F;
  float height = 1.0F;
};

/// Display / lookup name
struct Label
{
  Name name;
};

/**
 * @brief A transition to another level (or another place in this one)
 *
 * The game moves the player arriving in this portal's area to portal <code>target_portal</code> of
 * level <code>target_level</code>, facing <code>exit</code>.
 */
struct Portal
{
  /// Path of the destination level file (empty = this level)
  Path target_level;
  /// Name of the destination portal in that level
  Name target_portal;
  /// Direction the player faces when arriving through this portal
  Direction exit = Direction::kSouth;
  /// Requires an interaction (e.g. a door) rather than triggering on overlap
  bool interact = false;
};

/// Index of a path graph within a level
using GraphId = std::uint16_t;

/// A node of path graph <code>graph</code>
struct PathNode
{
  GraphId graph = 0;
  /// Waiting time at this node, in seconds (patrol routes)
  float wait = 0.0F;
};

/// An edge between two path nodes (of the same graph)
struct PathEdge
{
  Entity from = kNullEntity;
  Entity to = kNullEntity;
  /// Traversal cost multiplier
  float cost = 1.0F;
  /// Traversable in both directions
  bool bidirectional = true;
};

/// What a path graph describes
enum class GraphKind : std::uint8_t
{
  kRoad,
  kPatrol,
  kRiver,
  kNavigation,
};

/// Metadata of one path graph (its nodes/edges are entities)
struct PathGraph
{
  Name name;
  GraphKind kind = GraphKind::kRoad;
  Color color = {255, 210, 60, 255};
  bool visible = true;
};

/// A generic point of interest (spawn point, item, light, ...)
struct Marker
{
  /// Game-defined type ("spawn", "chest", ...)
  Name type;
};

/// Editor-only tag: the entity is selected
struct Selected
{};

/// Creates a portal entity
Entity create_portal(Registry& registry, std::string_view name, Position position, Extent extent, const Portal& portal);

/// Creates a path node entity
Entity create_path_node(Registry& registry, GraphId graph, Position position);

/// Creates a path edge entity between two nodes (asserts both are nodes of the same graph)
Entity create_path_edge(Registry& registry, Entity from, Entity to, float cost = 1.0F, bool bidirectional = true);

/// Creates a marker entity
Entity create_marker(Registry& registry, std::string_view name, std::string_view type, Position position);

/// Destroys a path node and every edge touching it
void destroy_path_node(Registry& registry, Entity node);

/// Destroys every node/edge of <code>graph</code>, and renumbers nodes of graphs after it
void destroy_path_graph(Registry& registry, GraphId graph);

/// The portal named <code>name</code>, or kNullEntity
Entity find_portal(const Registry& registry, std::string_view name);

/// The entity whose area (portal/marker) or node position is nearest <code>p</code> within
/// <code>radius</code> cells, or kNullEntity
Entity pick(const Registry& registry, Position p, float radius);

/**
 * @brief Shortest path between two nodes of one graph (Dijkstra over edge length * cost)
 *
 * Writes the node sequence into <code>out</code> (empty if unreachable). Used to preview routes.
 */
void shortest_path(const Registry& registry, Entity from, Entity to, memory::Vector<Entity>& out);

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::GraphKind>
{
  static constexpr std::array<std::string_view, 4> kNames = {"road", "patrol", "river", "navigation"};
};
