/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io.hpp
 *
 * Saving/loading levels and their standalone parts (tilesets, WFC rules, path graphs) as JSON (the
 * human-readable source of truth) or binary (a compact translation of the same data). The format is
 * picked from the file extension: ".lvlb"/".bin" for binary, anything else is JSON.
 *
 * Level files are saved/loaded in parallel: each layer's data plane is encoded/decoded by a worker
 * with its own JSON and scratch arenas, while the rest of the document is handled on the calling
 * thread. In JSON, layers are written last, so the loader can find each layer's text and parse the
 * layers concurrently; in binary, each layer is a length-prefixed chunk.
 *
 * In JSON, catalog entries, tilesets and seasons are referenced by name (so standalone files can be
 * moved between levels); tile cells are numbers: -1 for empty, else <code>tileset * 65536 + tile</code>.
 */
#pragma once

// C++ Standard Library
#include <cstddef>

// LVL
#include "lvl/core/jobs.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"
#include "lvl/level/level.hpp"
#include "lvl/project/project.hpp"

namespace lvl::level
{

enum class FileFormat
{
  kJson,
  kBinary,
};

/**
 * @brief How much of what a level draws on is written into its own file
 *
 * A level references its project's catalogs, seasons, tilesets and rule sets by id, so a file has to
 * say which of them it carries.
 *
 * - <code>kStandalone</code> writes them all, so the file reads back on its own, into a project it
 *   fills in as it goes. This is what <code>//tools:lvl_convert</code> emits and what exporting a
 *   level writes, and it is the default: a file handed to someone else should work.
 * - <code>kProject</code> writes only what the level itself owns -- its settings, path graphs,
 *   objects and layers. The project's manifest carries the rest, so the file is smaller and a
 *   project's levels cannot drift apart. Loading one needs a project that already holds the shared
 *   data; its cells are read as the project ids they are.
 *
 * In binary the two are different file kinds (<code>binary::Kind::kLevel</code> and
 * <code>kProjectLevel</code>), so a reader knows which it has before decoding.
 */
enum class LevelScope
{
  kStandalone,
  kProject,
};

/// Format implied by <code>path</code>'s extension
FileFormat format_of(const char* path);

/// Per-thread memory for (de)serialization work
struct IoWorker
{
  /// Installed as the thread's JSON arena while the worker runs
  memory::Arena json;
  /// File buffers, encoded layer text/bytes
  memory::Arena scratch;
};

/// Where (de)serialization gets its working memory and reports progress
struct IoContext
{
  /// At least one worker; worker 0 is used by the calling thread
  IoWorker* workers = nullptr;
  std::size_t worker_count = 0;
  /**
   * @brief Optional arena for the buffers whose size follows the level's, not the worker count's
   *
   * A whole file's bytes when loading, and a layer's encoded text when saving. It is shared by the
   * workers but carved up only by the calling thread, so bump allocation stays single-threaded.
   * Worker 0's scratch is used when none is given, which caps a level at one worker's share.
   */
  memory::Arena* shared = nullptr;
  /// Optional
  jobs::Progress* progress = nullptr;
};

/**
 * @brief Saves a whole level
 *
 * The catalogs, seasons, tilesets and rule sets a level draws on belong to its
 * <code>project</code>, not to the level. <code>scope</code> says whether they are written into the
 * level's own file as well.
 */
Status save_level(
  const char* path,
  const project::Project& project,
  const Level& level,
  IoContext& ctx,
  LevelScope scope = LevelScope::kStandalone);

/**
 * @brief Loads a whole level into <code>level</code> (which is reset first)
 *
 * Either scope is accepted: a file that carries the shared sections replaces the project's with
 * them, and one that does not leaves the project alone.
 *
 * On failure <code>level</code> holds a partial document, and <code>project</code> may hold
 * entries interned from the file; load into a spare of each and swap both on success.
 */
Status load_level(const char* path, project::Project& project, Level& level, IoContext& ctx);

/// Saves tileset <code>id</code> (with the project catalog entries it references)
Status save_tileset(const char* path, const project::Project& project, TileSetId id, IoContext& ctx);

/// Loads a tileset file and appends it to <code>level</code> (interning its catalog entries into
/// <code>project</code>)
Status load_tileset(const char* path, project::Project& project, IoContext& ctx, TileSetId& id);

/// Saves rule set <code>index</code> (WFC, auto-tiling and height steps together)
Status save_wfc_rules(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);

/**
 * @brief Reads a rule-set file into the project, and says which set it landed in
 *
 * The file names the set it holds, so it replaces the project's set of that name or becomes a new
 * one -- reading the same file twice does not leave two. Its tilesets must already be in the
 * project, by name.
 */
Status load_wfc_rules(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);

/// Saves scatter profile <code>index</code> (with the catalog entry its ground type names)
Status save_scatter(const char* path, const project::Project& project, std::size_t index, IoContext& ctx);

/**
 * @brief Reads a scatter-profile file into the project, and says which profile it landed in
 *
 * The file names the profile it holds, so it replaces the project's profile of that name or becomes
 * a new one -- reading the same file twice does not leave two. Its objects name their tileset and
 * group rather than indexing them, so the file needs no tileset to be in the project already; an
 * object naming one that is not there is kept as written and shows as dangling.
 */
Status load_scatter(const char* path, project::Project& project, IoContext& ctx, std::size_t& index);

/// Saves the level's path graphs (graphs, nodes and edges)
Status save_paths(const char* path, const Level& level, IoContext& ctx);

/// Replaces the level's path graphs with a file's
Status load_paths(const char* path, Level& level, IoContext& ctx);

}  // namespace lvl::level
