// C++ Standard Library
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/editor/jobs.hpp"
#include "lvl/level/objects.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/project/project.hpp"

using namespace lvl;
using namespace lvl::level;

namespace
{

std::string temp(const char* name) { return std::string{::testing::TempDir()} + name; }

/// Writes a noisy (poorly compressible) PNG
void write_png(const std::string& path, int width, int height, memory::Arena& arena)
{
  memory::Scope scope{arena};
  image::Image img = image::create(arena, width, height);
  std::uint32_t state = static_cast<std::uint32_t>(width * 31 + height);
  for (std::size_t i = 0; i < img.size(); ++i)
  {
    state = state * 1664525U + 1013904223U;
    img.pixels[i] = image::Rgba{
      static_cast<std::uint8_t>(state >> 24),
      static_cast<std::uint8_t>(state >> 16),
      static_cast<std::uint8_t>(state >> 8),
      255};
  }
  memory::Arena encode;
  memory::push(arena, encode, "encode");
  ASSERT_TRUE(image::save_png(path.c_str(), img, encode).ok);
  memory::pop(arena, encode);
}

class AtlasLoading : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(root, 768UL << 20, "root");
    memory::push(root, proj, 16UL << 20, "proj");
    memory::push(root, assets, 64UL << 20, "assets");
    memory::push(root, work, 256UL << 20, "work");
    pr = new (storage) project::Project{proj};
    write_png(big, 2048, 1700, work);
    write_png(small_a, 64, 32, work);
    write_png(small_b, 48, 48, work);
    for (const auto* p : {&big, &small_a, &small_b})
    {
      const TileSetId id = project::add_tileset(*pr, "ts");
      pr->tilesets[id].image.assign(*p);
    }
    project::add_tileset(*pr, "no image");
    const TileSetId missing = project::add_tileset(*pr, "missing");
    pr->tilesets[missing].image.assign("/nonexistent/atlas.png");
  }
  void TearDown() override
  {
    pr->~Project();
    memory::destroy(root);
  }

  /// Job memory partitioned from a jobs arena of <code>bytes</code>
  void partition_jobs(std::size_t bytes)
  {
    memory::clear(work);
    memory::Arena jobs_arena;
    memory::push(work, jobs_arena, bytes, "jobs");
    jobs = jobs_arena;
    editor::partition(jobs, memory);
  }

  memory::Arena root;
  memory::Arena proj;
  memory::Arena assets;
  memory::Arena work;
  memory::Arena jobs;
  editor::JobMemory memory;
  alignas(project::Project) unsigned char storage[sizeof(project::Project)] = {};
  project::Project* pr = nullptr;
  const std::string big = temp("big_atlas.png");
  const std::string small_a = temp("small_a.png");
  const std::string small_b = temp("small_b.png");
};

/// Job memory for level I/O, with no atlases in the way
class LevelJobs : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(root, 512UL << 20, "root");
    memory::push(root, doc_a, 48UL << 20, "doc_a");
    memory::push(root, doc_b, 48UL << 20, "doc_b");
    memory::push(root, proj_a, 8UL << 20, "proj_a");
    memory::push(root, proj_b, 8UL << 20, "proj_b");
    memory::push(root, work, 376UL << 20, "work");
    source = new (storage_a) Level{doc_a};
    target = new (storage_b) Level{doc_b};
    pr_a = new (project_storage_a) project::Project{proj_a};
    pr_b = new (project_storage_b) project::Project{proj_b};
  }
  void TearDown() override
  {
    source->~Level();
    target->~Level();
    pr_a->~Project();
    pr_b->~Project();
    memory::destroy(root);
  }

  /// Job memory partitioned from a jobs arena of <code>bytes</code>
  void partition_jobs(std::size_t bytes)
  {
    memory::clear(work);
    memory::Arena jobs_arena;
    memory::push(work, jobs_arena, bytes, "jobs");
    editor::partition(jobs_arena, memory);
  }

  memory::Arena root;
  memory::Arena doc_a;
  memory::Arena doc_b;
  memory::Arena proj_a;
  memory::Arena proj_b;
  memory::Arena work;
  editor::JobMemory memory;
  alignas(Level) unsigned char storage_a[sizeof(Level)];
  alignas(Level) unsigned char storage_b[sizeof(Level)];
  alignas(project::Project) unsigned char project_storage_a[sizeof(project::Project)] = {};
  alignas(project::Project) unsigned char project_storage_b[sizeof(project::Project)] = {};
  Level* source = nullptr;
  Level* target = nullptr;
  project::Project* pr_a = nullptr;
  project::Project* pr_b = nullptr;
};

}  // namespace

