/**
 * @copyright 2026-present Brian Cairl
 *
 * @file project.hpp
 *
 * A project: the folder tree (see paths.hpp), the data every one of its levels shares, and an index
 * of the levels themselves.
 *
 * Only one level is ever resident in memory, because a level carries whole grid planes and a project
 * may hold many. The others are represented by a <code>LevelEntry</code> each: a name, a file, the
 * grid size, and the names of the portals and markers the level holds. That is enough to list the
 * project, to offer a portal a destination, and to say whether a link still resolves -- all without
 * loading a single sibling. The index is written into the manifest, so opening a project reads one
 * small file.
 *
 * Everything here is allocated from one project arena, which outlives the document arenas, so a
 * level may borrow from the project but never the other way round. Functions that need both take
 * them as separate arguments rather than the level holding a back-pointer: it keeps the data
 * dependency visible, and it keeps <code>level::Level</code> relocatable between document slots.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"
#include "lvl/level/grid.hpp"
#include "lvl/level/types.hpp"

namespace lvl::project
{

/// Member levels a project may hold
inline constexpr std::size_t kMaxLevels = 256;

/**
 * @brief Portal and marker names the index can hold, across every member level
 *
 * The table is reserved to this size when the project is created and never grows, so rebuilding it
 * (which every level save does) reuses the same block instead of leaving the old one behind in the
 * bump arena.
 */
inline constexpr std::size_t kMaxObjectNames = 4096;

/// Returned by the lookups when there is no such member level
inline constexpr std::size_t kNoLevel = static_cast<std::size_t>(-1);

/**
 * @brief A member level: enough to list, link and validate it without loading it
 *
 * <code>portals</code> and <code>markers</code> are spans into <code>Project::object_names</code>.
 */
struct LevelEntry
{
  /// Unique within the project; this is what a portal names as its destination
  Name name;
  /// The level's file, relative to the project root (so "levels/overworld.json")
  Path file;
  /// Grid size, for the project listing (the level itself is the authority)
  level::Extents extents = {0, 0};
  std::uint32_t portals_begin = 0;
  std::uint32_t portal_count = 0;
  std::uint32_t markers_begin = 0;
  std::uint32_t marker_count = 0;
  /// The file is not where the manifest says it is (checked when the project is opened)
  bool missing = false;
};

/// What a resident level contributes to its index entry
struct LevelObjects
{
  level::Extents extents = {0, 0};
  const Name* portals = nullptr;
  std::size_t portal_count = 0;
  const Name* markers = nullptr;
  std::size_t marker_count = 0;
};

struct Project
{
  explicit Project(memory::Arena& arena);

  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;

  /// Arena all of this project's data lives in
  memory::Arena* arena;

  /// Display name; defaults to the root folder's own name
  Name name = "untitled";
  /**
   * @brief Absolute path of the folder holding the manifest
   *
   * Derived from where the manifest was loaded from, never serialized: a project that is moved must
   * still work.
   */
  Path root;

  // Shared by every level of the project, and referenced by name from the level files
  /// Semantic types; index 0 is the reserved "none" entry
  memory::Vector<level::SemanticType> semantics;
  /// Materials; index 0 is the reserved "none" entry
  memory::Vector<level::Material> materials;
  /// Seasons, in yearly order (at least one)
  memory::Vector<level::Season> seasons;

  /**
   * @brief The tileset and rule-set files the project owns, relative to the root
   *
   * Their contents are loaded into the project alongside the level model move; until then these are
   * the lists the manifest round-trips, so a project written now still names its parts.
   */
  memory::Vector<Path> tileset_files;
  memory::Vector<Path> rule_files;

  /// Member levels, in the order the project lists them
  memory::Vector<LevelEntry> levels;
  /// Flat name table the LevelEntry spans point into
  memory::Vector<Name> object_names;
};

/// Destroys everything in <code>project</code>, rewinds its arena and re-creates it empty
void reset(Project& project);

/**
 * @brief An empty project named <code>name</code>, rooted at <code>root</code>
 *
 * The result has the default catalogs (a "none" semantic type and material) and one season, like a
 * new level used to. It does not touch the file system; call <code>ensure_tree</code> for that.
 */
void create_default(Project& project, std::string_view name, std::string_view root);

/// Index of the member level named <code>name</code>, or <code>kNoLevel</code>
[[nodiscard]] std::size_t find_level(const Project& project, std::string_view name);

/// Index of the member level whose file is <code>file</code> (root-relative), or <code>kNoLevel</code>
[[nodiscard]] std::size_t find_level_by_file(const Project& project, std::string_view file);

/**
 * @brief Index of the member level whose file has the same name as <code>path</code>
 *
 * How a portal that names its destination by path -- as they did before the project knew the levels
 * by name -- is matched to a member on load.
 */
[[nodiscard]] std::size_t find_level_by_file_name(const Project& project, std::string_view path);

/**
 * @brief Adds a member level; returns its index, or <code>kNoLevel</code> when the project is full
 *
 * <code>file</code> is relative to the root. A name already in use is refused the same way, since
 * portals address levels by name.
 */
std::size_t add_level(Project& project, std::string_view name, std::string_view file);

/// Removes member level <code>index</code> and rebuilds the name table
void remove_level(Project& project, std::size_t index, memory::Arena& scratch);

/// Renames member level <code>index</code> (a no-op returning false if the name is taken)
bool rename_level(Project& project, std::size_t index, std::string_view name);

/// The portal names of member level <code>entry</code> (<code>entry.portal_count</code> of them)
[[nodiscard]] const Name* portals_of(const Project& project, const LevelEntry& entry);

/// The marker names of member level <code>entry</code>
[[nodiscard]] const Name* markers_of(const Project& project, const LevelEntry& entry);

/**
 * @brief Replaces what member level <code>index</code> contributes to the index
 *
 * Called when a level is saved, from the level that was just written. The whole name table is
 * rebuilt, because an entry's names are a span and the spans after it move; <code>scratch</code>
 * holds the new table while it is built and is rewound before returning.
 */
Status set_level_objects(Project& project, std::size_t index, const LevelObjects& objects, memory::Arena& scratch);

/// Marks every member level whose file is not on disk (as the project is opened)
void check_level_files(Project& project);

/**
 * @brief Whether a portal naming <code>level</code> and <code>portal</code> still resolves
 *
 * An empty <code>level</code> means "this level", which only the caller can check, so it is
 * reported as resolving.
 */
enum class LinkState : std::uint8_t
{
  /// Both the level and the portal are there
  kResolved,
  /// The destination is this same level
  kSelf,
  /// No member level of that name
  kUnknownLevel,
  /// The level is a member, but holds no portal of that name
  kUnknownPortal,
};

[[nodiscard]] LinkState portal_link(const Project& project, std::string_view level, std::string_view portal);

/// Index of the catalog entry named <code>name</code> (adding it if it is missing)
level::SemanticId intern_semantic(Project& project, std::string_view name);
level::MaterialId intern_material(Project& project, std::string_view name);

/// Index of the season named <code>name</code>, or <code>seasons.size()</code>
[[nodiscard]] level::SeasonId find_season(const Project& project, std::string_view name);

}  // namespace lvl::project
