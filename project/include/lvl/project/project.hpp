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
#include "lvl/level/auto_tile_rules.hpp"
#include "lvl/level/level.hpp"
#include "lvl/level/step_rules.hpp"
#include "lvl/level/tileset.hpp"
#include "lvl/level/types.hpp"
#include "lvl/level/wfc_rules.hpp"

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

/// Most scatter profiles a project may hold
inline constexpr std::size_t kMaxScatterProfiles = 64;

/// Most objects every scatter profile may name between them (see ScatterProfile)
inline constexpr std::size_t kMaxScatterObjects = 1024;

inline constexpr std::size_t kNoScatterProfile = static_cast<std::size_t>(-1);

/// Returned by <code>find_rules</code> when there is no rule set of that name
inline constexpr std::size_t kNoRules = static_cast<std::size_t>(-1);

/**
 * @brief The three kinds of tile rules that go together, as one set
 *
 * They are one set because they are used as one: a terrain's border tiles, a cliff's step tiles and
 * the WFC tiles they sit beside all come from the same tileset. The set's name is
 * <code>wfc.name</code>, which is what the rules file already carries and what the adjacency window
 * shows -- there is no second name to keep in step with it.
 *
 * A project holds a list of them and a level names the one it uses (<code>level::Level::rules</code>),
 * because its levels differ by biome: a cave tileset and an overworld tileset want separate
 * adjacency graphs, separate terrains and separate cliff art. One global set would force every tile
 * of every tileset into one WFC graph, and "only what the layout shows" would then forbid
 * cross-biome neighbours between levels that never touch.
 *
 * A project always holds at least one, as it always holds at least one season, so a level always
 * has rules to resolve to and no caller has an empty case to handle.
 */
struct RuleSet
{
  explicit RuleSet(memory::Arena& arena) : wfc{arena}, auto_tiles{arena}, steps{arena} {}

  level::WfcRules wfc;
  /// Auto-tiling terrains (see level/auto_tile_rules.hpp)
  level::AutoTileRules auto_tiles;
  /// Height-step terrains, keyed on the difference in level between cells (see level/step_rules.hpp)
  level::StepRules steps;
};

/**
 * @brief One object a scatter profile places, and how often
 *
 * The tileset and the group are named rather than indexed, as a portal names its destination level,
 * because a profile outlives what it was written against: a tileset removed renumbers the ones
 * after it and a group removed renumbers the ones after it, and a profile must not then quietly
 * start placing something else. A name that resolves to nothing is listed as dangling and skipped
 * by the scatter, which is a thing to fix rather than a reason to refuse the project.
 */
struct ScatterObject
{
  /// Tileset holding the group
  Name tileset;
  /// The tile group (see level::TileGroup)
  Name group;
  /// Share of the placements this object takes, relative to the others; 0 keeps it out
  float weight = 1.0F;
};

/**
 * @brief The criteria a scatter places objects by
 *
 * Plain numbers rather than a <code>procgen</code> type, because *which* layers the band and the
 * ground type are read from is the open level's business and not the profile's -- a project's levels
 * do not have the same layers in the same order. The editor pairs these with the layers chosen in
 * the panel and builds the <code>procgen::ScatterMask</code> from the two.
 */
struct ScatterSettings
{
  /// How many objects to place
  int count = 24;
  /// Cells of clearance kept between placements; 0 lets two objects touch
  int spacing = 1;
  /// Candidate positions tried per object before that one is given up on
  int attempts = 64;
  /// Place only inside a band of a scalar layer
  bool use_band = false;
  float lo = 0.0F;
  float hi = 1.0F;
  /// Place only where a semantic layer holds <code>semantic</code>
  bool use_semantic = false;
  level::SemanticId semantic = level::kNoSemantic;
  /// Place nothing over a cell the collision mask marks
  bool use_collision = true;
  /// Which bits of that mask block a placement
  std::uint8_t blocked = level::collision::kSolid;
  /// Place only where every cell of the object has no tile yet
  bool only_empty = true;
  /// Whether the scatter honours the select tool's region
  bool region = true;
};

// Binary serialization of what a scatter-profile file carries (the spans are in-memory bookkeeping,
// so ScatterProfile itself is not serialized; see level::save_scatter)
template <typename S> void serialize(S& s, ScatterObject& v)
{
  s.object(v.tileset);
  s.object(v.group);
  s.value4b(v.weight);
}

template <typename S> void serialize(S& s, ScatterSettings& v)
{
  s.value4b(v.count);
  s.value4b(v.spacing);
  s.value4b(v.attempts);
  s.value1b(v.use_band);
  s.value4b(v.lo);
  s.value4b(v.hi);
  s.value1b(v.use_semantic);
  s.value2b(v.semantic);
  s.value1b(v.use_collision);
  s.value1b(v.blocked);
  s.value1b(v.only_empty);
  s.value1b(v.region);
}