namespace
{

/// Writes a small level file for <code>name</code> and adds it to the project as a member
void add_member(
  project::Project& project,
  Level& scratch,
  const char* root,
  const char* name,
  const char* portal,
  editor::JobMemory& memory)
{
  level::reset(scratch);
  create_default(scratch, Settings{.name = name, .extents = {4, 3}, .cell_width = 8, .cell_height = 8});
  add_layer(scratch, LayerKind::kSemantic, "semantics");
  plane<IdGrid>(scratch.layers[find_layer(scratch, LayerKind::kSemantic)]).at(0, 0) = 2;
  create_portal(scratch.registry, portal, {1, 1}, {1, 1}, Portal{});

  Path file;
  ASSERT_TRUE(project::project_file(project::Folder::kLevels, name, ".json", file).ok);
  Path absolute;
  ASSERT_TRUE(project::resolve(std::string_view{root}, file.view(), absolute).ok);
  auto ctx = memory.context(nullptr);
  const Status saved = save_level(absolute.c_str(), project, scratch, ctx);
  ASSERT_TRUE(saved.ok) << saved;
  ASSERT_NE(project::add_level(project, name, file.view()), project::kNoLevel);
}

}  // namespace

TEST_F(LevelJobs, RescanReadsEveryMemberWithOnlyOneResident)
{
  partition_jobs(64UL << 20);
  const std::string root = temp("rescan_project");
  std::filesystem::remove_all(root);
  ASSERT_TRUE(project::ensure_tree(root).ok);
  pr_a->root.assign(root);
  // Two semantic entries, so the levels' cells name something
  project::intern_semantic(*pr_a, "grass");
  project::intern_semantic(*pr_a, "road");

  add_member(*pr_a, *source, root.c_str(), "overworld", "cave_mouth", memory);
  add_member(*pr_a, *source, root.c_str(), "cave", "exit", memory);
  // The index knows nothing about them yet: add_level records the name and file only
  ASSERT_EQ(pr_a->levels.size(), 2U);
  EXPECT_EQ(pr_a->levels[0].portal_count, 0U);

  editor::RescanProjectJob job;
  job.project = pr_a;
  job.scratch = target;
  job.scratch_project = pr_b;
  job.memory = &memory;
  jobs::Progress progress;
  job.run(progress);
  ASSERT_EQ(job.failed, 0UL) << job.result;
  ASSERT_EQ(job.member_count, 2UL);
  ASSERT_EQ(job.name_count, 2UL);
  EXPECT_TRUE(job.members[0].read);
  EXPECT_EQ(job.members[0].extents.width, 4);
  ASSERT_EQ(job.members[0].portal_count, 1U);
  EXPECT_EQ(job.names[job.members[0].portals_begin], std::string_view{"cave_mouth"});
  EXPECT_EQ(job.names[job.members[1].portals_begin], std::string_view{"exit"});

  // Applying what it found is what finish_job does; a link then resolves without a load
  for (std::size_t i = 0; i < job.member_count; ++i)
  {
    const auto& m = job.members[i];
    ASSERT_TRUE(project::set_level_objects(
                  *pr_a,
                  i,
                  project::LevelObjects{
                    .extents = m.extents,
                    .portals = job.names + m.portals_begin,
                    .portal_count = m.portal_count,
                    .markers = job.names + m.markers_begin,
                    .marker_count = m.marker_count},
                  work)
                  .ok);
  }
  EXPECT_EQ(project::portal_link(*pr_a, "cave", "exit"), project::LinkState::kResolved);
  EXPECT_EQ(project::portal_link(*pr_a, "cave", "cave_mouth"), project::LinkState::kUnknownPortal);
}

