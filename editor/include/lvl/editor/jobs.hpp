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
#include "lvl/render/compose.hpp"

namespace lvl::editor
{

inline constexpr std::size_t kMaxWorkers = 8;
inline constexpr std::size_t kMaxJobTileSets = 64;

/// Memory for one job: per-worker arenas (for level I/O) plus one general arena
struct JobMemory
{
  std::array<level::IoWorker, kMaxWorkers> workers;
  std::size_t worker_count = 0;
  memory::Arena general;

  [[nodiscard]] level::IoContext context(jobs::Progress* progress)
  {
    return level::IoContext{.workers = workers.data(), .worker_count = worker_count, .progress = progress};
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
  const level::Level& level,
  memory::Arena& dest,
  JobMemory& memory,
  AtlasImages& out,
  jobs::Progress* progress);

struct SaveLevelJob
{
  const level::Level* level = nullptr;
  Path path;
  JobMemory* memory = nullptr;
  Status result;

  void run(jobs::Progress& progress);
};

struct LoadLevelJob
{
  /// Loaded into (the spare document)
  level::Level* level = nullptr;
  /// Atlases decoded into (the spare asset arena)
  memory::Arena* assets = nullptr;
  Path path;
  JobMemory* memory = nullptr;
  AtlasImages atlases;
  Status result;

  void run(jobs::Progress& progress);
};

struct ReloadAtlasesJob
{
  const level::Level* level = nullptr;
  memory::Arena* assets = nullptr;
  JobMemory* memory = nullptr;
  AtlasImages atlases;

  void run(jobs::Progress& progress);
};

struct ExportJob
{
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
  const level::Level* level = nullptr;
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
  jobs::Background<WfcJob>>;

}  // namespace lvl::editor
