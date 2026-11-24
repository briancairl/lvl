/**
 * @copyright 2026-present Brian Cairl
 *
 * @file jobs.hpp
 *
 * The editor's background jobs. Each runs off the UI thread (in parallel internally where it
 * helps) while a modal progress dialog blocks editing, so it may read the document without locks.
 * Results are applied to the document on the UI thread once the job is done (see app.cpp).
 *
 * Every job gets its working memory from the jobs arena, split per worker when the job starts and
 * reclaimed wholesale when it has been applied.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstddef>
#include <variant>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"
#include "lvl/level/io.hpp"
#include "lvl/level/level.hpp"
#include "lvl/procgen/noise.hpp"
#include "lvl/procgen/watershed.hpp"
#include "lvl/procgen/wfc.hpp"
#include "lvl/project/project.hpp"
#include "lvl/render/compose.hpp"

namespace lvl::editor
{

inline constexpr std::size_t kMaxWorkers = 8;
inline constexpr std::size_t kMaxJobTileSets = 64;
/// Member levels one job can work through; the project's own limit is project::kMaxLevels
inline constexpr std::size_t kMaxJobLevels = 256;

/// Memory for one job: per-worker arenas (for level I/O) plus one general arena
struct JobMemory
{
  std::array<level::IoWorker, kMaxWorkers> workers;
  std::size_t worker_count = 0;
  memory::Arena general;

  /**
   * @brief An IoContext over this memory, with the general arena shared by the workers
   *
   * Level I/O and the general arena's other users (atlas decoding, generator results) are separate
   * phases of a job, never concurrent: a load decodes atlases only once the level is in, and
   * load_atlases clears the arena first.
   */
  [[nodiscard]] level::IoContext context(jobs::Progress* progress)
  {
    return level::IoContext{
      .workers = workers.data(), .worker_count = worker_count, .shared = &general, .progress = progress};
  }
};

/// Splits <code>arena</code> (cleared first) into a JobMemory
void partition(memory::Arena& arena, JobMemory& out);

/// Decoded atlas images for a set of tilesets
struct AtlasImages
{
  std::array<image::Image, kMaxJobTileSets> images = {};
  std::array<Status, kMaxJobTileSets> status = {};
  std::size_t count = 0;
};

/// Decodes every tileset's atlas into <code>dest</code> (in parallel); missing images are reported per tileset
void load_atlases(
  const project::Project& project,
  memory::Arena& dest,
  JobMemory& memory,
  AtlasImages& out,
  jobs::Progress* progress);

struct SaveLevelJob
{
  const project::Project* project = nullptr;
  const level::Level* level = nullptr;
  Path path;
  /**
   * @brief How much of what the level draws on goes into the file
   *
   * A member of a project on disk is written in project scope, so the manifest stays the one
   * authority for the catalogs, seasons, tilesets and rules its cells name and the project's levels
   * cannot drift apart. A level with no project folder is written standalone, which is what a level
   * file has always been and what a file handed to someone else has to be.
   */
  level::LevelScope scope = level::LevelScope::kStandalone;
  JobMemory* memory = nullptr;
  Status result;

  void run(jobs::Progress& progress);
};

struct LoadLevelJob
{
  /// Interned into: the spare project, or the open one when switching within a project
  project::Project* project = nullptr;
  /// Loaded into (the spare document)
  level::Level* level = nullptr;
  /// Atlases decoded into (the spare asset arena); unused when keep_project is set
  memory::Arena* assets = nullptr;
  Path path;
  /**
   * @brief Switching to another level of the open project, rather than opening a level
   *
   * The file is a member, so it carries none of the shared sections and the project it is read into
   * is left as it is -- which is why <code>project</code> is the open one and
   * <code>finish_job</code> does not swap it. The tilesets are therefore the same objects they
   * were, so the atlases are not decoded again either: not re-decoding them on every switch is
   * most of the point of the project owning them.
   */
  bool keep_project = false;
  JobMemory* memory = nullptr;
  AtlasImages atlases;
  Status result;

  void run(jobs::Progress& progress);
};

/**
 * @brief Rebuilds the project's index by reading every member level
 *
 * What the index says about a level is only as fresh as the last time this editor saved it, so a
 * project whose files have been changed elsewhere -- by a merge, or by another editor -- needs them
 * read again. Each member is loaded in turn and then thrown away, since all that is wanted is its
 * grid size and its portal and marker names: only one level is ever resident, and that stays true
 * here. Reading the whole level to learn so little is wasteful, and a cheaper "headers only" read
 * is possible in both formats -- the loaders already parse everything but the cells as their first
 * step -- but it would be new I/O surface, so it is left for when a rescan is slow enough to matter.
 *
 * The results are a flat <code>LevelObjects</code>-shaped span per member, which
 * <code>finish_job</code> writes into the project on the UI thread. The open project is read, never
 * written, while the job runs.
 *
 * It reads each member with a plain <code>load_level</code> into the <b>spare</b> document and
 * project -- the staging pair a load already uses -- rather than carving a document out of the job
 * arena. They are the right size by construction, which a fraction of the job arena is not: a level
 * that fits a document arena would not be guaranteed to fit, and exhausting an arena is an assert,
 * not a <code>Status</code>. Only one job runs at a time, so nothing else is staging into them.
 */