TEST_F(LevelJobs, RescanReportsAMemberItCannotRead)
{
  partition_jobs(64UL << 20);
  const std::string root = temp("rescan_missing");
  std::filesystem::remove_all(root);
  ASSERT_TRUE(project::ensure_tree(root).ok);
  pr_a->root.assign(root);
  add_member(*pr_a, *source, root.c_str(), "overworld", "cave_mouth", memory);
  ASSERT_NE(project::add_level(*pr_a, "gone", "levels/gone.json"), project::kNoLevel);

  editor::RescanProjectJob job;
  job.project = pr_a;
  job.scratch = target;
  job.scratch_project = pr_b;
  job.memory = &memory;
  jobs::Progress progress;
  job.run(progress);
  EXPECT_EQ(job.failed, 1UL);
  EXPECT_FALSE(job.result.ok);
  // The one that is there was still read
  EXPECT_TRUE(job.members[0].read);
  EXPECT_FALSE(job.members[1].read);
}

TEST_F(LevelJobs, CompactingRewritesEveryMemberButTheOpenOne)
{
  partition_jobs(64UL << 20);
  const std::string root = temp("compact_project");
  std::filesystem::remove_all(root);
  ASSERT_TRUE(project::ensure_tree(root).ok);
  pr_a->root.assign(root);
  const level::SemanticId grass = project::intern_semantic(*pr_a, "grass");
  const level::SemanticId road = project::intern_semantic(*pr_a, "road");
  ASSERT_EQ(grass, 1);
  ASSERT_EQ(road, 2);

  // Both levels put "road" in a cell, so both have an id that compaction moves
  for (const char* name : {"overworld", "cave"})
  {
    add_member(*pr_a, *source, root.c_str(), name, "p", memory);
    const std::size_t member = project::find_level(*pr_a, name);
    ASSERT_NE(member, project::kNoLevel);
    Path absolute;
    ASSERT_TRUE(project::resolve(std::string_view{root}, pr_a->levels[member].file.view(), absolute).ok);
    auto ctx = memory.context(nullptr);
    level::reset(*source);
    project::reset(*pr_b);
    ASSERT_TRUE(load_level(absolute.c_str(), *pr_b, *source, ctx).ok);
    EXPECT_EQ(plane<IdGrid>(source->layers[find_layer(*source, LayerKind::kSemantic)]).at(0, 0), road);
  }

  // "grass" goes, so "road" moves from 2 to 1
  level::reset(*source);
  create_default(*source, Settings{.name = "overworld", .extents = {1, 1}});
  project::remove_semantic(*pr_a, *source, grass);
  ASSERT_TRUE(project::needs_compaction(*pr_a));

  editor::CompactCatalogsJob job;
  job.project = pr_a;
  job.scratch = target;
  job.scratch_project = pr_b;
  job.skip = "overworld";
  job.memory = &memory;
  jobs::Progress progress;
  job.run(progress);
  ASSERT_TRUE(job.result.ok) << job.result;
  // One rewritten; the open one is left to the UI thread
  EXPECT_EQ(job.rewritten, 1UL);
  EXPECT_TRUE(job.stopped_on.empty());
  ASSERT_NE(job.plan, nullptr);
  EXPECT_EQ(job.plan->retired_semantics, 1UL);

  // The file on disk now holds "road"'s new id, and the entries have not been dropped yet
  EXPECT_EQ(pr_a->semantics.size(), 3U);
  Path absolute;
  ASSERT_TRUE(project::resolve(std::string_view{root}, std::string_view{"levels/cave.json"}, absolute).ok);
  auto ctx = memory.context(nullptr);
  level::reset(*source);
  project::reset(*pr_b);
  ASSERT_TRUE(load_level(absolute.c_str(), *pr_b, *source, ctx).ok);
  EXPECT_EQ(plane<IdGrid>(source->layers[find_layer(*source, LayerKind::kSemantic)]).at(0, 0), 1);
  // Written back in project scope: the manifest is the authority for a member's catalogs, so the
  // file no longer carries a copy that could name the old ids beside the new cells
  EXPECT_EQ(pr_b->semantics.size(), 1U) << "only the reserved 'none'";
}

