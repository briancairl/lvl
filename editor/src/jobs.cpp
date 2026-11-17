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
  // Half for the general arena (job results, images); the rest split between workers
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
  const level::Level& level,
  memory::Arena& dest,
  JobMemory& memory,
  AtlasImages& out,
  jobs::Progress* progress)
{
  out.count = std::min(level.tilesets.size(), kMaxJobTileSets);
  if (progress != nullptr)
  {
    progress->stage("Decoding atlas images");
    progress->add_total(out.count);
  }
  // Decode in parallel into each worker's scratch...
  jobs::parallel_for(out.count, memory.worker_count, [&](std::size_t i, std::size_t w) {
    const level::TileSet& ts = level.tilesets[i];
    out.images[i] = image::Image{};
    if (ts.image.empty())
    {
      out.status[i] = Status::error("no atlas image set");
    }
    else
    {
      memory::Arena& scratch = memory.workers[w].scratch;
      memory::Arena& keep = memory.workers[w].json;
      out.status[i] = image::load(ts.image.c_str(), keep, scratch, out.images[i]);
    }
    if (progress != nullptr)
    {
      progress->advance();
    }
  });
  // ... then copy into the destination asset arena on this thread
  memory::clear(dest);
  for (std::size_t i = 0; i < out.count; ++i)
  {
    if (!out.status[i].ok or out.images[i].empty())
    {
      continue;
    }
    if (out.images[i].size() * sizeof(image::Rgba) + 64 <= memory::available(dest))
    {
      out.images[i].pixels = memory::duplicate(dest, out.images[i].pixels, out.images[i].size());
    }
    else
    {
      out.status[i] = Status::error("atlas does not fit the asset memory");
      out.images[i] = image::Image{};
    }
  }
}

void SaveLevelJob::run(jobs::Progress& progress)
{
  auto ctx = memory->context(&progress);
  result = level::save_level(path.c_str(), *level, ctx);
}

void LoadLevelJob::run(jobs::Progress& progress)
{
  auto ctx = memory->context(&progress);
  result = level::load_level(path.c_str(), *level, ctx);
  if (result.ok)
  {
    // Worker arenas are free again: reuse them for the atlas images
    load_atlases(*level, *assets, *memory, atlases, &progress);
  }
}

void ReloadAtlasesJob::run(jobs::Progress& progress)
{
  progress.reset(1);
  load_atlases(*level, *assets, *memory, atlases, &progress);
  progress.advance();
}

void ExportJob::run(jobs::Progress& progress)
{
  if (minimap)
  {
    progress.reset(1);
    progress.stage("Rendering mini-preview");
    result = render::export_minimap_png(path.c_str(), *level, atlases, options, max_size, memory->general);
    progress.advance();
  }
  else
  {
    result =
      render::export_png(path.c_str(), *level, atlases, options, memory->general, memory->worker_count, &progress);
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
    if (Status s = image::load(seed_image.c_str(), memory->general, memory->workers[0].scratch, img); !s.ok)
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
  result = procgen::solve(level->wfc, constraints, params, tiles, memory->workers[0].scratch, &progress);
}

}  // namespace lvl::editor
