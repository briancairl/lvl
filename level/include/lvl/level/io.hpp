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

namespace lvl::level
{

enum class FileFormat
{
  kJson,
  kBinary,
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

/// Saves a whole level
Status save_level(const char* path, const Level& level, IoContext& ctx);

/**
 * @brief Loads a whole level into <code>level</code> (which is reset first)
 *
 * On failure <code>level</code> holds a partial document; load into a spare Level and swap on success.
 */
Status load_level(const char* path, Level& level, IoContext& ctx);

/// Saves tileset <code>id</code> (with the catalog entries it references)
Status save_tileset(const char* path, const Level& level, TileSetId id, IoContext& ctx);

/// Loads a tileset file and appends it to <code>level</code> (interning its catalog entries)
Status load_tileset(const char* path, Level& level, IoContext& ctx, TileSetId& id);

/// Saves the level's WFC rules
Status save_wfc_rules(const char* path, const Level& level, IoContext& ctx);

/// Replaces the level's WFC rules with a file's (its tilesets must exist in the level, by name)
Status load_wfc_rules(const char* path, Level& level, IoContext& ctx);

/// Saves the level's path graphs (graphs, nodes and edges)
Status save_paths(const char* path, const Level& level, IoContext& ctx);

/// Replaces the level's path graphs with a file's
Status load_paths(const char* path, Level& level, IoContext& ctx);

}  // namespace lvl::level
