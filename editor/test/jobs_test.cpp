// C++ Standard Library
#include <cstdint>
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/editor/jobs.hpp"

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
    memory::push(root, doc, 16UL << 20, "doc");
    memory::push(root, assets, 64UL << 20, "assets");
    memory::push(root, work, 256UL << 20, "work");
    level = new (storage) Level{doc};
    create_default(*level, Settings{});
    write_png(big, 2048, 1700, work);
    write_png(small_a, 64, 32, work);
    write_png(small_b, 48, 48, work);
    for (const auto* p : {&big, &small_a, &small_b})
    {
      const TileSetId id = add_tileset(*level, "ts");
      level->tilesets[id].image.assign(*p);
    }
    add_tileset(*level, "no image");
    const TileSetId missing = add_tileset(*level, "missing");
    level->tilesets[missing].image.assign("/nonexistent/atlas.png");
  }
  void TearDown() override
  {
    level->~Level();
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
  memory::Arena doc;
  memory::Arena assets;
  memory::Arena work;
  memory::Arena jobs;
  editor::JobMemory memory;
  alignas(Level) unsigned char storage[sizeof(Level)];
  Level* level = nullptr;
  const std::string big = temp("big_atlas.png");
  const std::string small_a = temp("small_a.png");
  const std::string small_b = temp("small_b.png");
};

}  // namespace

TEST_F(AtlasLoading, LargeAtlasLoadsDespiteSmallWorkerArenas)
{
  // Workers' scratch arenas end up far smaller than the big atlas needs (the field report)
  partition_jobs(128UL << 20);
  ASSERT_LT(memory.workers[0].scratch.capacity, 30UL << 20);
  editor::AtlasImages out;
  jobs::Progress progress;
  progress.reset(1);
  progress.advance();  // as when a level load came first
  editor::load_atlases(*level, assets, memory, out, &progress);
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
  editor::load_atlases(*level, assets, memory, out, nullptr);
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
  editor::load_atlases(*level, small_assets, memory, out, nullptr);
  EXPECT_FALSE(out.status[0].ok);
  EXPECT_NE(out.status[0].message.view().find("asset memory"), std::string_view::npos) << out.status[0];
  EXPECT_TRUE(out.status[1].ok) << out.status[1];
}
