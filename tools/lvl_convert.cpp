/**
 * @copyright 2026-present Brian Cairl
 *
 * @file lvl_convert.cpp
 *
 * Headless level tool:
 *
 *   lvl_convert <in> <out>                 convert a level between JSON and binary (by extension)
 *   lvl_convert <in> --info                print a summary
 *   lvl_convert <in> --png <out.png> [--ppc N] [--season NAME] [--objects] [--no-overlays]
 *   lvl_convert <in> --minimap <out.png> [--size N] [--season NAME]
 *   [--memory=MiB] [--workers=N]
 *
 * Every binary file has a JSON translation (and vice versa) through this tool.
 */

// C++ Standard Library
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/io.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/project/project.hpp"
#include "lvl/render/compose.hpp"

namespace
{

using namespace lvl;

struct Options
{
  const char* input = nullptr;
  const char* output = nullptr;
  const char* png = nullptr;
  const char* minimap = nullptr;
  const char* season = "";
  bool info = false;
  bool objects = false;
  bool overlays = true;
  int ppc = 0;
  int size = 256;
  std::size_t memory_mib = 1024;
  std::size_t workers = 0;
};

int usage()
{
  std::fprintf(
    stderr,
    "usage: lvl_convert <in> <out>\n"
    "       lvl_convert <in> --info\n"
    "       lvl_convert <in> --png <out.png> [--ppc N] [--season NAME] [--objects] [--no-overlays]\n"
    "       lvl_convert <in> --minimap <out.png> [--size N] [--season NAME]\n"
    "       options: [--memory=MiB] [--workers=N]\n");
  return EXIT_FAILURE;
}

bool parse(int argc, char** argv, Options& o)
{
  for (int i = 1; i < argc; ++i)
  {
    const char* a = argv[i];
    const auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (std::strcmp(a, "--info") == 0)
    {
      o.info = true;
    }
    else if (std::strcmp(a, "--png") == 0)
    {
      o.png = next();
    }
    else if (std::strcmp(a, "--minimap") == 0)
    {
      o.minimap = next();
    }
    else if (std::strcmp(a, "--season") == 0)
    {
      o.season = next();
    }
    else if (std::strcmp(a, "--ppc") == 0)
    {
      const char* v = next();
      o.ppc = (v != nullptr) ? std::atoi(v) : 0;
    }
    else if (std::strcmp(a, "--size") == 0)
    {
      const char* v = next();
      o.size = (v != nullptr) ? std::atoi(v) : 256;
    }
    else if (std::strcmp(a, "--objects") == 0)
    {
      o.objects = true;
    }
    else if (std::strcmp(a, "--no-overlays") == 0)
    {
      o.overlays = false;
    }
    else if (std::strncmp(a, "--memory=", 9) == 0)
    {
      o.memory_mib = static_cast<std::size_t>(std::strtoull(a + 9, nullptr, 10));
    }
    else if (std::strncmp(a, "--workers=", 10) == 0)
    {
      o.workers = static_cast<std::size_t>(std::strtoull(a + 10, nullptr, 10));
    }
    else if (a[0] == '-')
    {
      return false;
    }
    else if (o.input == nullptr)
    {
      o.input = a;
    }
    else if (o.output == nullptr)
    {
      o.output = a;
    }
    else
    {
      return false;
    }
  }
  return o.input != nullptr and (o.output != nullptr or o.info or o.png != nullptr or o.minimap != nullptr) and
    (o.png == nullptr or o.png != o.minimap);
}

void print_info(const project::Project& pr, const level::Level& lv)
{
  std::printf("name: %s\n", lv.settings.name.c_str());
  std::printf(
    "size: %d x %d cells (%d x %d px cells)\n",
    lv.settings.extents.width,
    lv.settings.extents.height,
    lv.settings.cell_width,
    lv.settings.cell_height);
  std::printf("seed: %llu\n", static_cast<unsigned long long>(lv.settings.seed));
  std::printf("seasons:");
  for (const auto& s : pr.seasons)
  {
    std::printf(" %s(%ud)", s.name.c_str(), s.duration_days);
  }
  std::printf("\ncatalogs: %zu semantic types, %zu materials\n", pr.semantics.size() - 1, pr.materials.size() - 1);
  for (const auto& ts : pr.tilesets)
  {
    std::printf(
      "tileset: %s (%s, %zu tiles, %zu seasonal variants)\n",
      ts.name.c_str(),
      ts.image.c_str(),
      ts.tiles.size(),
      ts.seasonal.size());
  }
  for (const auto& layer : lv.layers)
  {
    std::printf(
      "layer: %-20s %-10s%s\n", layer.name.c_str(), enum_name(layer.kind).data(), layer.visible ? "" : " (hidden)");
  }
  const level::WfcRules& wfc = pr.rules[project::rules_of(pr, lv)].wfc;
  std::printf("wfc: %zu tiles, %zu adjacencies\n", wfc.tiles.size(), wfc.adjacency.size());
  std::printf(
    "objects: %zu portals, %zu markers, %zu path graphs (%zu nodes, %zu edges)\n",
    lv.registry.view<level::Portal>().size(),
    lv.registry.view<level::Marker>().size(),
    lv.graphs.size(),
    lv.registry.view<level::PathNode>().size(),
    lv.registry.view<level::PathEdge>().size());
}

}  // namespace

