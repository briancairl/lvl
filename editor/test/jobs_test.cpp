// C++ Standard Library
#include <cmath>
#include <cstdint>
#include <new>
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/editor/jobs.hpp"
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