TEST_F(LevelJobs, SavesAndLoadsLayersBiggerThanAWorkersScratch)
{
  // A worker gets jobs / 2 / workers, so its share depends on the core count this machine happens
  // to have. Work backwards from that instead: the jobs arena that lands a worker's scratch on each
  // size below, whatever the core count. The level is then sized from the scratch partition()
  // really handed out, so one layer's text always needs more than it holds (the field report). A
  // scalar cell's text is at most 16 bytes.
  partition_jobs(16UL << 20);
  const std::size_t workers = memory.worker_count;
  for (const std::size_t want : {2UL << 20, 16UL << 20})
  {
    partition_jobs(want * 8 / 3 * workers + 8192);
    const std::size_t scratch = memory.workers[0].scratch.capacity;
    const int side = static_cast<int>(std::sqrt(static_cast<double>(scratch / 16 * 3 / 2)));
    level::reset(*source);
    level::reset(*target);
    create_default(*source, Settings{.name = "big", .extents = {side, side}, .cell_width = 8, .cell_height = 8});
    auto& height = plane<ScalarGrid>(source->layers[0]);
    for (std::size_t i = 0; i < height.values.size(); ++i)
    {
      height.values[i] = static_cast<float>(i % 10);
    }
    ASSERT_GT(height.values.size() * 16, scratch) << (scratch >> 20) << " MiB of worker scratch";

    jobs::Progress progress;
    auto ctx = memory.context(&progress);
    ASSERT_EQ(ctx.shared, &memory.general);

    const auto path = temp("job_level.json");
    const auto saved = save_level(path.c_str(), *pr_a, *source, ctx);
    ASSERT_TRUE(saved.ok) << saved;

    const auto status = load_level(path.c_str(), *pr_b, *target, ctx);
    ASSERT_TRUE(status.ok) << status;
    ASSERT_EQ(target->layers.size(), source->layers.size());
    const auto& reloaded = plane<ScalarGrid>(target->layers[0]);
    ASSERT_EQ(reloaded.values.size(), height.values.size());
    for (std::size_t i = 0; i < height.values.size(); ++i)
    {
      ASSERT_EQ(reloaded.values[i], height.values[i]) << "cell " << i;
    }
  }
}

TEST_F(AtlasLoading, LargeAtlasLoadsDespiteSmallWorkerArenas)
{
  // Workers' scratch arenas end up far smaller than the big atlas needs (the field report)
  partition_jobs(128UL << 20);
  ASSERT_LT(memory.workers[0].scratch.capacity, 30UL << 20);
  editor::AtlasImages out;
  jobs::Progress progress;
  progress.reset(1);
  progress.advance();  // as when a level load came first
  editor::load_atlases(*pr, assets, memory, out, &progress);
  ASSERT_EQ(out.count, 5UL);
  EXPECT_TRUE(out.status[0].ok) << out.status[0];
  EXPECT_EQ(out.images[0].width, 2048);
  EXPECT_TRUE(out.status[1].ok) << out.status[1];
  EXPECT_EQ(out.images[1].width, 64);
  EXPECT_TRUE(out.status[2].ok) << out.status[2];
  EXPECT_FALSE(out.status[3].ok);
  EXPECT_FALSE(out.status[4].ok);
  EXPECT_TRUE(out.images[4].empty());
  EXPECT_EQ(progress.completed(), progress.total());
  // Pixels live in the asset arena
  const auto lo = reinterpret_cast<std::uintptr_t>(assets.data);
  const auto p = reinterpret_cast<std::uintptr_t>(out.images[0].pixels);
  EXPECT_GE(p, lo);
  EXPECT_LT(p, lo + assets.capacity);
}

TEST_F(AtlasLoading, TooLittleJobMemoryIsReportedPerAtlas)
{
  // The general arena can't hold the big atlas's decode buffers: an error for it, the rest load
  partition_jobs(40UL << 20);
  editor::AtlasImages out;
  editor::load_atlases(*pr, assets, memory, out, nullptr);
  EXPECT_FALSE(out.status[0].ok);
  EXPECT_NE(out.status[0].message.view().find("--memory"), std::string_view::npos) << out.status[0];
  EXPECT_TRUE(out.images[0].empty());
  EXPECT_TRUE(out.status[1].ok) << out.status[1];
  EXPECT_TRUE(out.status[2].ok) << out.status[2];
}

TEST_F(AtlasLoading, TooLittleAssetMemoryIsReportedPerAtlas)
{
  partition_jobs(128UL << 20);
  memory::Arena small_assets;
  memory::push(work, small_assets, 1UL << 20, "small-assets");
  editor::AtlasImages out;
  editor::load_atlases(*pr, small_assets, memory, out, nullptr);
  EXPECT_FALSE(out.status[0].ok);
  EXPECT_NE(out.status[0].message.view().find("asset memory"), std::string_view::npos) << out.status[0];
  EXPECT_TRUE(out.status[1].ok) << out.status[1];
}
