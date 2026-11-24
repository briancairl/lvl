/**
 * @copyright 2026-present Brian Cairl
 *
 * @file jobs.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/editor/jobs.hpp"

namespace lvl::editor
{

void partition(memory::Arena& arena, JobMemory& out)
{
  memory::clear(arena);
  out = JobMemory{};
  out.worker_count = std::clamp<std::size_t>(jobs::hardware_concurrency(), 1, kMaxWorkers);
  // Half for the general arena (job results, images, the buffers level I/O sizes by the level);
  // the rest split between workers
  const std::size_t total = memory::available(arena);
  memory::push(arena, out.general, total / 2, "job-general");
  const std::size_t per_worker = (memory::available(arena) - 4096) / out.worker_count;
  for (std::size_t w = 0; w < out.worker_count; ++w)
  {
    memory::push(arena, out.workers[w].json, per_worker / 4, "job-json");
    memory::push(arena, out.workers[w].scratch, per_worker - per_worker / 4 - 256, "job-scratch");
  }
}

void load_atlases(
  const project::Project& project,
  memory::Arena& dest,
  JobMemory& memory,
  AtlasImages& out,
  jobs::Progress* progress)
{
  constexpr std::size_t kMiB = 1UL << 20;
  constexpr std::size_t kSlack = 64;
  out.count = std::min(project.tilesets.size(), kMaxJobTileSets);
  if (progress != nullptr)
  {
    progress->stage("Decoding atlas images");
    progress->add_total(out.count);
  }
  memory::clear(dest);
  memory::Arena& general = memory.general;
  memory::clear(general);

  // 1. Headers: how much destination and scratch memory each atlas needs
  std::array<image::Info, kMaxJobTileSets> info = {};
  std::array<bool, kMaxJobTileSets> pending = {};
  for (std::size_t i = 0; i < out.count; ++i)
  {
    const level::TileSet& ts = project.tilesets[i];
    out.images[i] = image::Image{};
    out.status[i] = Status::okay();
    if (ts.image.empty())
    {
      out.status[i] = Status::error("no atlas image set");
      continue;
    }
    out.status[i] = image::probe(ts.image.c_str(), memory.workers[0].scratch, info[i]);
    pending[i] = out.status[i].ok;
  }

  // 2. Destination pixels, reserved up front in the asset arena (decoding can't allocate from it
  //    concurrently)
  std::array<void*, kMaxJobTileSets> slots = {};
  for (std::size_t i = 0; i < out.count; ++i)
  {
    if (!pending[i])
    {
      continue;
    }
    const std::size_t bytes = info[i].pixel_bytes() + kSlack;
    if (bytes > memory::available(dest))
    {
      out.status[i] = Status::error(
        "atlas '%s' (%dx%d, %zu MiB) does not fit the remaining %zu MiB of asset memory (raise --memory)",
        project.tilesets[i].image.c_str(),
        info[i].width,
        info[i].height,
        (bytes + kMiB - 1) / kMiB,
        memory::available(dest) / kMiB);
      pending[i] = false;
      continue;
    }
    slots[i] = memory::allocate_bytes(dest, bytes, alignof(image::Rgba));
  }

  // Atlases that already failed count as done
  for (std::size_t i = 0; i < out.count; ++i)
  {
    if (!pending[i] and progress != nullptr)
    {
      progress->advance();
    }
  }

  // 3. Scratch: a sub-arena sized for each atlas, carved from the job's general arena, for as many as
  //    fit side by side; those decode in parallel. The rest decode one at a time with all of it.
  std::array<memory::Arena, kMaxJobTileSets> scratch = {};
  std::array<std::size_t, kMaxJobTileSets> batch = {};
  std::size_t batch_size = 0;
  for (std::size_t i = 0; i < out.count; ++i)
  {
    // Keep room for a sub-arena's alignment padding
    if (pending[i] and info[i].scratch_bytes + 128 <= memory::available(general))
    {
      memory::push(general, scratch[i], info[i].scratch_bytes, "atlas-scratch");
      batch[batch_size++] = i;
    }
  }
  const auto decode = [&](std::size_t i, memory::Arena& work) {
    // The reserved slot, as an arena the decoder allocates the pixels from
    memory::Arena slot;
    memory::initialize(slot, slots[i], info[i].pixel_bytes() + kSlack, "atlas-pixels");
    out.status[i] = image::load(project.tilesets[i].image.c_str(), slot, work, out.images[i]);
    pending[i] = false;
    if (progress != nullptr)
    {
      progress->advance();
    }
  };
  jobs::parallel_for(
    batch_size, memory.worker_count, [&](std::size_t k, std::size_t) { decode(batch[k], scratch[batch[k]]); });
  for (std::size_t k = batch_size; k-- > 0;)
  {
    memory::pop(general, scratch[batch[k]]);
  }
  for (std::size_t i = 0; i < out.count; ++i)
  {
    if (pending[i])
    {
      // Too large to share: the whole general arena (image::load reports it if even that's too small)
      decode(i, general);
    }
  }
  for (std::size_t i = 0; i < out.count; ++i)
  {
    if (!out.status[i].ok)
    {
      out.images[i] = image::Image{};
      LVL_LOG_WARN() << "Tileset '" << project.tilesets[i].name << "': " << out.status[i].message;
    }
  }
}

void SaveLevelJob::run(jobs::Progress& progress)
{
  auto ctx = memory->context(&progress);
  result = level::save_level(path.c_str(), *project, *level, ctx);
}

void LoadLevelJob::run(jobs::Progress& progress)
{
  auto ctx = memory->context(&progress);
  result = level::load_level(path.c_str(), *project, *level, ctx);
  if (result.ok)
  {
    // Worker arenas are free again: reuse them for the atlas images
    load_atlases(*project, *assets, *memory, atlases, &progress);
  }
}

void ReloadAtlasesJob::run(jobs::Progress& progress)
{
  progress.reset(1);
  load_atlases(*project, *assets, *memory, atlases, &progress);
  progress.advance();
}

void ExportJob::run(jobs::Progress& progress)
{
  if (minimap)
  {
    progress.reset(1);
    progress.stage("Rendering mini-preview");
    result = render::export_minimap_png(path.c_str(), *project, *level, atlases, options, max_size, memory->general);
    progress.advance();
  }
  else
  {
    result = render::export_png(
      path.c_str(), *project, *level, atlases, options, memory->general, memory->worker_count, &progress);
  }
}

void HeightmapJob::run(jobs::Progress& progress)
{
  height = level::Grid<float>{memory->general};
  height.assign(extents, 0.0F);
  image::Image img;
  if (!seed_image.empty() and params.seed_weight > 0.0F)
  {
    progress.stage("Loading seed image");
    if (Status s = image::load(seed_image.c_str(), memory->general, img); !s.ok)
    {
      result = s;
      return;
    }
  }
  procgen::generate_heightmap(params, seed, height, img.empty() ? nullptr : &img, memory->worker_count, &progress);
  result = Status::okay();
}

void WatershedJob::run(jobs::Progress& progress)
{
  const procgen::WatershedOutputs outputs{
    .water = &water,
    .semantic = semantics ? &semantic : nullptr,
    .river_id = river_id,
    .lake_id = lake_id,
    .tunnel_id = tunnel_id,
  };
  summary = procgen::run_watershed(height, outputs, params, seed, memory->workers[0].scratch, &progress);
}

void WfcJob::run(jobs::Progress& progress)
{
  constraints.existing = params.keep_existing ? &tiles : nullptr;
  // The solver reads `existing` while writing `tiles`: give it a separate copy of the input
  level::Grid<level::TileRef> existing{memory->general};
  if (constraints.existing != nullptr)
  {
    existing.extents = tiles.extents;
    existing.values.assign(tiles.values.begin(), tiles.values.end());
    constraints.existing = &existing;
  }
  result = procgen::solve(project->rules[rules].wfc, constraints, params, tiles, memory->workers[0].scratch, &progress);
}

}  // namespace lvl::editor