/**
 * @brief A named set of objects and the criteria that place them (the Generate panel's Scatter tab)
 *
 * The objects are <code>count</code> entries of <code>Project::scatter_objects</code> starting at
 * <code>first</code> -- the same flat table the level index uses for its object names and a tileset
 * uses for its group cells, so adding a profile reuses the block rather than leaving the last one
 * behind in a bump arena. The table is always packed and in the order the profiles are listed.
 *
 * Profiles belong to the project because the objects they name do: the tile groups live in the
 * project's tilesets, and "oaks and pines on grass between these two heights" is a decision about
 * the world rather than about one level of it.
 */
struct ScatterProfile
{
  Name name;
  /// Offset of this profile's first object in <code>Project::scatter_objects</code>
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  ScatterSettings settings;
};

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
  /// The tilesets its levels draw from, indexed by <code>level::TileSetId</code>
  level::TileSets tilesets;
  /// The tile rules its levels auto-tile with; at least one, each named by its <code>wfc.name</code>
  memory::Vector<RuleSet> rules;

  /**
   * @brief Where each tileset's and rule set's own file lives, relative to the root
   *
   * Parallel to <code>tilesets</code> and to the rule sets: entry <i>i</i> is where that one is
   * written and where it was read from. An entry that is empty, or missing because the vector is
   * shorter, has not been given a file yet; <code>save_project</code> gives it one made from its
   * name (<code>project_file</code>) and records it here, so a part keeps the file it was loaded
   * from rather than being renamed under the user.
   */
  memory::Vector<Path> tileset_files;
  memory::Vector<Path> rule_files;
  /// Where each scatter profile's own file lives, relative to the root; parallel to <code>scatter</code>
  memory::Vector<Path> scatter_files;

  /// Member levels, in the order the project lists them
  memory::Vector<LevelEntry> levels;
  /// Flat name table the LevelEntry spans point into
  memory::Vector<Name> object_names;

  /// Scatter profiles (Generate > Scatter), in the order the project lists them
  memory::Vector<ScatterProfile> scatter;
  /// Every profile's objects, back to back in the order the profiles are listed
  memory::Vector<ScatterObject> scatter_objects;
};

/// Index of the scatter profile named <code>name</code>, or <code>kNoScatterProfile</code>
[[nodiscard]] std::size_t find_scatter(const Project& project, std::string_view name);

/**
 * @brief Adds a scatter profile named <code>name</code>; returns its index
 *
 * Returns <code>kNoScatterProfile</code> if the name is empty, a profile already has it, or the
 * project is full.
 */
std::size_t add_scatter(Project& project, std::string_view name);

/**
 * @brief The profile named <code>name</code>, added empty if the project has none of that name
 *
 * What reading a profile file does, so importing the same one twice does not leave two -- the same
 * bargain <code>intern_rules</code> makes. An unnamed file gets the name "scatter". The returned
 * profile has its objects cleared, since the file is about to fill them.
 */
std::size_t intern_scatter(Project& project, std::string_view name);

/// Removes profile <code>index</code>, compacting the object table the later profiles span into
void remove_scatter(Project& project, std::size_t index);

/// First of profile <code>profile</code>'s objects, or nullptr if it has none
[[nodiscard]] const ScatterObject* objects_of(const Project& project, const ScatterProfile& profile);
[[nodiscard]] ScatterObject* objects_of(Project& project, const ScatterProfile& profile);

/**
 * @brief Adds <code>object</code> to profile <code>index</code>; returns whether there was room
 *
 * The object goes on the end of that profile's span, which moves the later profiles' objects along
 * so the table stays packed.
 */
bool add_scatter_object(Project& project, std::size_t index, const ScatterObject& object);

/// Removes object <code>slot</code> of profile <code>index</code>, keeping the table packed
void remove_scatter_object(Project& project, std::size_t index, std::size_t slot);

/**
 * @brief Drops every profile whose span does not pack the object table exactly
 *
 * What <code>validate_groups</code> is for a tileset's groups, and for the same reason: the table
 * is always written packed and in order, so anything else is a file that cannot say which objects
 * belong to which profile. Returns how many profiles were dropped. The manifest loader builds the
 * table through <code>add_scatter_object</code> and so cannot produce one that fails this; it is
 * called after a load as the backstop that keeps that true.
 */
std::size_t validate_scatter(Project& project);

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

/// Appends a tileset; returns its id
level::TileSetId add_tileset(Project& project, std::string_view name);

/// Index of the rule set named <code>name</code>, or <code>kNoRules</code>
[[nodiscard]] std::size_t find_rules(const Project& project, std::string_view name);

/// Appends a rule set named <code>name</code>; returns its index
std::size_t add_rules(Project& project, std::string_view name);

/**
 * @brief Index of the rule set named <code>name</code>, adding it if there is none
 *
 * How a rule-set file is read in, so reading the same one twice does not leave two sets.
 *
 * A project's blank starting set is <b>not</b> taken over by the first set read in, which was
 * tried: "one empty set" cannot tell that set apart from one the user just added and has not filled
 * yet, so the second rules file read into a project would silently replace the first. The cost is a
 * project that has only ever had rules imported keeping an unused set, which the user can remove;
 * the cost of the other way round is losing a rule set without saying so.
 */
