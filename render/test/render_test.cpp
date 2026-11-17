// C++ Standard Library
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/render/compose.hpp"
#include "lvl/render/preview.hpp"

using namespace lvl;
using namespace lvl::level;

class Render : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(doc, 32UL << 20, "doc");
    memory::create(scratch, 64UL << 20, "scratch");
    level = new (&storage) Level{doc};
    create_default(*level, Settings{.extents = {6, 4}, .cell_width = 4, .cell_height = 4});
    // A 2x1-tile atlas: tile 0 red, tile 1 green (4x4 pixels each)
    atlas = image::create(doc, 8, 4, image::Rgba{255, 0, 0, 255});
    for (int y = 0; y < 4; ++y)
    {
      for (int x = 4; x < 8; ++x)
      {
        atlas.at(x, y) = image::Rgba{0, 255, 0, 255};
      }
    }
    const TileSetId id = add_tileset(*level, "rg");
    TileSet& ts = level->tilesets[id];
    ts.slice = SliceParams{.tile_width = 4, .tile_height = 4};
    ts.image_width = 8;
    ts.image_height = 4;
    reslice(ts);
    set_seasonal_variant(ts, SeasonalVariant{.tile = 0, .season = "spring", .alternate = 1});
    colors = memory::make_vector<image::Rgba>(doc);
    render::average_tile_colors(ts, atlas, colors);
    auto& ground = plane<TileGrid>(level->layers[1]);
    ground.fill(TileRef{id, 0});
    ground.at(5, 3) = TileRef{id, 1};
    atlases = render::Atlases{.images = &atlas, .tile_colors = &colors, .count = 1};
  }
  void TearDown() override
  {
    level->~Level();
    memory::destroy(scratch);
    memory::destroy(doc);
  }

  memory::Arena doc;
  memory::Arena scratch;
  alignas(Level) unsigned char storage[sizeof(Level)];
  Level* level = nullptr;
  image::Image atlas;
  memory::Vector<image::Rgba> colors;
  render::Atlases atlases;
};

TEST_F(Render, AverageTileColors)
{
  ASSERT_EQ(colors.size(), 2UL);
  EXPECT_EQ(colors[0], (image::Rgba{255, 0, 0, 255}));
  EXPECT_EQ(colors[1], (image::Rgba{0, 255, 0, 255}));
}

TEST_F(Render, ComposeDrawsTilesAndSeasons)
{
  image::Image img;
  ASSERT_TRUE(render::compose(*level, atlases, render::ComposeOptions{.overlays = false}, scratch, img, 3).ok);
  EXPECT_EQ(img.width, 24);
  EXPECT_EQ(img.height, 16);
  EXPECT_EQ(img.at(1, 1), (image::Rgba{255, 0, 0, 255}));
  EXPECT_EQ(img.at(22, 14), (image::Rgba{0, 255, 0, 255}));

  image::Image spring;
  ASSERT_TRUE(
    render::compose(*level, atlases, render::ComposeOptions{.season = "spring", .overlays = false}, scratch, spring)
      .ok);
  EXPECT_EQ(spring.at(1, 1), (image::Rgba{0, 255, 0, 255}));
}

TEST_F(Render, OverlaysBlendOverTiles)
{
  // Visible water layer, fully deep at one cell
  const auto w = add_layer(*level, LayerKind::kWater, "water");
  level->layers[w].opacity = 1.0F;
  level->layers[w].preview = Preview::kHeat;
  plane<ScalarGrid>(level->layers[w]).at(0, 0) = 1.0F;
  fit_range(level->layers[w]);
  image::Image img;
  ASSERT_TRUE(render::compose(*level, atlases, render::ComposeOptions{.pixels_per_cell = 1}, scratch, img).ok);
  EXPECT_EQ(img.at(0, 0), render::heat_ramp(1.0F));
  EXPECT_EQ(img.at(1, 0), (image::Rgba{255, 0, 0, 255}));
}

TEST_F(Render, ComposeRefusesHugeImages)
{
  memory::Arena tiny;
  memory::push(scratch, tiny, 1024, "tiny");
  image::Image img;
  const auto status = render::compose(*level, atlases, render::ComposeOptions{.pixels_per_cell = 64}, tiny, img);
  EXPECT_FALSE(status.ok);
  memory::pop(scratch, tiny);
}

TEST_F(Render, MinimapFitsMaxSize)
{
  image::Image img;
  ASSERT_TRUE(render::minimap(*level, atlases, render::ComposeOptions{}, 3, doc, scratch, img).ok);
  EXPECT_EQ(img.width, 3);
  EXPECT_EQ(img.height, 2);
  EXPECT_EQ(scratch.utilized, 0UL);
}

TEST_F(Render, ExportPngs)
{
  const std::string full = std::string{::testing::TempDir()} + "map.png";
  const std::string mini = std::string{::testing::TempDir()} + "mini.png";
  ASSERT_TRUE(
    render::export_png(full.c_str(), *level, atlases, render::ComposeOptions{.objects = true}, scratch, 2).ok);
  ASSERT_TRUE(render::export_minimap_png(mini.c_str(), *level, atlases, render::ComposeOptions{}, 64, scratch).ok);
  EXPECT_EQ(scratch.utilized, 0UL);
  image::Image loaded;
  memory::Arena tmp;
  memory::push(scratch, tmp, 1 << 20, "tmp");
  ASSERT_TRUE(image::load(full.c_str(), doc, tmp, loaded).ok);
  EXPECT_EQ(loaded.width, 24);
  ASSERT_TRUE(image::load(mini.c_str(), doc, tmp, loaded).ok);
  EXPECT_EQ(loaded.width, 24);  // 6 cells, at most 4x upscaled
  memory::pop(scratch, tmp);
}

TEST(Preview, Ramps)
{
  EXPECT_EQ(render::terrain_ramp(0.0F), (image::Rgba{20, 40, 110, 255}));
  EXPECT_EQ(render::terrain_ramp(1.0F), (image::Rgba{250, 250, 250, 255}));
  EXPECT_EQ(render::heat_ramp(-1.0F), render::heat_ramp(0.0F));
  EXPECT_NE(render::id_color(1), render::id_color(2));
}
