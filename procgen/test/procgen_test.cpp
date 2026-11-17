// C++ Standard Library
#include <algorithm>
#include <cmath>
#include <numeric>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/level/level.hpp"
#include "lvl/procgen/noise.hpp"
#include "lvl/procgen/watershed.hpp"
#include "lvl/procgen/wfc.hpp"

using namespace lvl;
using namespace lvl::level;
using namespace lvl::procgen;

class Procgen : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(doc, 64UL << 20, "doc");
    memory::create(scratch, 64UL << 20, "scratch");
  }
  void TearDown() override
  {
    memory::destroy(scratch);
    memory::destroy(doc);
  }
  memory::Arena doc;
  memory::Arena scratch;
};

TEST(Noise, PerlinIsDeterministicAndBounded)
{
  float lo = 1e9F;
  float hi = -1e9F;
  for (int i = 0; i < 2000; ++i)
  {
    const float x = static_cast<float>(i) * 0.137F;
    const float y = static_cast<float>(i % 37) * 0.291F;
    const float v = perlin(7, x, y);
    EXPECT_EQ(v, perlin(7, x, y));
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  EXPECT_GE(lo, -1.5F);
  EXPECT_LE(hi, 1.5F);
  EXPECT_LT(lo, -0.3F);
  EXPECT_GT(hi, 0.3F);
  // Integer lattice points are zero
  EXPECT_FLOAT_EQ(perlin(7, 3.0F, 4.0F), 0.0F);
  EXPECT_NE(perlin(7, 3.5F, 4.5F), perlin(8, 3.5F, 4.5F));
}

TEST_F(Procgen, HeightmapIsNormalizedAndParallelSafe)
{
  Grid<float> a{doc};
  Grid<float> b{doc};
  a.assign({97, 61}, 0.0F);
  b.assign({97, 61}, 0.0F);
  HeightmapParams params;
  params.layer_count = 2;
  params.layers[1] =
    NoiseLayer{.kind = NoiseKind::kRidged, .blend = Blend::kMax, .frequency = 0.05F, .amplitude = 0.8F};
  params.falloff = 0.5F;
  jobs::Progress progress;
  generate_heightmap(params, 42, a, nullptr, 1);
  generate_heightmap(params, 42, b, nullptr, 4, &progress);
  EXPECT_EQ(a.values, b.values);
  EXPECT_FLOAT_EQ(progress.fraction(), 1.0F);
  const auto [lo, hi] = std::minmax_element(a.values.begin(), a.values.end());
  EXPECT_FLOAT_EQ(*lo, 0.0F);
  EXPECT_FLOAT_EQ(*hi, 1.0F);
  generate_heightmap(params, 43, b, nullptr, 4);
  EXPECT_NE(a.values, b.values);
}

TEST_F(Procgen, SeedImageDrivesHeight)
{
  // Left half dark, right half bright
  image::Image img = image::create(doc, 8, 8, image::Rgba{0, 0, 0, 255});
  for (int y = 0; y < 8; ++y)
  {
    for (int x = 4; x < 8; ++x)
    {
      img.at(x, y) = image::Rgba{255, 255, 255, 255};
    }
  }
  Grid<float> h{doc};
  h.assign({32, 32}, 0.0F);
  HeightmapParams params;
  params.seed_weight = 1.0F;
  generate_heightmap(params, 1, h, &img, 2);
  EXPECT_LT(h.at(2, 16), 0.1F);
  EXPECT_GT(h.at(29, 16), 0.9F);
  params.seed_invert = true;
  generate_heightmap(params, 1, h, &img, 2);
  EXPECT_GT(h.at(2, 16), 0.9F);
}

TEST_F(Procgen, ErosionIsDeterministicAndMovesMaterial)
{
  Grid<float> h{doc};
  h.assign({64, 64}, 0.0F);
  HeightmapParams params;
  generate_heightmap(params, 5, h, nullptr, 1);
  Grid<float> h2{doc};
  h2.extents = h.extents;
  h2.values = h.values;
  const auto before = h.values;
  ErosionParams erosion{.droplets = 5000};
  erode(h, erosion, 9);
  erode(h2, erosion, 9);
  EXPECT_EQ(h.values, h2.values);
  float changed = 0.0F;
  for (std::size_t i = 0; i < before.size(); ++i)
  {
    changed += std::abs(before[i] - h.values[i]);
  }
  EXPECT_GT(changed, 0.1F);
}

TEST_F(Procgen, WatershedFindsRiversAndLakes)
{
  // A pit (lake) in a plane tilted down to the east (rivers)
  Grid<float> h{doc};
  h.assign({48, 48}, 0.0F);
  for (int y = 0; y < 48; ++y)
  {
    for (int x = 0; x < 48; ++x)
    {
      const float dx = static_cast<float>(x - 24);
      const float dy = static_cast<float>(y - 24);
      h.at(x, y) = 1.0F - static_cast<float>(x) / 48.0F + 0.002F * std::sqrt(dx * dx + dy * dy);
    }
  }
  h.at(24, 24) -= 0.3F;  // pit
  Grid<float> water{doc};
  water.assign(h.extents, 0.0F);
  Grid<std::uint16_t> semantic{doc};
  semantic.assign(h.extents, 0);
  WatershedParams params;
  params.erode = false;
  params.iterations = 2;
  params.river_threshold = 30.0F;
  Grid<float> flow{doc};
  flow.assign(h.extents, 0.0F);
  const WatershedOutputs outputs{
    .water = &water, .semantic = &semantic, .river_id = 1, .lake_id = 2, .tunnel_id = 3, .flow = &flow};
  const auto result = run_watershed(h, outputs, params, 1, scratch);
  // The plane descends eastwards: flow accumulates along each row towards the east edge
  EXPECT_GT(flow.at(46, 10), flow.at(10, 10));
  EXPECT_GT(result.river_cells, 0UL);
  EXPECT_GT(result.lake_cells, 0UL);
  EXPECT_EQ(result.tunnel_cells, 0UL);
  EXPECT_EQ(semantic.at(24, 24), 2);
  EXPECT_GT(water.at(24, 24), 0.1F);
  EXPECT_EQ(scratch.utilized, 0UL);
}

TEST_F(Procgen, WatershedBreachCutsTunnelsThroughRidges)
{
  // A high ridge across the middle (x = 20..23) dams the upper plateau, which drains west
  Grid<float> h{doc};
  h.assign({48, 24}, 0.0F);
  for (int y = 0; y < 24; ++y)
  {
    for (int x = 0; x < 48; ++x)
    {
      h.at(x, y) = 0.5F + 0.002F * static_cast<float>(x) + ((x >= 20 and x <= 23) ? 1.0F : 0.0F);
    }
  }
  // Block every other outlet except the west edge
  for (int x = 0; x < 48; ++x)
  {
    h.at(x, 0) = 3.0F;
    h.at(x, 23) = 3.0F;
  }
  for (int y = 0; y < 24; ++y)
  {
    h.at(47, y) = 3.0F;
  }
  Grid<float> water{doc};
  water.assign(h.extents, 0.0F);
  Grid<std::uint16_t> semantic{doc};
  semantic.assign(h.extents, 0);
  WatershedParams params;
  params.erode = false;
  params.iterations = 1;
  params.mode = DepressionMode::kBreach;
  params.river_threshold = 20.0F;
  params.tunnel_threshold = 0.2F;
  const WatershedOutputs outputs{.water = &water, .semantic = &semantic, .river_id = 1, .lake_id = 2, .tunnel_id = 3};
  const auto result = run_watershed(h, outputs, params, 1, scratch);
  EXPECT_GT(result.tunnel_cells, 0UL);
  EXPECT_EQ(result.lake_cells, 0UL);
  bool tunnel_in_ridge = false;
  for (int y = 0; y < 24; ++y)
  {
    tunnel_in_ridge = tunnel_in_ridge or semantic.at(21, y) == 3;
  }
  EXPECT_TRUE(tunnel_in_ridge);
}

namespace
{

/// Checks every adjacent pair of a solution against the rules
bool consistent(const WfcRules& rules, const Grid<TileRef>& g)
{
  for (int y = 0; y < g.height(); ++y)
  {
    for (int x = 0; x < g.width(); ++x)
    {
      const WfcTileId a = find_tile(rules, g.at(x, y));
      if (a >= rules.tiles.size())
      {
        return false;
      }
      if (x + 1 < g.width() and !allowed(rules, a, find_tile(rules, g.at(x + 1, y)), Direction::kEast))
      {
        return false;
      }
      if (y + 1 < g.height() and !allowed(rules, a, find_tile(rules, g.at(x, y + 1)), Direction::kSouth))
      {
        return false;
      }
    }
  }
  return true;
}

/// Water (0), sand (1), grass (2): water-sand-grass may only touch their neighbors in that order
void coast_rules(WfcRules& rules)
{
  for (TileIndex t = 0; t < 3; ++t)
  {
    add_tile(rules, TileRef{0, t});
  }
  for (const Direction d : kDirections)
  {
    for (WfcTileId a = 0; a < 3; ++a)
    {
      for (WfcTileId b = 0; b < 3; ++b)
      {
        if (std::abs(static_cast<int>(a) - static_cast<int>(b)) <= 1)
        {
          set_allowed(rules, a, b, d, true);
        }
      }
    }
  }
}

}  // namespace

TEST_F(Procgen, WfcSolvesWithAdjacency)
{
  WfcRules rules{doc};
  coast_rules(rules);
  Grid<TileRef> out{doc};
  out.assign({40, 30}, kEmptyTile);
  jobs::Progress progress;
  const auto result = solve(rules, WfcConstraints{}, WfcParams{.seed = 3}, out, scratch, &progress);
  ASSERT_TRUE(result.status.ok) << result.status;
  EXPECT_TRUE(consistent(rules, out));
  EXPECT_EQ(scratch.utilized, 0UL);
  // Deterministic per seed
  Grid<TileRef> again{doc};
  again.assign(out.extents, kEmptyTile);
  ASSERT_TRUE(solve(rules, WfcConstraints{}, WfcParams{.seed = 3}, again, scratch).status.ok);
  EXPECT_EQ(out.values, again.values);
}

TEST_F(Procgen, WfcFollowsHeightAndWaterLayers)
{
  WfcRules rules{doc};
  coast_rules(rules);
  rules.tiles[0].water = WaterRule::kWet;
  rules.tiles[1].water = WaterRule::kShore;
  rules.tiles[2].water = WaterRule::kDry;
  rules.tiles[2].height_min = 0.5F;
  Grid<float> water{doc};
  water.assign({30, 20}, 0.0F);
  Grid<float> height{doc};
  height.assign({30, 20}, 1.0F);
  for (int y = 0; y < 20; ++y)
  {
    for (int x = 0; x < 8; ++x)
    {
      water.at(x, y) = 1.0F;
    }
  }
  Grid<TileRef> out{doc};
  out.assign(water.extents, kEmptyTile);
  const WfcConstraints constraints{.height = &height, .water = &water};
  const auto result = solve(rules, constraints, WfcParams{.seed = 11}, out, scratch);
  ASSERT_TRUE(result.status.ok) << result.status;
  EXPECT_TRUE(consistent(rules, out));
  for (int y = 0; y < 20; ++y)
  {
    EXPECT_EQ(out.at(3, y).tile, 0) << "water where wet";
    EXPECT_EQ(out.at(8, y).tile, 1) << "sand on the shore";
    EXPECT_EQ(out.at(20, y).tile, 2) << "grass inland";
  }
}

TEST_F(Procgen, WfcKeepsExistingTilesAndRegion)
{
  WfcRules rules{doc};
  coast_rules(rules);
  Grid<TileRef> out{doc};
  out.assign({20, 20}, TileRef{0, 2});
  out.at(10, 10) = TileRef{0, 0};  // a water tile, fixed
  // Existing tiles: grass outside the region, the water tile inside it, nothing else
  Grid<TileRef> existing{doc};
  existing.extents = out.extents;
  existing.values = out.values;
  for (int y = 5; y < 15; ++y)
  {
    for (int x = 5; x < 15; ++x)
    {
      existing.at(x, y) = (x == 10 and y == 10) ? TileRef{0, 0} : kEmptyTile;
    }
  }
  const WfcConstraints constraints{.existing = &existing};
  const auto result = solve(rules, constraints, WfcParams{.seed = 5, .region = {5, 5, 10, 10}}, out, scratch);
  ASSERT_TRUE(result.status.ok) << result.status;
  EXPECT_EQ(out.at(10, 10), (TileRef{0, 0}));
  EXPECT_EQ(out.at(0, 0), (TileRef{0, 2}));
  EXPECT_TRUE(consistent(rules, out));
}

TEST_F(Procgen, WfcReportsImpossibleConstraints)
{
  WfcRules rules{doc};
  coast_rules(rules);
  rules.tiles[0].water = WaterRule::kWet;
  rules.tiles[1].water = WaterRule::kWet;
  rules.tiles[2].water = WaterRule::kWet;
  Grid<float> water{doc};
  water.assign({4, 4}, 0.0F);
  Grid<TileRef> out{doc};
  out.assign({4, 4}, kEmptyTile);
  const auto result = solve(rules, WfcConstraints{.water = &water}, WfcParams{}, out, scratch);
  EXPECT_FALSE(result.status.ok);

  WfcRules empty{doc};
  EXPECT_FALSE(solve(empty, WfcConstraints{}, WfcParams{}, out, scratch).status.ok);
}
