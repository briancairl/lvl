/**
 * @copyright 2026-present Brian Cairl
 *
 * @file jobs.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <new>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/editor/jobs.hpp"
#include "lvl/level/objects.hpp"
#include "lvl/project/paths.hpp"

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
  result = level::save_level(path.c_str(), *project, *level, ctx, scope);
}

void LoadLevelJob::run(jobs::Progress& progress)
{
  auto ctx = memory->context(&progress);
  result = level::load_level(path.c_str(), *project, *level, ctx);
  if (result.ok and decode_atlases)
  {
    // Worker arenas are free again: reuse them for the atlas images
    load_atlases(*project, *assets, *memory, atlases, &progress);
  }
}

void RescanProjectJob::run(jobs::Progress& progress)
{
  const project::Project& pr = *project;
  member_count = std::min(pr.levels.size(), kMaxJobLevels);
  progress.reset(member_count == 0 ? 1 : member_count);
  // The names every member's spans point into. One block out of the general arena, which the job
  // owns until finish_job has copied them into the project.
  auto table = memory::make_vector<Name>(memory->general);
  table.reserve(project::kMaxObjectNames);

  for (std::size_t i = 0; i < member_count; ++i)
  {
    const project::LevelEntry& entry = pr.levels[i];
    progress.stage(entry.name.c_str());
    Path absolute;
    Status located = project::resolve(pr.root.view(), entry.file.view(), absolute);
    if (located.ok)
    {
      level::reset(*scratch);
      project::reset(*scratch_project);
      auto ctx = memory->context(nullptr);
      located = level::load_level(absolute.c_str(), *scratch_project, *scratch, ctx);
    }
    if (!located.ok)
    {
      ++failed;
      if (result.ok)
      {
        result = located;
      }
      progress.advance();
      continue;
    }
    Member& out = members[i];
    out.read = true;
    out.extents = scratch->settings.extents;
    out.portals_begin = static_cast<std::uint32_t>(table.size());
    for (auto [e, label, portal] : scratch->registry.view<level::Label, level::Portal>().each())
    {
      table.push_back(label.name);
    }
    out.portal_count = static_cast<std::uint32_t>(table.size() - out.portals_begin);
    out.markers_begin = static_cast<std::uint32_t>(table.size());
    for (auto [e, label, marker] : scratch->registry.view<level::Label, level::Marker>().each())
    {
      table.push_back(label.name);
    }
    out.marker_count = static_cast<std::uint32_t>(table.size() - out.markers_begin);
    if (table.size() > project::kMaxObjectNames)
    {
      result = Status::error(
        "the project's levels hold more than %zu portals and markers between them", project::kMaxObjectNames);
      failed += member_count - i;
      break;
    }
    progress.advance();
  }
  names = table.data();
  name_count = table.size();
  // Detach without releasing the block: finish_job reads it after the job is done
  new (&table) memory::Vector<Name>{memory::ArenaAllocator<Name>{}};
  if (member_count == 0)
  {
    progress.advance();
  }
}

void CompactCatalogsJob::run(jobs::Progress& progress)
{
  const project::Project& pr = *project;
  // Planned once, here, so that the members rewritten below and the open level that finish_job
  // rewrites afterwards move by the same table
  plan = new (plan_storage) project::CatalogPlan{memory->general};
  project::plan_compaction(pr, *plan);
  progress.reset(pr.levels.empty() ? 1 : pr.levels.size());
  for (const project::LevelEntry& entry : pr.levels)
  {
    if (entry.name == skip)
    {
      // The resident document: the UI thread applies the plan to it, since a job may not touch it
      progress.advance();
      continue;
    }
    progress.stage(entry.name.c_str());
    stopped_on = entry.file;
    Path absolute;
    if (Status s = project::resolve(pr.root.view(), entry.file.view(), absolute); !s.ok)
    {
      result = s;
      return;
    }
    level::reset(*scratch);
    project::reset(*scratch_project);
    auto ctx = memory->context(nullptr);
    if (Status s = level::load_level(absolute.c_str(), *scratch_project, *scratch, ctx); !s.ok)
    {
      result = s;
      return;
    }
    project::apply_compaction(*plan, *scratch);
    // Written back in project scope, whatever it was read in. The plan is the project's, so the
    // cells now hold the project's new ids; a file that carried its own copy of the catalogs would
    // be left naming the old ones beside them. The manifest is the authority for a member, which is
    // what kProject exists for.
    if (Status s = level::save_level(absolute.c_str(), *scratch_project, *scratch, ctx, level::LevelScope::kProject);
        !s.ok)
    {
      result = s;
      return;
    }
    ++rewritten;
    progress.advance();
  }
  stopped_on.clear();
  if (pr.levels.empty())
  {
    progress.advance();
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
  if (layer != static_cast<std::size_t>(-1))
  {
    progress.reset(1);
    progress.stage("Writing the layer");
    result = (layer < level->layers.size())
      ? render::export_layer_png(
          path.c_str(), *project, level->layers[layer], &atlases, options.season, memory->general)
      : Status::error("the layer was removed before the export ran");
    progress.advance();
  }
  else if (minimap)
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