std::size_t intern_rules(Project& project, std::string_view name);

/**
 * @brief Index of the rule set <code>level</code> auto-tiles with
 *
 * The set it names, or the project's first when it names none or names one that has gone -- which
 * is what a level written before rule sets were plural resolves to. Never <code>kNoRules</code>,
 * since a project always holds at least one set.
 */
[[nodiscard]] std::size_t rules_of(const Project& project, const level::Level& level);

/**
 * @brief Removes a rule set; a project keeps at least one, so removing the last is a no-op
 *
 * Levels naming it fall back to the first set, as one naming a set that was never there does.
 */
void remove_rules(Project& project, std::size_t index);

/// Index of the tileset named <code>name</code>, or <code>tilesets.size()</code>
[[nodiscard]] std::size_t find_tileset(const Project& project, std::string_view name);

/**
 * @brief Removes a tileset and every reference to it in <code>level</code>
 *
 * Refused while the project has more than one member level: the others are not resident, so their
 * tile cells would keep an id that has moved. The resident level's references are cleared and
 * renumbered by <code>level::clear_tileset_refs</code>.
 */
Status remove_tileset(Project& project, level::Level& level, level::TileSetId id);

/// Index of the catalog entry named <code>name</code> (adding it if it is missing)
level::SemanticId intern_semantic(Project& project, std::string_view name);
level::MaterialId intern_material(Project& project, std::string_view name);

/**
 * @brief Removes a catalog entry; everything that used it becomes "none" (id 0)
 *
 * Remapped: <code>level</code>'s semantic/material cells, its tile metadata, its seasonal variants
 * (materials) and its WFC tiles (semantics); ids after the removed one shift down. Id 0 ("none")
 * can't be removed.
 *
 * <b>Only the level given is remapped.</b> A project's other levels are not resident, so their
 * cells keep an id that has moved. Until the catalogs stop renumbering, a project with more than
 * one level should not remove an entry.
 */
void remove_semantic(Project& project, level::Level& level, level::SemanticId id);
void remove_material(Project& project, level::Level& level, level::MaterialId id);

/**
 * @brief Where every catalog entry goes when the retired ones are dropped
 *
 * One entry per current id, in id order: its new id, or <code>kRetiredEntry</code> for one that is
 * going. A plan is made once and then applied to every level of the project, which is what makes
 * compaction all-or-nothing rather than a renumbering each level does for itself.
 */
inline constexpr std::uint16_t kRetiredEntry = 0xFFFFU;

struct CatalogPlan
{
  explicit CatalogPlan(memory::Arena& arena) :
      semantics{memory::ArenaAllocator<std::uint16_t>{arena}}, materials{memory::ArenaAllocator<std::uint16_t>{arena}}
  {}

  memory::Vector<std::uint16_t> semantics;
  memory::Vector<std::uint16_t> materials;
  /// How many entries of each the compaction would drop
  std::size_t retired_semantics = 0;
  std::size_t retired_materials = 0;
};

/// True if either catalog holds a retired entry, so there is anything to compact
[[nodiscard]] bool needs_compaction(const Project& project);

/// Fills <code>out</code> with where every entry would go (allocated from <code>out</code>'s arena)
void plan_compaction(const Project& project, CatalogPlan& out);

/**
 * @brief Renumbers one level's semantic and material cells by <code>plan</code>
 *
 * A cell holding a retired entry becomes "none". Every level of the project must go through this
 * with the same plan, including the resident one, before <code>finish_compaction</code> drops the
 * entries -- that is the whole point of planning first.
 */
void apply_compaction(const CatalogPlan& plan, level::Level& level);

/**
 * @brief Drops the retired entries and renumbers everything the project itself holds
 *
 * Tile metadata, seasonal variants and the rule sets' WFC tiles. Called last, once every level has
 * been through <code>apply_compaction</code>, because until then the plan's old ids are what the
 * project's own vectors are indexed by.
 */
void finish_compaction(Project& project, const CatalogPlan& plan);

/**
 * @brief How many cells, tiles, seasonal variants and WFC tiles of <code>level</code> use an entry
 *
 * Counts the one level given, for the same reason: the project's other levels are not loaded, so a
 * count of zero does not mean the entry is unused across the project.
 */
[[nodiscard]] std::size_t semantic_uses(const Project& project, const level::Level& level, level::SemanticId id);
[[nodiscard]] std::size_t material_uses(const Project& project, const level::Level& level, level::MaterialId id);

/// Index of the season named <code>name</code>, or <code>seasons.size()</code>
[[nodiscard]] level::SeasonId find_season(const Project& project, std::string_view name);

/**
 * @brief Removes a season (a project keeps at least one)
 *
 * Tile variants for it stay in their tilesets, which key them on the season's name, so re-adding a
 * season of that name brings its variants back.
 */
void remove_season(Project& project, level::SeasonId id);

/// Adds a season named <code>name</code>; returns its index (at most 255 seasons)
level::SeasonId add_season(Project& project, std::string_view name);

}  // namespace lvl::project