struct RescanProjectJob
{
  const project::Project* project = nullptr;
  /// The spare document and project, which each member is read into in turn and then reset
  level::Level* scratch = nullptr;
  project::Project* scratch_project = nullptr;
  JobMemory* memory = nullptr;
  /// One per member, in index order: what that level turned out to hold
  struct Member
  {
    level::Extents extents = {0, 0};
    std::uint32_t portals_begin = 0;
    std::uint32_t portal_count = 0;
    std::uint32_t markers_begin = 0;
    std::uint32_t marker_count = 0;
    bool read = false;
  };
  std::array<Member, kMaxJobLevels> members = {};
  std::size_t member_count = 0;
  /// The names the spans point into, in the job's general arena
  Name* names = nullptr;
  std::size_t name_count = 0;
  /// How many members could not be read, and the first reason why
  std::size_t failed = 0;
  Status result;

  void run(jobs::Progress& progress);
};

/**
 * @brief Renumbers the catalogs of every member level, so the retired entries can be dropped
 *
 * The one thing that moves a catalog id (see project::plan_compaction). Every member but the open
 * one is loaded, rewritten and saved here, through the spare document and project for the same
 * reason as the rescan above; the open one is done on the UI thread, because it is the document and
 * a job may not touch it. If a member cannot be read or written the job stops and says which, and
 * the entries are not dropped -- so the ids on disk and the ids in the project still agree.
 *
 * A member is written back in <b>project scope</b>, whatever it was read in: the plan is the
 * project's, so the cells come out holding the project's new ids, and a file that carried its own
 * copy of the catalogs would be left naming the old ones beside them. The manifest is the authority
 * for a member, which is what kProject is for.
 */
struct CompactCatalogsJob
{
  const project::Project* project = nullptr;
  /**
   * @brief Where every catalog entry goes, built by the job from the project's retired flags
   *
   * In the job rather than passed in, because the same plan has to rewrite the members here and the
   * open level in <code>finish_job</code>, and a plan built on the UI thread would have to live
   * somewhere until then -- the frame arena is cleared every frame. Its vectors come from the job's
   * general arena, which outlives the job itself; the object is placement-new'd into
   * <code>plan_storage</code> and never destroyed, since the arena is reclaimed wholesale.
   */
  project::CatalogPlan* plan = nullptr;
  alignas(project::CatalogPlan) unsigned char plan_storage[sizeof(project::CatalogPlan)] = {};
  /// Skipped, because it is the resident document: the UI thread applies the plan to it
  Name skip;
  /// The spare document and project, as the rescan uses them
  level::Level* scratch = nullptr;
  project::Project* scratch_project = nullptr;
  JobMemory* memory = nullptr;
  /// How many members were rewritten, and where it stopped if it did
  std::size_t rewritten = 0;
  Path stopped_on;
  Status result;

  void run(jobs::Progress& progress);
};

struct ReloadAtlasesJob
{
  /// Holds the tilesets whose atlases are decoded
  const project::Project* project = nullptr;
  memory::Arena* assets = nullptr;
  JobMemory* memory = nullptr;
  AtlasImages atlases;

  void run(jobs::Progress& progress);
};

struct ExportJob
{
  const project::Project* project = nullptr;
  const level::Level* level = nullptr;
  render::Atlases atlases;
  render::ComposeOptions options;
  /// Mini-preview instead of the full map
  bool minimap = false;
  int max_size = 256;
  Path path;
  JobMemory* memory = nullptr;
  Status result;

  void run(jobs::Progress& progress);
};

struct HeightmapJob
{
  procgen::HeightmapParams params;
  std::uint64_t seed = 1;
  level::Extents extents;
  Path seed_image;
  JobMemory* memory = nullptr;
  /// Result (in the job's general arena)
  level::Grid<float> height;
  Status result;

  void run(jobs::Progress& progress);
};

struct WatershedJob
{
  procgen::WatershedParams params;
  std::uint64_t seed = 1;
  /// Inputs copied in, results written back by the UI thread (in the job's general arena)
  level::Grid<float> height;
  level::Grid<float> water;
  level::Grid<std::uint16_t> semantic;
  bool semantics = false;
  std::uint16_t river_id = 0;
  std::uint16_t lake_id = 0;
  std::uint16_t tunnel_id = 0;
  JobMemory* memory = nullptr;
  procgen::WatershedResult summary;

  void run(jobs::Progress& progress);
};

struct WfcJob
{
  /// Holds the adjacency rules the solver works to
  const project::Project* project = nullptr;
  /// Which of the project's rule sets, taken from the open level when the job is prepared
  std::size_t rules = 0;
  procgen::WfcConstraints constraints;
  procgen::WfcParams params;
  /// Target layer's tiles (copied in; written back by the UI thread)
  level::Grid<level::TileRef> tiles;
  std::size_t layer = 0;
  JobMemory* memory = nullptr;
  procgen::WfcResult result;

  void run(jobs::Progress& progress);
};

/// The (at most one) running job
using JobSlot = std::variant<
  std::monostate,
  jobs::Background<SaveLevelJob>,
  jobs::Background<LoadLevelJob>,
  jobs::Background<ReloadAtlasesJob>,
  jobs::Background<ExportJob>,
  jobs::Background<HeightmapJob>,
  jobs::Background<WatershedJob>,
  jobs::Background<WfcJob>,
  jobs::Background<RescanProjectJob>,
  jobs::Background<CompactCatalogsJob>>;

}  // namespace lvl::editor