int main(int argc, char** argv)
{
  Options o;
  if (!parse(argc, argv, o))
  {
    return usage();
  }
  setLogLevel(LogSeverity::kWarn);

  // One reservation: document, per-worker I/O arenas, render scratch
  const std::size_t total = std::max<std::size_t>(o.memory_mib, 64) << 20;
  const std::size_t workers = std::clamp<std::size_t>(o.workers == 0 ? jobs::hardware_concurrency() : o.workers, 1, 16);
  memory::Arena root;
  memory::create(root, total, "lvl_convert");
  memory::Arena document;
  memory::Arena project_arena;
  memory::Arena render_scratch;
  memory::push(root, document, total / 4, "document");
  // The catalogs, seasons, tilesets and rule sets the level draws on (see //project)
  memory::push(root, project_arena, total / 20, "project");
  memory::push(root, render_scratch, total / 4, "render");
  // Half of what is left is shared by the workers, for the buffers sized by the level (a whole
  // file's bytes, a layer's encoded text); the rest is split between them
  memory::Arena io_shared;
  memory::push(root, io_shared, memory::available(root) / 2, "io-shared");
  level::IoWorker io_workers[16];
  const std::size_t per_worker = (memory::available(root) - 4096) / workers;
  for (std::size_t w = 0; w < workers; ++w)
  {
    memory::push(root, io_workers[w].json, per_worker / 4, "json");
    memory::push(root, io_workers[w].scratch, per_worker - per_worker / 4 - 256, "scratch");
  }
  jobs::Progress progress;
  level::IoContext ctx{.workers = io_workers, .worker_count = workers, .shared = &io_shared, .progress = &progress};

  int rc = EXIT_SUCCESS;
  {
    project::Project project{project_arena};
    level::Level level{document};
    if (const Status s = level::load_level(o.input, project, level, ctx); !s.ok)
    {
      std::fprintf(stderr, "error: %s\n", s.message.c_str());
      rc = EXIT_FAILURE;
    }
    if (rc == EXIT_SUCCESS and o.info)
    {
      print_info(project, level);
    }
    if (rc == EXIT_SUCCESS and o.output != nullptr)
    {
      // Always self-contained: a converted file has no project beside it, so it carries the
      // catalogs, seasons, tilesets and rules its cells reference
      if (const Status s = level::save_level(o.output, project, level, ctx, level::LevelScope::kStandalone); !s.ok)
      {
        std::fprintf(stderr, "error: %s\n", s.message.c_str());
        rc = EXIT_FAILURE;
      }
    }
    if (rc == EXIT_SUCCESS and (o.png != nullptr or o.minimap != nullptr))
    {
      // Atlases are decoded into the render scratch, below the image
      image::Image atlases[64];
      memory::Vector<image::Rgba> colors[64];
      const std::size_t count = std::min<std::size_t>(project.tilesets.size(), 64);
      memory::Arena atlas_arena;
      memory::push(render_scratch, atlas_arena, memory::available(render_scratch) / 2, "atlases");
      for (std::size_t i = 0; i < count; ++i)
      {
        new (&colors[i]) memory::Vector<image::Rgba>{memory::ArenaAllocator<image::Rgba>{atlas_arena}};
        // Stored relative to the project root (empty for a standalone level, which leaves the path
        // as it is), never relative to the working directory the tool happens to run in
        Path atlas;
        const Status located = project::resolve(project.root.view(), project.tilesets[i].image.view(), atlas);
        // A path that will not resolve is the same kind of failure as one that will not load, and
        // is reported the same way: a warning, and that tileset drawn as flat colors
        if (const Status s = located.ok ? image::load(atlas.c_str(), atlas_arena, atlases[i]) : located; !s.ok)
        {
          std::fprintf(
            stderr,
            "warning: tileset '%s': %s (drawn as flat colors)\n",
            project.tilesets[i].name.c_str(),
            s.message.c_str());
          continue;
        }
        render::average_tile_colors(project.tilesets[i], atlases[i], colors[i]);
      }
      const render::Atlases set{.images = atlases, .tile_colors = colors, .count = count};
      const render::ComposeOptions options{
        .pixels_per_cell = o.ppc, .season = o.season, .overlays = o.overlays, .objects = o.objects};
      memory::Arena work;
      memory::push(render_scratch, work, "render-work");
      Status s = Status::okay();
      if (o.png != nullptr)
      {
        s = render::export_png(o.png, project, level, set, options, work, workers);
      }
      if (s.ok and o.minimap != nullptr)
      {
        s = render::export_minimap_png(o.minimap, project, level, set, options, o.size, work);
      }
      memory::pop(render_scratch, work);
      memory::pop(render_scratch, atlas_arena);
      if (!s.ok)
      {
        std::fprintf(stderr, "error: %s\n", s.message.c_str());
        rc = EXIT_FAILURE;
      }
    }
  }
  memory::destroy(root);
  return rc;
}
