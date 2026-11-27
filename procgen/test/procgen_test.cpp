// C++ Standard Library
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/image.hpp"
#include "lvl/level/auto_tile_rules.hpp"
#include "lvl/level/level.hpp"
#include "lvl/procgen/autotile.hpp"
#include "lvl/procgen/fill.hpp"
#include "lvl/procgen/noise.hpp"
#include "lvl/procgen/scatter.hpp"
#include "lvl/procgen/steps.hpp"
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

/**
 * @brief A river is a bed as wide as the flow it carries, with banks that taper into the terrain
 *
 * The map is a steep-sided valley running east: every cell of a wall drains straight into the
 * middle row, so the only channel over the threshold is the trunk, and the flow in it grows as it
 * goes. That is what the widths and the taper are read off.
 */
TEST_F(Procgen, WatershedWidensRiversWithFlow)
{
  constexpr int kWidth = 64;
  constexpr int kHeight = 33;
  constexpr int kMiddle = 16;
  constexpr int kColumn = 48;
  const auto make_valley = [](Grid<float>& g) {
    g.assign({kWidth, kHeight}, 0.0F);
    for (int y = 0; y < kHeight; ++y)
    {
      for (int x = 0; x < kWidth; ++x)
      {
        g.at(x, y) = 1.0F - 0.4F * static_cast<float>(x) / static_cast<float>(kWidth) +
          0.1F * static_cast<float>(std::abs(y - kMiddle));
      }
    }
  };
  const auto wet_in_column = [](const Grid<float>& water, int x) {
    int count = 0;
    for (int y = 0; y < kHeight; ++y)
    {
      count += (water.at(x, y) > 1e-4F) ? 1 : 0;
    }
    return count;
  };

  WatershedParams params;
  params.erode = false;
  params.iterations = 1;
  params.river_threshold = 20.0F;

  // With no width at all, the channel is the one cell the flow line runs through, as it always was
  {
    Grid<float> h{doc};
    make_valley(h);
    Grid<float> water{doc};
    water.assign(h.extents, 0.0F);
    WatershedParams narrow = params;
    narrow.river_width = 0.0F;
    narrow.river_width_max = 0.0F;
    narrow.bank_width = 0.0F;
    narrow.channel_smoothing = 0;
    const auto result = run_watershed(h, WatershedOutputs{.water = &water}, narrow, 1, scratch);
    EXPECT_GT(result.river_cells, 0UL);
    EXPECT_EQ(wet_in_column(water, kColumn), 1);
    EXPECT_GT(water.at(kColumn, kMiddle), 0.0F);
    EXPECT_EQ(scratch.utilized, 0UL);
  }

  // With the defaults the bed is several cells across, and both the water and the carve fall away
  // from the middle of it rather than stepping
  Grid<float> h{doc};
  make_valley(h);
  const auto before = h.values;
  Grid<float> water{doc};
  water.assign(h.extents, 0.0F);
  Grid<std::uint16_t> semantic{doc};
  semantic.assign(h.extents, 0);
  const WatershedOutputs outputs{.water = &water, .semantic = &semantic, .river_id = 1, .lake_id = 2};
  const auto result = run_watershed(h, outputs, params, 1, scratch);
  EXPECT_EQ(scratch.utilized, 0UL);
  EXPECT_GT(result.river_cells, 0UL);
  EXPECT_EQ(result.lake_cells, 0UL);
  EXPECT_GT(wet_in_column(water, kColumn), 3);
  // The semantics cover the whole widened bed, not just the flow line
  int river_semantics = 0;
  for (int y = 0; y < kHeight; ++y)
  {
    river_semantics += (semantic.at(kColumn, y) == 1) ? 1 : 0;
  }
  EXPECT_EQ(river_semantics, wet_in_column(water, kColumn));

  // Away from the middle row: shallower water every step, and a carve that only lessens
  int last_wet = kMiddle;
  while (last_wet + 1 < kHeight and water.at(kColumn, last_wet + 1) > 1e-4F)
  {
    ++last_wet;
  }
  EXPECT_GT(last_wet, kMiddle + 2);
  const auto carved = [&](int y) {
    const std::size_t i = h.extents.index(kColumn, y);
    return before[i] - h.values[i];
  };
  for (int y = kMiddle; y < last_wet; ++y)
  {
    EXPECT_LE(water.at(kColumn, y + 1), water.at(kColumn, y)) << "water deepens away from the channel at y=" << y;
    EXPECT_LE(carved(y + 1), carved(y) + 1e-6F) << "the carve deepens away from the channel at y=" << y;
  }
  // The bank is a ramp rather than a wall: between the bed and dry ground are cells of its own,
  // which is the whole point of the taper -- an edge the tiles and the preview can grade over
  int ramp = 0;
  for (int y = kMiddle; y <= last_wet; ++y)
  {
    const float f = water.at(kColumn, y) / water.at(kColumn, kMiddle);
    ramp += (f > 0.05F and f < 0.9F) ? 1 : 0;
  }
  EXPECT_GE(ramp, 2);
  EXPECT_LT(water.at(kColumn, last_wet), water.at(kColumn, kMiddle));
  EXPECT_GT(carved(kMiddle), 0.0F);
  // A river carrying more flow is wider: the valley collects all the way east
  EXPECT_GT(wet_in_column(water, 56), wet_in_column(water, 8));

  // However much flow collects, the bed stops widening at river_width_max
  Grid<float> capped_height{doc};
  make_valley(capped_height);
  Grid<float> capped{doc};
  capped.assign(capped_height.extents, 0.0F);
  WatershedParams tight = params;
  tight.river_width_max = 1.0F;
  tight.bank_width = 0.0F;
  tight.channel_smoothing = 0;
  run_watershed(capped_height, WatershedOutputs{.water = &capped}, tight, 1, scratch);
  EXPECT_EQ(wet_in_column(capped, 56), 3);
  EXPECT_LT(wet_in_column(capped, 56), wet_in_column(water, 56));
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
      for (const Direction d : {Direction::kSouthEast, Direction::kSouthWest})
      {
        const Cell n = step(Cell{x, y}, d);
        if (rules.diagonals and g.extents.contains(n) and !allowed(rules, a, find_tile(rules, g.at(n)), d))
        {
          return false;
        }
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

TEST_F(Procgen, WfcHonorsDiagonalRules)
{
  // Two tiles that may sit side by side freely, but corners must match: only checkerboards or
  // uniform rows/columns patterns survive. Without the diagonal flag the corner rules are ignored.
  WfcRules rules{doc};
  const WfcTileId a = add_tile(rules, TileRef{0, 0});
  const WfcTileId b = add_tile(rules, TileRef{0, 1});
  for (const Direction d : kDirections)
  {
    for (const WfcTileId x : {a, b})
    {
      for (const WfcTileId y : {a, b})
      {
        set_allowed(rules, x, y, d, true);
      }
    }
  }
  for (const Direction d : kDiagonals)
  {
    set_allowed(rules, a, a, d, true);
    set_allowed(rules, b, b, d, true);
  }
  const auto corners_match = [&](const Grid<TileRef>& g) {
    for (int y = 0; y + 1 < g.height(); ++y)
    {
      for (int x = 0; x + 1 < g.width(); ++x)
      {
        if (g.at(x, y) != g.at(x + 1, y + 1) or g.at(x + 1, y) != g.at(x, y + 1))
        {
          return false;
        }
      }
    }
    return true;
  };
  rules.diagonals = true;
  for (std::uint64_t seed = 1; seed <= 4; ++seed)
  {
    Grid<TileRef> out{doc};
    out.assign({16, 12}, kEmptyTile);
    const auto result = solve(rules, WfcConstraints{}, WfcParams{.seed = seed}, out, scratch);
    ASSERT_TRUE(result.status.ok) << result.status;
    EXPECT_TRUE(consistent(rules, out));
    EXPECT_TRUE(corners_match(out));
  }
  rules.diagonals = false;
  Grid<TileRef> free{doc};
  free.assign({16, 12}, kEmptyTile);
  ASSERT_TRUE(solve(rules, WfcConstraints{}, WfcParams{.seed = 1}, free, scratch).status.ok);
  EXPECT_FALSE(corners_match(free));
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

namespace
{

/// Tiles of the auto-tiling rule sets below
enum : TileIndex
{
  kGrass = 0,
  kEdge = 1,
  kCenter = 2,
  /// A tile of another tileset, which the rules know nothing about
  kDecoration = 7,
};

constexpr TileRef tile_of(TileIndex tile) { return TileRef{0, tile}; }

/**
 * @brief Rules for a path (or a lake) in a field: an edge tile has to come between the two
 *
 * Grass, edge and center may each sit next to themselves and next to the one beside them, but grass
 * may never touch a center, so painting centers forces edges around them.
 */
WfcRules path_rules(memory::Arena& arena, bool diagonals)
{
  WfcRules rules{arena};
  rules.diagonals = diagonals;
  for (const TileIndex tile : {kGrass, kEdge, kCenter})
  {
    add_tile(rules, tile_of(tile));
  }
  // Grass is the most frequent tile, so it is what an empty cell falls back to
  rules.tiles[kGrass].weight = 10.0F;
  static constexpr std::pair<TileIndex, TileIndex> kPairs[] = {
    {kGrass, kGrass}, {kGrass, kEdge}, {kEdge, kEdge}, {kEdge, kCenter}, {kCenter, kCenter}};
  for (const auto& [a, b] : kPairs)
  {
    for (const Direction d : kNeighbors)
    {
      if (!diagonal(d) or diagonals)
      {
        set_allowed(rules, a, b, d, true);
        set_allowed(rules, b, a, d, true);
      }
    }
  }
  return rules;
}

/// A field of grass with <code>centers</code> painted into it
Grid<TileRef> field(memory::Arena& arena, Extents extents)
{
  Grid<TileRef> tiles{arena};
  tiles.assign(extents, tile_of(kGrass));
  return tiles;
}

}  // namespace

TEST_F(Procgen, AutoTileFillsEdgesAroundPaintedCenters)
{
  WfcRules rules = path_rules(doc, false);
  Grid<TileRef> tiles = field(doc, Extents{9, 9});
  const Cell painted{4, 4};
  tiles.at(painted) = tile_of(kCenter);

  const AutoTileResult result = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
  EXPECT_EQ(result.unresolved, 0U);
  EXPECT_EQ(result.changed, 4U);
  // The painted center stays, an edge comes between it and the grass, and the rest is untouched
  EXPECT_EQ(tiles.at(painted), tile_of(kCenter));
  for (const Direction d : kDirections)
  {
    EXPECT_EQ(tiles.at(step(painted, d)), tile_of(kEdge)) << "direction " << static_cast<int>(d);
  }
  EXPECT_EQ(tiles.at(3, 3), tile_of(kGrass));
  EXPECT_EQ(tiles.at(2, 4), tile_of(kGrass));
  EXPECT_EQ(tiles.at(0, 0), tile_of(kGrass));
}

TEST_F(Procgen, AutoTileRefillsAroundCentersPaintedLater)
{
  WfcRules rules = path_rules(doc, false);
  Grid<TileRef> tiles = field(doc, Extents{12, 5});
  // A path drawn one cell at a time: each stroke re-fills the band around the growing shape
  for (int x = 2; x <= 8; ++x)
  {
    const Cell painted{x, 2};
    tiles.at(painted) = tile_of(kCenter);
    const AutoTileResult result = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
    EXPECT_EQ(result.unresolved, 0U) << "at x = " << x;
  }
  for (int x = 2; x <= 8; ++x)
  {
    // Every center painted stayed a center, with edges above and below
    EXPECT_EQ(tiles.at(x, 2), tile_of(kCenter)) << "at x = " << x;
    EXPECT_EQ(tiles.at(x, 1), tile_of(kEdge)) << "at x = " << x;
    EXPECT_EQ(tiles.at(x, 3), tile_of(kEdge)) << "at x = " << x;
  }
  // The ends of the path are capped, and the field beyond the edges is untouched
  EXPECT_EQ(tiles.at(1, 2), tile_of(kEdge));
  EXPECT_EQ(tiles.at(9, 2), tile_of(kEdge));
  EXPECT_EQ(tiles.at(0, 2), tile_of(kGrass));
  EXPECT_EQ(tiles.at(5, 0), tile_of(kGrass));
  EXPECT_EQ(tiles.at(5, 4), tile_of(kGrass));
}

TEST_F(Procgen, AutoTileIsStableOnceTheRulesHold)
{
  WfcRules rules = path_rules(doc, false);
  Grid<TileRef> tiles = field(doc, Extents{9, 9});
  const Cell painted{4, 4};
  tiles.at(painted) = tile_of(kCenter);
  autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);

  const AutoTileResult again = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
  EXPECT_EQ(again.changed, 0U);
  EXPECT_EQ(again.unresolved, 0U);
  EXPECT_GT(again.considered, 0U);
}

TEST_F(Procgen, AutoTileFillsCornersWhenDiagonalsAreOn)
{
  WfcRules rules = path_rules(doc, true);
  Grid<TileRef> tiles = field(doc, Extents{9, 9});
  const Cell painted{4, 4};
  tiles.at(painted) = tile_of(kCenter);

  const AutoTileResult result = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
  EXPECT_EQ(result.unresolved, 0U);
  // Grass may not touch a center at a corner either, so the whole ring becomes edge
  for (const Direction d : kNeighbors)
  {
    EXPECT_EQ(tiles.at(step(painted, d)), tile_of(kEdge)) << "direction " << static_cast<int>(d);
  }
  EXPECT_EQ(tiles.at(2, 2), tile_of(kGrass));
}

TEST_F(Procgen, AutoTileLeavesTilesTheRulesDoNotKnow)
{
  WfcRules rules = path_rules(doc, false);
  Grid<TileRef> tiles = field(doc, Extents{9, 9});
  const Cell painted{4, 4};
  tiles.at(painted) = tile_of(kCenter);
  // Hand-placed decoration from another tileset, right where an edge would go
  const TileRef decoration = TileRef{1, kDecoration};
  tiles.at(4, 3) = decoration;
  // An empty cell, which has no tile worth keeping
  tiles.at(4, 5) = kEmptyTile;

  const AutoTileResult result = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
  EXPECT_EQ(tiles.at(4, 3), decoration);
  EXPECT_EQ(tiles.at(4, 5), tile_of(kEdge));
  // The decoration constrains nothing, so the cell beyond it is free to stay grass
  EXPECT_EQ(tiles.at(4, 2), tile_of(kGrass));
  EXPECT_EQ(result.unresolved, 0U);
}

TEST_F(Procgen, AutoTileReportsWhatTheRulesCannotExpress)
{
  // No edge tile: grass may not touch a center, and nothing can go between them
  WfcRules rules{doc};
  rules.diagonals = false;
  add_tile(rules, tile_of(kGrass));
  add_tile(rules, tile_of(kCenter));
  for (const Direction d : kDirections)
  {
    set_allowed(rules, 0, 0, d, true);
    set_allowed(rules, 1, 1, d, true);
  }
  Grid<TileRef> tiles = field(doc, Extents{7, 7});
  const Cell painted{3, 3};
  tiles.at(painted) = tile_of(kCenter);

  const AutoTileResult result = autotile(rules, &painted, 1, AutoTileParams{}, tiles, scratch);
  EXPECT_GT(result.unresolved, 0U);
  // The painted cell is never rewritten, whatever the rules say
  EXPECT_EQ(tiles.at(painted), tile_of(kCenter));
}

TEST_F(Procgen, AutoTileDoesNothingWithoutRulesOrPaintedCells)
{
  WfcRules rules = path_rules(doc, false);
  Grid<TileRef> tiles = field(doc, Extents{5, 5});
  const Cell painted{2, 2};
  EXPECT_EQ(autotile(rules, nullptr, 0, AutoTileParams{}, tiles, scratch).considered, 0U);
  EXPECT_EQ(autotile(rules, &painted, 1, AutoTileParams{.radius = 0}, tiles, scratch).considered, 0U);
  WfcRules empty{doc};
  EXPECT_EQ(autotile(empty, &painted, 1, AutoTileParams{}, tiles, scratch).considered, 0U);
  for (const TileRef& tile : tiles.values)
  {
    EXPECT_EQ(tile, tile_of(kGrass));
  }
}

namespace
{

enum : TileIndex
{
  /// The nine-slice occupies tiles 0..8 (reading across), the inner corners 9..12
  kNineSlice = 0,
  kInnerCorners = 9,
  /// What the map holds where the terrain is not
  kBackground = 20,
};

/// A terrain whose tiles are a nine-slice, optionally with the four inner corners
AutoTileRules terrain_rules(memory::Arena& arena, bool inner_corners, int expand)
{
  AutoTileRules rules{arena};
  AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, "path")];
  terrain.expand = expand;
  Grid<TileRef> layout{arena};
  layout.assign(Extents{3, 3}, kEmptyTile);
  for (int i = 0; i < 9; ++i)
  {
    layout.values[i] = TileRef{0, static_cast<TileIndex>(kNineSlice + i)};
  }
  learn_from_layout(terrain, layout);
  if (inner_corners)
  {
    Grid<TileRef> inner{arena};
    inner.assign(Extents{2, 2}, kEmptyTile);
    for (int i = 0; i < 4; ++i)
    {
      inner.values[i] = TileRef{0, static_cast<TileIndex>(kInnerCorners + i)};
    }
    learn_from_layout(terrain, inner);
  }
  return rules;
}

/// Tile of the nine-slice at (x, y) of its block
constexpr TileRef slice(int x, int y) { return TileRef{0, static_cast<TileIndex>(kNineSlice + y * 3 + x)}; }

Grid<TileRef> background(memory::Arena& arena, Extents extents)
{
  Grid<TileRef> tiles{arena};
  tiles.assign(extents, TileRef{0, kBackground});
  return tiles;
}

}  // namespace

TEST_F(Procgen, PaintTerrainLaysDownTheWholeNineSlice)
{
  const AutoTileRules rules = terrain_rules(doc, false, 1);
  Grid<TileRef> tiles = background(doc, Extents{11, 11});
  const Cell painted{5, 5};

  const TerrainPaintResult result = paint_terrain(rules, 0, &painted, 1, tiles, scratch);
  EXPECT_EQ(result.missing, 0U);
  EXPECT_EQ(result.covered, 9U);
  EXPECT_EQ(result.changed, 9U);
  // One painted cell becomes the middle, and the ring around it the edges and outer corners
  for (int y = 0; y < 3; ++y)
  {
    for (int x = 0; x < 3; ++x)
    {
      EXPECT_EQ(tiles.at(4 + x, 4 + y), slice(x, y)) << "at (" << x << ", " << y << ")";
    }
  }
  // Nothing beyond the border is touched
  EXPECT_EQ(tiles.at(3, 5), (TileRef{0, kBackground}));
  EXPECT_EQ(tiles.at(7, 5), (TileRef{0, kBackground}));
  EXPECT_EQ(tiles.at(0, 0), (TileRef{0, kBackground}));
}

TEST_F(Procgen, PaintTerrainRetilesTheBorderAsTheShapeGrows)
{
  const AutoTileRules rules = terrain_rules(doc, false, 1);
  Grid<TileRef> tiles = background(doc, Extents{14, 9});
  const Cell first{5, 4};
  paint_terrain(rules, 0, &first, 1, tiles, scratch);
  ASSERT_EQ(tiles.at(6, 3), slice(2, 0));

  // Painting the next cell of the path turns the old corner into a straight edge
  const Cell second{6, 4};
  const TerrainPaintResult result = paint_terrain(rules, 0, &second, 1, tiles, scratch);
  EXPECT_EQ(result.missing, 0U);
  EXPECT_EQ(tiles.at(6, 3), slice(1, 0));
  EXPECT_EQ(tiles.at(7, 3), slice(2, 0));
  EXPECT_EQ(tiles.at(5, 4), slice(1, 1));
  EXPECT_EQ(tiles.at(6, 4), slice(1, 1));
  EXPECT_EQ(tiles.at(4, 4), slice(0, 1));
  EXPECT_EQ(tiles.at(7, 4), slice(2, 1));
  EXPECT_EQ(tiles.at(6, 5), slice(1, 2));

  // Painting over what is already the middle changes nothing
  const TerrainPaintResult again = paint_terrain(rules, 0, &second, 1, tiles, scratch);
  EXPECT_EQ(again.changed, 0U);
}

TEST_F(Procgen, PaintTerrainDrawsInnerCornersWhereTheTerrainWraps)
{
  // No border to grow: the painted cells themselves take the edge tiles
  const AutoTileRules rules = terrain_rules(doc, true, 0);
  Grid<TileRef> tiles = background(doc, Extents{11, 11});
  // A 5x5 patch with one corner cell missing, so the cell inside that corner wraps around it
  auto painted = memory::make_vector<Cell>(doc);
  for (int y = 3; y <= 7; ++y)
  {
    for (int x = 3; x <= 7; ++x)
    {
      if (x != 3 or y != 3)
      {
        painted.push_back(Cell{x, y});
      }
    }
  }
  const TerrainPaintResult result = paint_terrain(rules, 0, painted.data(), painted.size(), tiles, scratch);
  EXPECT_EQ(result.missing, 0U);
  // Surrounded on every side but the north-west corner: the first of the inner-corner tiles
  EXPECT_EQ(tiles.at(4, 4), (TileRef{0, kInnerCorners}));
  // The middle of the patch is fully surrounded, and the hole was left alone
  EXPECT_EQ(tiles.at(5, 5), slice(1, 1));
  EXPECT_EQ(tiles.at(3, 3), (TileRef{0, kBackground}));
}

TEST_F(Procgen, PaintTerrainWithoutABorderLeavesNeighborsAlone)
{
  const AutoTileRules rules = terrain_rules(doc, false, 0);
  Grid<TileRef> tiles = background(doc, Extents{9, 9});
  const Cell painted{4, 4};

  const TerrainPaintResult result = paint_terrain(rules, 0, &painted, 1, tiles, scratch);
  EXPECT_EQ(result.covered, 1U);
  EXPECT_EQ(result.missing, 0U);
  // Only the painted cell is the terrain's, and with no like neighbors it takes the pattern for none
  EXPECT_NE(tiles.at(painted), (TileRef{0, kBackground}));
  for (const Direction d : kNeighbors)
  {
    EXPECT_EQ(tiles.at(step(painted, d)), (TileRef{0, kBackground})) << "direction " << static_cast<int>(d);
  }
}

TEST_F(Procgen, PaintTerrainTreatsTheMapEdgeAsTerrain)
{
  const AutoTileRules rules = terrain_rules(doc, false, 1);
  Grid<TileRef> tiles = background(doc, Extents{9, 9});
  const Cell painted{0, 0};

  paint_terrain(rules, 0, &painted, 1, tiles, scratch);
  // Nothing off the map to draw a border against, so the painted corner cell is fully surrounded
  // and the border only appears on the inward sides
  EXPECT_EQ(tiles.at(0, 0), slice(1, 1));
  EXPECT_EQ(tiles.at(1, 0), slice(2, 1));
  EXPECT_EQ(tiles.at(0, 1), slice(1, 2));
  EXPECT_EQ(tiles.at(1, 1), slice(2, 2));
  EXPECT_EQ(tiles.at(2, 0), (TileRef{0, kBackground}));
  EXPECT_EQ(tiles.at(0, 2), (TileRef{0, kBackground}));
}

TEST_F(Procgen, PaintTerrainReportsATerrainWithNoTiles)
{
  AutoTileRules rules{doc};
  add_terrain(rules, doc, "empty");
  Grid<TileRef> tiles = background(doc, Extents{7, 7});
  const Cell painted{3, 3};

  const TerrainPaintResult result = paint_terrain(rules, 0, &painted, 1, tiles, scratch);
  EXPECT_EQ(result.covered, 0U);
  EXPECT_EQ(result.changed, 0U);
  EXPECT_GT(result.missing, 0U);
  for (const TileRef& tile : tiles.values)
  {
    EXPECT_EQ(tile, (TileRef{0, kBackground}));
  }
  // An unknown terrain, or nothing painted, does nothing at all
  EXPECT_EQ(paint_terrain(rules, kNoTerrain, &painted, 1, tiles, scratch).missing, 0U);
  EXPECT_EQ(paint_terrain(rules, 0, nullptr, 0, tiles, scratch).missing, 0U);
}

TEST_F(Procgen, PaintTerrainScattersTheMiddlesOfAPattern)
{
  AutoTileRules rules = terrain_rules(doc, true, 1);
  AutoTerrain& terrain = rules.terrains[0];
  // Three middles instead of one: a painted area should use all of them, and keep what it has
  add_variant(terrain, kAllNeighbors, TileRef{0, 30});
  add_variant(terrain, kAllNeighbors, TileRef{0, 31});
  ASSERT_EQ(variant_count(terrain, kAllNeighbors), 3U);

  Grid<TileRef> tiles = background(doc, Extents{21, 21});
  auto painted = memory::make_vector<Cell>(doc);
  for (int y = 4; y <= 16; ++y)
  {
    for (int x = 4; x <= 16; ++x)
    {
      painted.push_back(Cell{x, y});
    }
  }
  const TerrainPaintResult result = paint_terrain(rules, 0, painted.data(), painted.size(), tiles, scratch);
  EXPECT_EQ(result.missing, 0U);
  std::array<int, 3> counts = {0, 0, 0};
  for (int y = 5; y <= 15; ++y)
  {
    for (int x = 5; x <= 15; ++x)
    {
      counts[0] += (tiles.at(x, y) == slice(1, 1)) ? 1 : 0;
      counts[1] += (tiles.at(x, y) == TileRef{0, 30}) ? 1 : 0;
      counts[2] += (tiles.at(x, y) == TileRef{0, 31}) ? 1 : 0;
    }
  }
  EXPECT_EQ(counts[0] + counts[1] + counts[2], 11 * 11);
  for (const int n : counts)
  {
    EXPECT_GT(n, 0) << "every middle should appear somewhere in a 11x11 area";
  }
  // Painting the area again leaves every cell exactly as it was
  Grid<TileRef> before{doc};
  before.assign(tiles.extents, kEmptyTile);
  before.values.assign(tiles.values.begin(), tiles.values.end());
  const TerrainPaintResult again = paint_terrain(rules, 0, painted.data(), painted.size(), tiles, scratch);
  EXPECT_EQ(again.changed, 0U);
  EXPECT_EQ(before.values, tiles.values);
}

namespace
{

/// Tiles 100.. , so they cannot be mistaken for anything else a test places
constexpr TileRef fill_tile(int n) { return TileRef{0, static_cast<TileIndex>(100 + n)}; }

/// A level-shaped set of planes: tiles, a heightmap rising with y, and a semantic layer in stripes
struct Field
{
  Field(memory::Arena& arena, Extents e) : tiles{arena}, height{arena}, ids{arena}
  {
    tiles.assign(e, kEmptyTile);
    height.assign(e, 0.0F);
    ids.assign(e, std::uint16_t{0});
    for (int y = 0; y < e.height; ++y)
    {
      for (int x = 0; x < e.width; ++x)
      {
        height.at(x, y) = static_cast<float>(y) / static_cast<float>(e.height - 1);
        ids.at(x, y) = static_cast<std::uint16_t>(x % 3);
      }
    }
  }
  Grid<TileRef> tiles;
  Grid<float> height;
  Grid<std::uint16_t> ids;
};

}  // namespace

TEST_F(Procgen, FillTakesOnlyTheHeightBand)
{
  Field f{doc, Extents{10, 11}};
  const TileRef one = fill_tile(0);
  // Rows 0..10 hold heights 0.0 .. 1.0 in steps of 0.1, so [0.3, 0.5] is rows 3, 4 and 5
  const FillMask mask{.band = &f.height, .lo = 0.3F, .hi = 0.5F};
  const FillResult result = fill_masked(&one, nullptr, 1, mask, 1, f.tiles);
  EXPECT_EQ(result.matched, 30U);
  EXPECT_EQ(result.changed, 30U);
  for (int y = 0; y < 11; ++y)
  {
    for (int x = 0; x < 10; ++x)
    {
      const bool inside = (y >= 3 and y <= 5);
      EXPECT_EQ(f.tiles.at(x, y), inside ? one : kEmptyTile) << "at (" << x << ", " << y << ")";
    }
  }
  // Filling again writes the same thing, so nothing changes
  EXPECT_EQ(fill_masked(&one, nullptr, 1, mask, 1, f.tiles).changed, 0U);
  // A band given the wrong way round matches nothing
  const FillMask empty{.band = &f.height, .lo = 0.5F, .hi = 0.3F};
  EXPECT_EQ(fill_masked(&one, nullptr, 1, empty, 1, f.tiles).matched, 0U);
}

TEST_F(Procgen, FillTakesOnlyTheGroundType)
{
  Field f{doc, Extents{9, 4}};
  const TileRef one = fill_tile(0);
  const FillResult result = fill_masked(&one, nullptr, 1, FillMask{.ids = &f.ids, .id = 2}, 1, f.tiles);
  EXPECT_EQ(result.matched, 12U);
  for (int y = 0; y < 4; ++y)
  {
    for (int x = 0; x < 9; ++x)
    {
      EXPECT_EQ(f.tiles.at(x, y), (x % 3 == 2) ? one : kEmptyTile);
    }
  }
}

TEST_F(Procgen, FillIntersectsItsMasks)
{
  Field f{doc, Extents{9, 11}};
  const TileRef one = fill_tile(0);
  // Both tests must pass: rows 3..5 of the columns whose semantic is 1
  const FillMask mask{.band = &f.height, .lo = 0.3F, .hi = 0.5F, .ids = &f.ids, .id = 1};
  EXPECT_EQ(fill_masked(&one, nullptr, 1, mask, 1, f.tiles).matched, 9U);
  for (int y = 0; y < 11; ++y)
  {
    for (int x = 0; x < 9; ++x)
    {
      const bool inside = (y >= 3 and y <= 5) and (x % 3 == 1);
      EXPECT_EQ(f.tiles.at(x, y), inside ? one : kEmptyTile) << "at (" << x << ", " << y << ")";
    }
  }
  // With no mask at all, every cell is matched
  Field all{doc, Extents{9, 11}};
  EXPECT_EQ(fill_masked(&one, nullptr, 1, FillMask{}, 1, all.tiles).matched, 99U);
}

TEST_F(Procgen, FillScattersSeveralTilesAndKeepsThemPut)
{
  Field f{doc, Extents{32, 32}};
  const TileRef tiles[3] = {fill_tile(0), fill_tile(1), fill_tile(2)};
  const FillResult result = fill_masked(tiles, nullptr, 3, FillMask{}, 7, f.tiles);
  EXPECT_EQ(result.matched, 1024U);
  int counts[3] = {0, 0, 0};
  for (const TileRef& t : f.tiles.values)
  {
    for (int i = 0; i < 3; ++i)
    {
      counts[i] += (t == tiles[i]) ? 1 : 0;
    }
  }
  EXPECT_EQ(counts[0] + counts[1] + counts[2], 1024);
  for (const int n : counts)
  {
    // Roughly even, without pinning the hash down
    EXPECT_GT(n, 1024 / 6) << "every tile should be scattered through the area";
  }
  // The same seed writes the same thing, however often it is run
  Grid<TileRef> before{doc};
  before.assign(f.tiles.extents, kEmptyTile);
  before.values.assign(f.tiles.values.begin(), f.tiles.values.end());
  EXPECT_EQ(fill_masked(tiles, nullptr, 3, FillMask{}, 7, f.tiles).changed, 0U);
  EXPECT_EQ(before.values, f.tiles.values);
  // A different seed moves them, and a cell's tile does not depend on what else was filled
  EXPECT_GT(fill_masked(tiles, nullptr, 3, FillMask{}, 8, f.tiles).changed, 0U);
  Field narrow{doc, Extents{32, 32}};
  fill_masked(tiles, nullptr, 3, FillMask{.band = &narrow.height, .lo = 0.3F, .hi = 0.5F}, 7, narrow.tiles);
  for (int y = 10; y <= 15; ++y)
  {
    for (int x = 0; x < 32; ++x)
    {
      EXPECT_EQ(narrow.tiles.at(x, y), before.at(x, y)) << "at (" << x << ", " << y << ")";
    }
  }
}

TEST_F(Procgen, FillStaysInsideItsRegionAndSparesWhatIsDrawn)
{
  Field f{doc, Extents{12, 12}};
  const TileRef one = fill_tile(0);
  const TileRef drawn = fill_tile(9);
  f.tiles.at(5, 5) = drawn;
  const FillResult result =
    fill_masked(&one, nullptr, 1, FillMask{.region = Region{.x = 4, .y = 4, .width = 3, .height = 3}}, 1, f.tiles);
  EXPECT_EQ(result.matched, 9U);
  EXPECT_EQ(f.tiles.at(4, 4), one);
  EXPECT_EQ(f.tiles.at(6, 6), one);
  EXPECT_EQ(f.tiles.at(3, 4), kEmptyTile);
  EXPECT_EQ(f.tiles.at(7, 4), kEmptyTile);
  // A region hanging off the grid is clipped, not an error
  EXPECT_EQ(
    fill_masked(&one, nullptr, 1, FillMask{.region = Region{.x = 10, .y = 10, .width = 8, .height = 8}}, 1, f.tiles)
      .matched,
    4U);

  // Only the empty cells, so what is already drawn survives
  Field keep{doc, Extents{12, 12}};
  keep.tiles.at(5, 5) = drawn;
  EXPECT_EQ(fill_masked(&one, nullptr, 1, FillMask{.only_empty = true}, 1, keep.tiles).matched, 143U);
  EXPECT_EQ(keep.tiles.at(5, 5), drawn);
  EXPECT_EQ(keep.tiles.at(5, 6), one);
}

TEST_F(Procgen, FillWithoutTilesWritesNothing)
{
  Field f{doc, Extents{4, 4}};
  const TileRef one = fill_tile(0);
  EXPECT_EQ(fill_masked(nullptr, nullptr, 0, FillMask{}, 1, f.tiles).matched, 0U);
  EXPECT_EQ(fill_masked(&one, nullptr, 0, FillMask{}, 1, f.tiles).matched, 0U);
  for (const TileRef& t : f.tiles.values)
  {
    EXPECT_EQ(t, kEmptyTile);
  }
  Grid<TileRef> none{doc};
  none.assign(Extents{0, 0}, kEmptyTile);
  EXPECT_EQ(fill_masked(&one, nullptr, 1, FillMask{}, 1, none).matched, 0U);
}

TEST_F(Procgen, FillWeightsDecideTheShares)
{
  Field f{doc, Extents{64, 64}};
  const TileRef tiles[3] = {fill_tile(0), fill_tile(1), fill_tile(2)};
  const auto shares = [&](const float* weights) {
    fill_masked(tiles, weights, 3, FillMask{}, 11, f.tiles);
    std::array<int, 3> out{0, 0, 0};
    for (const TileRef& t : f.tiles.values)
    {
      for (int i = 0; i < 3; ++i)
      {
        out[static_cast<std::size_t>(i)] += (t == tiles[i]) ? 1 : 0;
      }
    }
    EXPECT_EQ(out[0] + out[1] + out[2], 64 * 64);
    return out;
  };
  // Six to one to one: the first tile takes about three quarters of the cells
  const float steep[3] = {6.0F, 1.0F, 1.0F};
  const auto weighted = shares(steep);
  EXPECT_NEAR(static_cast<double>(weighted[0]) / (64 * 64), 0.75, 0.03);
  EXPECT_NEAR(static_cast<double>(weighted[1]) / (64 * 64), 0.125, 0.03);
  EXPECT_NEAR(static_cast<double>(weighted[2]) / (64 * 64), 0.125, 0.03);

  // A weight of zero keeps a tile out of the scatter altogether
  const float without[3] = {1.0F, 0.0F, 3.0F};
  const auto some = shares(without);
  EXPECT_EQ(some[1], 0);
  EXPECT_NEAR(static_cast<double>(some[2]) / (64 * 64), 0.75, 0.03);

  // Negative weights count as zero, and weights that are all zero leave the scatter even
  const float negative[3] = {1.0F, -5.0F, 1.0F};
  EXPECT_EQ(shares(negative)[1], 0);
  const float none[3] = {0.0F, 0.0F, 0.0F};
  const auto even = shares(none);
  for (const int n : even)
  {
    EXPECT_NEAR(static_cast<double>(n) / (64 * 64), 1.0 / 3.0, 0.03);
  }
  // Equal weights and no weights at all come to the same thing
  const float ones[3] = {1.0F, 1.0F, 1.0F};
  fill_masked(tiles, ones, 3, FillMask{}, 11, f.tiles);
  Grid<TileRef> flat{doc};
  flat.assign(f.tiles.extents, kEmptyTile);
  flat.values.assign(f.tiles.values.begin(), f.tiles.values.end());
  EXPECT_EQ(fill_masked(tiles, nullptr, 3, FillMask{}, 11, f.tiles).changed, 0U);
  EXPECT_EQ(flat.values, f.tiles.values);
}

namespace
{

/// A code where the listed neighbors (NW, N, NE, W, E, SW, S, SE) are below and the rest level
StepCode below(std::initializer_list<std::size_t> neighbors)
{
  StepCode code = 0;
  for (const std::size_t n : neighbors)
  {
    code = step_code_with(code, n, StepRelation::kBelow);
  }
  return code;
}

/// Levels laid out from a picture, one digit per cell
Grid<std::uint16_t> levels_of(memory::Arena& arena, std::initializer_list<const char*> rows)
{
  Grid<std::uint16_t> g{arena};
  const auto height = static_cast<int>(rows.size());
  const auto width = static_cast<int>(std::string_view{*rows.begin()}.size());
  g.assign(Extents{width, height}, std::uint16_t{0});
  int y = 0;
  for (const char* row : rows)
  {
    for (int x = 0; x < width; ++x)
    {
      g.at(x, y) = static_cast<std::uint16_t>(row[x] - '0');
    }
    ++y;
  }
  return g;
}

/**
 * @brief One terrain per level, each drawing the ground at its level and above
 *
 * A terrain's rim comes from the edge of its own upper set, so one terrain covering every level
 * would be a single flat sheet with no cliffs anywhere. The terraces are separate terrains on
 * separate layers, which is also how they have to be drawn: a cliff tile is transparent where the
 * terrace below shows through it.
 *
 * Terrain k's tiles are numbered k * 18 + the step type, so a tile says which terrace drew it.
 */
StepRules per_level_rules(memory::Arena& arena, int levels)
{
  StepRules rules{arena};
  for (int k = 0; k < levels; ++k)
  {
    StepTerrain& t = rules.terrains[add_step_terrain(rules, format<32>("level_%d", k))];
    t.level_lo = k;
    t.level_hi = k;
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      t.tiles[i] = TileRef{0, static_cast<TileIndex>(k * static_cast<int>(kStepTypeCount) + i)};
    }
  }
  return rules;
}

/// The step type a tile holds, whichever terrace drew it
StepType type_of(TileRef tile) { return static_cast<StepType>(tile.tile % kStepTypeCount); }

/// One tile layer per terrace, which is how they have to be drawn
struct Terraces
{
  Terraces(memory::Arena& arena, Extents e, std::size_t n) :
      grids{memory::ArenaAllocator<Grid<TileRef>>{arena}}, ptrs{memory::ArenaAllocator<Grid<TileRef>*>{arena}}
  {
    grids.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
      grids.emplace_back(arena);
      grids.back().assign(e, kEmptyTile);
    }
    for (Grid<TileRef>& g : grids)
    {
      ptrs.push_back(&g);
    }
  }
  memory::Vector<Grid<TileRef>> grids;
  memory::Vector<Grid<TileRef>*> ptrs;
};

StepFillResult draw(
  const StepRules& rules,
  const Grid<std::uint16_t>& levels,
  StepFillParams params,
  std::initializer_list<Grid<TileRef>*> layers,
  Grid<std::uint8_t>* mask = nullptr)
{
  return tile_steps(rules, levels, params, layers.begin(), layers.size(), mask);
}

}  // namespace

TEST(Steps, TheTableMatchesGft)
{
  // Every case of gft's getSlopeFromBumpCode, by the neighbors it has below it
  EXPECT_EQ(step_type_of(below({0, 1, 2, 3, 5})), StepType::kOutwardTopRight);
  EXPECT_EQ(step_type_of(below({0, 1, 2})), StepType::kOutwardTop);
  EXPECT_EQ(step_type_of(below({0, 1, 2, 4, 7})), StepType::kOutwardTopLeft);
  EXPECT_EQ(step_type_of(below({7})), StepType::kInwardBottomRight);
  EXPECT_EQ(step_type_of(below({5, 6})), StepType::kInwardBottom);
  EXPECT_EQ(step_type_of(below({6, 7})), StepType::kInwardBottom);
  EXPECT_EQ(step_type_of(below({5})), StepType::kInwardBottomLeft);
  EXPECT_EQ(step_type_of(below({0, 3, 5})), StepType::kOutwardLeft);
  EXPECT_EQ(step_type_of(below({2, 4, 7})), StepType::kOutwardRight);
  EXPECT_EQ(step_type_of(below({2, 4})), StepType::kInwardRight);
  EXPECT_EQ(step_type_of(below({4, 7})), StepType::kInwardRight);
  EXPECT_EQ(step_type_of(below({0, 3})), StepType::kInwardLeft);
  EXPECT_EQ(step_type_of(below({3, 5})), StepType::kInwardLeft);
  EXPECT_EQ(step_type_of(below({0, 3, 5, 6, 7})), StepType::kOutwardBottomLeft);
  EXPECT_EQ(step_type_of(below({5, 6, 7})), StepType::kOutwardBottom);
  EXPECT_EQ(step_type_of(below({2, 4, 5, 6, 7})), StepType::kOutwardBottomRight);
  EXPECT_EQ(step_type_of(below({2})), StepType::kInwardTopRight);
  EXPECT_EQ(step_type_of(below({1, 2})), StepType::kInwardTop);
  EXPECT_EQ(step_type_of(below({0, 1})), StepType::kInwardTop);
  EXPECT_EQ(step_type_of(below({0})), StepType::kInwardTopLeft);
  // Level on every side, and anything the table does not name, is flat
  EXPECT_EQ(step_type_of(below({})), StepType::kFlat);
  EXPECT_EQ(step_type_of(below({1, 6})), StepType::kFlat);
  EXPECT_EQ(step_type_of(below({0, 1, 2, 3, 4, 5, 6, 7})), StepType::kFlat);
  // A neighbor above means this cell is at the foot of a step, which is never drawn
  for (std::size_t n = 0; n < 8; ++n)
  {
    const StepCode code = step_code_with(below({0, 1, 2}), n, StepRelation::kAbove);
    EXPECT_EQ(step_type_of(code), StepType::kFlat) << "with neighbor " << n << " above";
  }
}

TEST_F(Procgen, StepsDrawTheRimOfAPlateauOnItsOwnLayer)
{
  const StepRules rules = per_level_rules(doc, 2);
  // A 3x3 plateau of level 1 in a field of level 0
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01110", "00000"});
  Grid<TileRef> ground{doc};
  Grid<TileRef> upper{doc};
  ground.assign(levels.extents, kEmptyTile);
  upper.assign(levels.extents, kEmptyTile);

  const StepFillResult result = draw(rules, levels, StepFillParams{}, {&ground, &upper});
  EXPECT_EQ(result.missing, 0U);
  EXPECT_EQ(result.untermed, 0U);
  // The bottom terrace covers the whole map and is flat throughout: it has no edge to show
  EXPECT_EQ(result.tiled, 25U + 9U);
  for (int y = 0; y < 5; ++y)
  {
    for (int x = 0; x < 5; ++x)
    {
      EXPECT_EQ(type_of(ground.at(x, y)), StepType::kFlat) << "at (" << x << ", " << y << ")";
    }
  }
  // The upper terrace holds its nine-slice, and nothing at all outside itself, so what shows
  // through its transparent parts is the terrace below
  for (int y = 0; y < 3; ++y)
  {
    for (int x = 0; x < 3; ++x)
    {
      static constexpr StepType kSlice[3][3] = {
        {StepType::kOutwardTopRight, StepType::kOutwardTop, StepType::kOutwardTopLeft},
        {StepType::kOutwardLeft, StepType::kFlat, StepType::kOutwardRight},
        {StepType::kOutwardBottomLeft, StepType::kOutwardBottom, StepType::kOutwardBottomRight}};
      EXPECT_EQ(type_of(upper.at(1 + x, 1 + y)), kSlice[y][x]) << "at (" << x << ", " << y << ")";
    }
  }
  for (int x = 0; x < 5; ++x)
  {
    EXPECT_TRUE(upper.at(x, 0).empty()) << "at (" << x << ", 0)";
    EXPECT_TRUE(upper.at(x, 4).empty()) << "at (" << x << ", 4)";
  }
  // Each terrace's tiles come from its own terrain
  EXPECT_EQ(ground.at(0, 0), (TileRef{0, static_cast<TileIndex>(StepType::kFlat)}));
  EXPECT_EQ(upper.at(2, 2), (TileRef{0, static_cast<TileIndex>(kStepTypeCount + 7)}));

  // Drawing again changes nothing, and lowering the plateau clears the upper layer behind it
  EXPECT_EQ(draw(rules, levels, StepFillParams{}, {&ground, &upper}).changed, 0U);
  for (std::uint16_t& v : levels.values)
  {
    v = 0;
  }
  const StepFillResult flattened = draw(rules, levels, StepFillParams{}, {&ground, &upper});
  EXPECT_GT(flattened.changed, 0U);
  for (const TileRef& t : upper.values)
  {
    EXPECT_TRUE(t.empty()) << "the upper layer should be cleared where its terrace has gone";
  }
}

TEST_F(Procgen, StepsDrawInwardCornersWhereTheGroundWraps)
{
  const StepRules rules = per_level_rules(doc, 2);
  // A plateau with one corner cell missing, so the ground wraps around a hole
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01100", "00000"});
  Grid<TileRef> ground{doc};
  Grid<TileRef> upper{doc};
  ground.assign(levels.extents, kEmptyTile);
  upper.assign(levels.extents, kEmptyTile);
  draw(rules, levels, StepFillParams{}, {&ground, &upper});
  // (2, 2) has only its SE neighbor below -- the hole at (3, 3) -- which is the concave corner
  EXPECT_EQ(type_of(upper.at(2, 2)), StepType::kInwardBottomRight);
  EXPECT_EQ(type_of(upper.at(2, 1)), StepType::kOutwardTop);
  // The hole itself is not part of the upper terrace, so the terrace below shows through
  EXPECT_TRUE(upper.at(3, 3).empty());
  EXPECT_EQ(type_of(ground.at(3, 3)), StepType::kFlat);
}

TEST_F(Procgen, StepsTakeTheMapEdgeAsLevelGround)
{
  const StepRules rules = per_level_rules(doc, 2);
  // A plateau running off the left edge: no cliff is drawn along the edge itself
  Grid<std::uint16_t> levels = levels_of(doc, {"11000", "11000", "11000"});
  Grid<TileRef> ground{doc};
  Grid<TileRef> upper{doc};
  ground.assign(levels.extents, kEmptyTile);
  upper.assign(levels.extents, kEmptyTile);
  draw(rules, levels, StepFillParams{}, {&ground, &upper});
  EXPECT_EQ(type_of(upper.at(0, 1)), StepType::kFlat);
  EXPECT_EQ(type_of(upper.at(1, 1)), StepType::kOutwardRight);
}

TEST_F(Procgen, StepLayersAreOnePerLevelAndTerrainsOnlySayHow)
{
  // The layers follow the levels; a terrain only says which tiles a level is drawn in. One
  // terrain covering every level is the ordinary case, and two let the art change with height.
  StepRules rules{doc};
  StepTerrain& water = rules.terrains[add_step_terrain(rules, "water")];
  water.level_lo = 0;
  water.level_hi = 0;
  water.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{0, 100};
  StepTerrain& grass = rules.terrains[add_step_terrain(rules, "grass")];
  grass.level_lo = 1;
  grass.level_hi = 3;
  grass.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{0, 200};
  grass.tiles[static_cast<std::size_t>(StepType::kOutwardTop)] = TileRef{0, 201};

  // A 3x3 plateau, so it has a middle as well as a rim
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01110", "00000"});
  Terraces drawn{doc, levels.extents, 2};
  const StepFillResult result =
    tile_steps(rules, levels, StepFillParams{}, drawn.ptrs.data(), drawn.ptrs.size(), nullptr);
  EXPECT_EQ(result.untermed, 0U);
  // Level 0's layer covers the whole map in the water terrain's flat tile: the opaque base
  for (const TileRef& t : drawn.grids[0].values)
  {
    EXPECT_EQ(t, (TileRef{0, 100}));
  }
  // Level 1's layer holds only the raised ground, in the grass terrain's tiles
  EXPECT_EQ(drawn.grids[1].at(2, 1), (TileRef{0, 201}));
  EXPECT_EQ(drawn.grids[1].at(2, 2), (TileRef{0, 200}));
  EXPECT_TRUE(drawn.grids[1].at(0, 0).empty()) << "nothing below the terrace, so the base shows through";
  EXPECT_GT(result.missing, 0U) << "the corners have no tile, which is reported rather than guessed";

  // One terrain covering every level draws every terrace in the same eighteen tiles
  const StepRules single = [&] {
    StepRules r{doc};
    StepTerrain& all = r.terrains[add_step_terrain(r, "ground")];
    all.level_lo = 0;
    all.level_hi = 255;
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      all.tiles[i] = TileRef{0, static_cast<TileIndex>(i)};
    }
    return r;
  }();
  Terraces one{doc, levels.extents, 2};
  tile_steps(single, levels, StepFillParams{}, one.ptrs.data(), one.ptrs.size(), nullptr);
  EXPECT_EQ(type_of(one.grids[0].at(0, 0)), StepType::kFlat);
  EXPECT_EQ(type_of(one.grids[1].at(2, 1)), StepType::kOutwardTop) << "the terrace above still gets its cliff";
  EXPECT_TRUE(one.grids[1].at(0, 0).empty());

  // A level no terrain covers is left undrawn and counted, and a null layer is simply skipped
  StepRules high{doc};
  StepTerrain& top = high.terrains[add_step_terrain(high, "top")];
  top.level_lo = 1;
  top.level_hi = 3;
  top.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{0, 300};
  Terraces sparse{doc, levels.extents, 2};
  const StepFillResult gap =
    tile_steps(high, levels, StepFillParams{}, sparse.ptrs.data(), sparse.ptrs.size(), nullptr);
  EXPECT_EQ(gap.untermed, 16U) << "every cell at level 0, which no terrain covers";
  for (const TileRef& t : sparse.grids[0].values)
  {
    EXPECT_TRUE(t.empty()) << "level 0 has no art, so its layer stays empty";
  }
  EXPECT_EQ(sparse.grids[1].at(2, 2), (TileRef{0, 300}));
  Grid<TileRef>* skipped[2] = {nullptr, &sparse.grids[1]};
  EXPECT_EQ(tile_steps(high, levels, StepFillParams{}, skipped, 2, nullptr).changed, 0U);
}

TEST_F(Procgen, StepCollisionStaysOneFlatLayer)
{
  const StepRules rules = per_level_rules(doc, 2);
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01110", "00000"});
  Grid<TileRef> ground{doc};
  Grid<TileRef> upper{doc};
  ground.assign(levels.extents, kEmptyTile);
  upper.assign(levels.extents, kEmptyTile);
  Grid<std::uint8_t> mask{doc};
  mask.assign(levels.extents, std::uint8_t{0});

  // Below: the foot of the cliff is blocked and the plateau stays walkable
  draw(rules, levels, StepFillParams{.collision = StepCollision::kBelow}, {&ground, &upper}, &mask);
  EXPECT_EQ(mask.at(0, 0), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 0), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 1), 0);
  EXPECT_EQ(mask.at(2, 2), 0);

  // Above: the rim is blocked instead, and the old marks are cleared rather than left behind
  draw(rules, levels, StepFillParams{.collision = StepCollision::kAbove}, {&ground, &upper}, &mask);
  EXPECT_EQ(mask.at(0, 0), 0);
  EXPECT_EQ(mask.at(2, 1), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 2), 0);

  // Other bits of the mask are left alone
  mask.at(0, 0) = level::collision::kTrigger;
  draw(rules, levels, StepFillParams{.collision = StepCollision::kBelow}, {&ground, &upper}, &mask);
  EXPECT_EQ(mask.at(0, 0), level::collision::kTrigger | level::collision::kSolid);
  // And nothing is touched at all when the collision layer is not asked for
  draw(rules, levels, StepFillParams{}, {&ground, &upper}, &mask);
  EXPECT_EQ(mask.at(2, 1), 0);
  // The mask is one flat layer whatever the tiles are spread over: drawing with no tile layers at
  // all still fills it
  draw(rules, levels, StepFillParams{.collision = StepCollision::kBelow}, {nullptr, nullptr}, &mask);
  EXPECT_EQ(mask.at(0, 0), level::collision::kTrigger | level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 2), 0);
}

TEST_F(Procgen, TidyRemovesSpursAndNotches)
{
  // A plateau with a one-cell spur poking out of its east side and a one-cell bite out of its west
  Grid<std::uint16_t> levels = levels_of(doc, {"0000000", "0111000", "0111100", "0011000", "0111000", "0000000"});
  const TidyResult result = tidy_levels(levels, TidyParams{}, scratch);
  EXPECT_TRUE(result.stable);
  EXPECT_GT(result.changed, 0U);
  // The spur is gone and the bite is filled, and the plateau is still there
  EXPECT_EQ(levels.at(4, 2), 0) << "the one-cell spur should go";
  EXPECT_EQ(levels.at(2, 3), 1) << "the one-cell bite should be filled";
  EXPECT_EQ(levels.at(2, 2), 1) << "the body of the plateau should stay";
  std::size_t high = 0;
  for (const std::uint16_t v : levels.values)
  {
    high += (v > 0) ? 1U : 0U;
  }
  EXPECT_GE(high, 8U) << "squaring the plateau off should not swallow it";
  // And what is left is a shape the tiles can draw
  const StepRules rules = per_level_rules(doc, 4);
  Terraces drawn{doc, levels.extents, 4};
  EXPECT_EQ(tile_steps(rules, levels, StepFillParams{}, drawn.ptrs.data(), drawn.ptrs.size(), nullptr).unexpressed, 0U);
  // Running it again settles immediately and changes nothing
  const TidyResult again = tidy_levels(levels, TidyParams{}, scratch);
  EXPECT_EQ(again.changed, 0U);
  EXPECT_TRUE(again.stable);
}

TEST_F(Procgen, TidyKeepsTheMapEdgeAndSeveralLevels)
{
  // Terraces running off the edge are not shaved back
  Grid<std::uint16_t> levels = levels_of(doc, {"2211", "2211", "1111", "1111"});
  tidy_levels(levels, TidyParams{}, scratch);
  EXPECT_EQ(levels.at(0, 0), 2) << "a terrace running off the edge should not be shaved back";
  EXPECT_EQ(levels.at(1, 1), 2);
  EXPECT_EQ(levels.at(3, 3), 1);

  // A single high cell on a middle terrace goes, and the hole wide enough to keep stays
  Grid<std::uint16_t> stack =
    levels_of(doc, {"11111111", "11211111", "11111111", "11000011", "11000011", "11000011", "11000011", "11111111"});
  tidy_levels(stack, TidyParams{}, scratch);
  EXPECT_EQ(stack.at(2, 1), 1) << "the lone level-2 cell should be shaved off";
  EXPECT_EQ(stack.at(3, 4), 0) << "a hole well wider than the lattice is kept";
  EXPECT_EQ(stack.at(0, 0), 1) << "the ground around it should stay where it is";
  // A lone high cell in a corner is squared off rather than left as a shape with no tile: the
  // repair grows it until the table has an answer
  Grid<std::uint16_t> corner = levels_of(doc, {"112", "111", "111"});
  const TidyParams repair_only{.remove_spurs = false, .fill_notches = false};
  EXPECT_GT(tidy_levels(corner, repair_only, scratch).changed, 0U);
  EXPECT_EQ(corner.at(2, 0), 2) << "the repair only ever raises, so the high cell stays high";
  EXPECT_EQ(tidy_levels(corner, repair_only, scratch).changed, 0U) << "and then it is settled";

  // With every pass turned off nothing moves at all
  Grid<std::uint16_t> untouched = levels_of(doc, {"112", "111", "111"});
  const TidyParams nothing{.remove_spurs = false, .fill_notches = false, .square_corners = false};
  EXPECT_EQ(tidy_levels(untouched, nothing, scratch).changed, 0U);
  EXPECT_EQ(untouched.at(2, 0), 2);
  EXPECT_EQ(untouched.at(0, 0), 1);
}

TEST_F(Procgen, TidyMakesEveryPatternOneTheTableCanDraw)
{
  // The point of the pass. gft's table names 20 of the 255 ways a cell can stand above its
  // neighbors, so a terraced heightmap is full of shapes it cannot draw: one-cell spurs and
  // notches, and 45-degree boundaries, which a staircase of single cells is. Tidying squares them
  // off, and what is left is counted rather than guessed at.
  for (const std::uint64_t seed : {1234U, 7U, 99U})
  {
    for (const int bands : {3, 5, 8})
    {
      Grid<float> height{doc};
      height.assign(Extents{64, 64}, 0.0F);
      generate_heightmap(HeightmapParams{}, seed, height, nullptr, 1);
      Grid<std::uint16_t> levels{doc};
      levels.assign(height.extents, std::uint16_t{0});
      terrace(height, TerraceParams{.levels = bands}, levels);
      const StepRules rules = per_level_rules(doc, bands);
      Terraces drawn{doc, height.extents, static_cast<std::size_t>(bands)};
      const auto tile = [&] {
        return tile_steps(rules, levels, StepFillParams{}, drawn.ptrs.data(), drawn.ptrs.size(), nullptr);
      };

      const StepFillResult before = tile();
      EXPECT_GT(before.unexpressed, 0U) << "a raw terracing should hold shapes with no tile";

      const TidyResult tidied = tidy_levels(levels, TidyParams{}, scratch);
      EXPECT_TRUE(tidied.stable) << "seed " << seed << ", " << bands << " bands";
      const StepFillResult after = tile();
      EXPECT_EQ(after.unexpressed, 0U) << "seed " << seed << ", " << bands << " bands";
      EXPECT_EQ(after.missing, 0U);
      // The repair on its own is a fixed point: there is nothing left for it to square off. The
      // smoothing is not, because the repair may leave a one-cell notch -- which the inward corner
      // tiles draw perfectly well -- and a two-cell smoothing would fill it again
      const TidyParams repair_only{.remove_spurs = false, .fill_notches = false};
      EXPECT_EQ(tidy_levels(levels, repair_only, scratch).changed, 0U) << "seed " << seed;
      EXPECT_EQ(tile().unexpressed, 0U);
    }
  }
}

TEST_F(Procgen, TidyLeavesTheShapeRecognisable)
{
  // Squaring a staircase off should not swallow the terrain: the levels still have to follow the
  // heightmap, and the range it covers should not collapse
  Grid<float> height{doc};
  height.assign(Extents{64, 64}, 0.0F);
  generate_heightmap(HeightmapParams{}, 1234, height, nullptr, 1);
  Grid<std::uint16_t> levels{doc};
  levels.assign(height.extents, std::uint16_t{0});
  terrace(height, TerraceParams{.levels = 5}, levels);
  auto before = memory::make_vector<std::uint16_t>(doc);
  before.assign(levels.values.begin(), levels.values.end());

  tidy_levels(levels, TidyParams{}, scratch);
  int top_before = 0;
  int top_after = 0;
  std::size_t same = 0;
  std::size_t raised = 0;
  for (std::size_t i = 0; i < levels.values.size(); ++i)
  {
    top_before = std::max(top_before, static_cast<int>(before[i]));
    top_after = std::max(top_after, static_cast<int>(levels.values[i]));
    same += (before[i] == levels.values[i]) ? 1U : 0U;
    raised += (levels.values[i] > before[i]) ? 1U : 0U;
  }
  EXPECT_EQ(top_after, top_before) << "the range of levels should survive";
  EXPECT_GT(same * 10, levels.values.size() * 9) << "nine cells in ten should keep the level they had";
  EXPECT_GT(raised, 0U);
  // No cell moves by more than one level, and the average move is a small fraction of one. This is
  // what squaring terraces off a block at a time buys: growing cells one at a time until the table
  // is happy instead cascades along every diagonal and floods whole valleys
  long total = 0;
  int worst = 0;
  for (std::size_t i = 0; i < levels.values.size(); ++i)
  {
    const int d = std::abs(static_cast<int>(levels.values[i]) - static_cast<int>(before[i]));
    total += d;
    worst = std::max(worst, d);
  }
  EXPECT_LE(worst, 1);
  EXPECT_LT(static_cast<double>(total) / static_cast<double>(levels.values.size()), 0.15);
}

TEST_F(Procgen, TerraceSkewMakesTheUpperBandsThinner)
{
  // A ramp from 0 to 1, so the number of cells in a band is that band's height
  Grid<float> height{doc};
  height.assign(Extents{1000, 1}, 0.0F);
  for (int x = 0; x < 1000; ++x)
  {
    height.at(x, 0) = static_cast<float>(x) / 999.0F;
  }
  Grid<std::uint16_t> levels{doc};
  levels.assign(height.extents, std::uint16_t{0});

  const auto widths = [&](float skew) {
    const TerraceResult r = terrace(height, TerraceParams{.levels = 4, .skew = skew}, levels);
    EXPECT_EQ(r.top, 3) << "skew " << skew;
    std::array<int, 4> counts{};
    for (const std::uint16_t v : levels.values)
    {
      counts[std::min<std::size_t>(v, 3)]++;
    }
    EXPECT_EQ(counts[0] + counts[1] + counts[2] + counts[3], 1000);
    return counts;
  };

  // A skew of 1 is the equal bands it was before
  const auto equal = widths(1.0F);
  for (const int c : equal)
  {
    EXPECT_NEAR(c, 250, 2);
  }

  // Below 1, each band is thinner than the one below it, and they still divide the range exactly
  const auto thinning = widths(0.5F);
  for (std::size_t i = 1; i < thinning.size(); ++i)
  {
    EXPECT_LT(thinning[i], thinning[i - 1]) << "band " << i;
    // Each one is about half the last
    EXPECT_NEAR(static_cast<double>(thinning[i]) / thinning[i - 1], 0.5, 0.05) << "band " << i;
  }

  // Above 1 it is the other way round
  const auto thickening = widths(2.0F);
  for (std::size_t i = 1; i < thickening.size(); ++i)
  {
    EXPECT_GT(thickening[i], thickening[i - 1]) << "band " << i;
  }

  // The result says what the bands came out at, bottom and top
  const TerraceResult r = terrace(height, TerraceParams{.levels = 4, .skew = 0.5F}, levels);
  EXPECT_NEAR(static_cast<double>(r.rate), 1.0 / 1.875, 0.01) << "the lowest band";
  EXPECT_NEAR(static_cast<double>(r.top_rate), 0.125 / 1.875, 0.01) << "and the highest";
}

TEST_F(Procgen, TerraceSkewByRateRunsIntoTheCeiling)
{
  Grid<float> height{doc};
  height.assign(Extents{100, 1}, 0.0F);
  for (int x = 0; x < 100; ++x)
  {
    height.at(x, 0) = static_cast<float>(x) / 99.0F;
  }
  Grid<std::uint16_t> levels{doc};
  levels.assign(height.extents, std::uint16_t{0});

  // Bands of 0.2 that each shrink by a quarter sum to 0.8 at most, so the top of a range of 1
  // is past where they converge. It is held at the ceiling rather than running away
  const TerraceResult r =
    terrace(height, TerraceParams{.mode = TerraceMode::kRate, .rate = 0.2F, .skew = 0.25F, .max_level = 6}, levels);
  EXPECT_LE(r.top, 6);
  EXPECT_GT(r.top, 0);
  for (int x = 1; x < 100; ++x)
  {
    EXPECT_GE(levels.at(x, 0), levels.at(x - 1, 0)) << "levels should never fall as the ground rises";
  }
  EXPECT_EQ(levels.at(99, 0), r.top);
  // A skew above 1 by rate simply gives fewer, fatter bands
  const TerraceResult wide =
    terrace(height, TerraceParams{.mode = TerraceMode::kRate, .rate = 0.1F, .skew = 1.5F}, levels);
  const TerraceResult plain = terrace(height, TerraceParams{.mode = TerraceMode::kRate, .rate = 0.1F}, levels);
  EXPECT_LT(wide.top, plain.top);
}

// -------------------------------------------------------------------------------------------------
// Scattered objects
// -------------------------------------------------------------------------------------------------

namespace
{

/// A tileset of 64 tiles holding the groups the scatter tests place
struct Objects
{
  explicit Objects(memory::Arena& arena) : tilesets{memory::make_vector<TileSet>(arena)}
  {
    tilesets.emplace_back(arena);
    TileSet& ts = tilesets.back();
    ts.name = "objects";
    ts.slice = SliceParams{.tile_width = 16, .tile_height = 16};
    ts.image_width = 16 * 8;
    ts.image_height = 16 * 8;
    reslice(ts);
    // A 2x2 house, a 1x1 rock, and a 3x2 tree whose top corners are holes
    const TileIndex house[] = {1, 2, 9, 10};
    const TileIndex rock[] = {3};
    const TileIndex tree[] = {kNoTileIndex, 4, kNoTileIndex, 11, 12, 13};
    set_group(ts, "house", Extents{2, 2}, house);
    set_group(ts, "rock", Extents{1, 1}, rock);
    set_group(ts, "tree", Extents{3, 2}, tree);
  }
  TileSets tilesets;
};

/// Cells of <code>tiles</code> holding a tile at all
std::size_t drawn(const Grid<TileRef>& tiles)
{
  return static_cast<std::size_t>(
    std::count_if(tiles.values.begin(), tiles.values.end(), [](TileRef t) { return !t.empty(); }));
}

constexpr ScatterObject object(GroupId group, float weight = 1.0F)
{
  return ScatterObject{.tileset = 0, .group = group, .weight = weight};
}

}  // namespace

TEST_F(Procgen, ScatterPlacesWholeObjectsAndRepeatsForASeed)
{
  Objects o{doc};
  Field f{doc, Extents{40, 40}};
  const ScatterObject objects[] = {object(0)};
  const ScatterParams params{.count = 12, .spacing = 0, .attempts = 200};
  const ScatterResult result = scatter_groups(o.tilesets, objects, 1, ScatterMask{}, params, 7, scratch, f.tiles);
  EXPECT_EQ(result.placed, 12U);
  EXPECT_EQ(result.unplaced, 0U);
  // Every placement is the whole 2x2 house, so the cells written follow from the count
  EXPECT_EQ(result.cells, 12U * 4U);
  EXPECT_EQ(drawn(f.tiles), 12U * 4U);
  // Nothing but that group's own tiles, and each house complete
  for (int y = 0; y < 40; ++y)
  {
    for (int x = 0; x < 40; ++x)
    {
      if (f.tiles.at(x, y) == TileRef{0, 1})
      {
        EXPECT_EQ(f.tiles.get(x + 1, y), (TileRef{0, 2})) << x << "," << y;
        EXPECT_EQ(f.tiles.get(x, y + 1), (TileRef{0, 9})) << x << "," << y;
        EXPECT_EQ(f.tiles.get(x + 1, y + 1), (TileRef{0, 10})) << x << "," << y;
      }
    }
  }
  // The run is a function of the seed alone
  Field again{doc, Extents{40, 40}};
  const ScatterResult twice = scatter_groups(o.tilesets, objects, 1, ScatterMask{}, params, 7, scratch, again.tiles);
  EXPECT_EQ(twice.placed, result.placed);
  for (std::size_t i = 0; i < f.tiles.values.size(); ++i)
  {
    ASSERT_EQ(f.tiles.values[i], again.tiles.values[i]) << "cell " << i;
  }
  // Another seed moves them
  Field other{doc, Extents{40, 40}};
  scatter_groups(o.tilesets, objects, 1, ScatterMask{}, params, 8, scratch, other.tiles);
  EXPECT_NE(f.tiles.values, other.tiles.values);
}

TEST_F(Procgen, ScatterPlacementsNeverOverlapAndKeepTheirSpacing)
{
  Objects o{doc};
  const ScatterObject objects[] = {object(0)};
  // A 10x10 map takes at most 25 2x2 houses with no spacing, so asking for far more is the test:
  // the ones that fit are placed and the rest run out of room rather than overlapping
  Field f{doc, Extents{10, 10}};
  const ScatterResult packed = scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{},
    ScatterParams{.count = 200, .spacing = 0, .attempts = 400},
    3,
    scratch,
    f.tiles);
  EXPECT_GT(packed.placed, 8U);
  EXPECT_LE(packed.placed, 25U);
  EXPECT_EQ(packed.unplaced, 200U - packed.placed);
  // No cell was written twice, which is what "placed * 4" proves against the cells drawn
  EXPECT_EQ(drawn(f.tiles), packed.placed * 4U);

  // With a spacing of one, no two houses touch: every cell around a house is clear of another one
  Field spaced{doc, Extents{30, 30}};
  const ScatterResult result = scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{},
    ScatterParams{.count = 20, .spacing = 1, .attempts = 400},
    5,
    scratch,
    spaced.tiles);
  ASSERT_GT(result.placed, 4U);
  for (int y = 0; y < 30; ++y)
  {
    for (int x = 0; x < 30; ++x)
    {
      // The top-left tile of a house: the ring around its 2x2 box must hold nothing
      if (spaced.tiles.at(x, y) != TileRef{0, 1})
      {
        continue;
      }
      for (int dy = -1; dy <= 2; ++dy)
      {
        for (int dx = -1; dx <= 2; ++dx)
        {
          const bool inside = (dx >= 0 and dx <= 1 and dy >= 0 and dy <= 1);
          if (!inside)
          {
            EXPECT_TRUE(spaced.tiles.get(x + dx, y + dy).empty()) << x + dx << "," << y + dy;
          }
        }
      }
    }
  }
}

TEST_F(Procgen, ScatterObeysTheBandTheGroundTypeAndTheCollisionMask)
{
  Objects o{doc};
  const ScatterObject objects[] = {object(1)};  // the 1x1 rock, so a cell is a placement
  const ScatterParams params{.count = 400, .spacing = 0, .attempts = 50};

  // Rows 0..10 hold heights 0.0 .. 1.0, so [0.3, 0.5] is rows 3, 4 and 5
  Field band{doc, Extents{10, 11}};
  scatter_groups(
    o.tilesets, objects, 1, ScatterMask{.band = &band.height, .lo = 0.3F, .hi = 0.5F}, params, 1, scratch, band.tiles);
  EXPECT_GT(drawn(band.tiles), 0U);
  for (int y = 0; y < 11; ++y)
  {
    for (int x = 0; x < 10; ++x)
    {
      EXPECT_EQ(band.tiles.at(x, y).empty(), y < 3 or y > 5) << x << "," << y;
    }
  }

  // The semantic layer is in stripes of x % 3
  Field ground{doc, Extents{12, 12}};
  scatter_groups(o.tilesets, objects, 1, ScatterMask{.ids = &ground.ids, .id = 2}, params, 1, scratch, ground.tiles);
  EXPECT_GT(drawn(ground.tiles), 0U);
  for (int y = 0; y < 12; ++y)
  {
    for (int x = 0; x < 12; ++x)
    {
      EXPECT_EQ(ground.tiles.at(x, y).empty(), (x % 3) != 2) << x << "," << y;
    }
  }

  // The collision mask: the left half is solid, and nothing may land in it
  Field blocked{doc, Extents{12, 12}};
  Grid<std::uint8_t> collision{doc};
  collision.assign(blocked.tiles.extents, collision::kNone);
  for (int y = 0; y < 12; ++y)
  {
    for (int x = 0; x < 6; ++x)
    {
      collision.at(x, y) = collision::kSolid;
    }
  }
  scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{.collision = &collision, .blocked = collision::kSolid},
    params,
    1,
    scratch,
    blocked.tiles);
  EXPECT_GT(drawn(blocked.tiles), 0U);
  for (int y = 0; y < 12; ++y)
  {
    for (int x = 0; x < 6; ++x)
    {
      EXPECT_TRUE(blocked.tiles.at(x, y).empty()) << x << "," << y;
    }
  }
}

TEST_F(Procgen, ScatterRefusesAPlacementWhoseEveryCellDoesNotFit)
{
  Objects o{doc};
  // The 3x2 tree, on a map whose right half alone is the right ground: a tree straddling the edge
  // has cells on both, which is the case a per-cell fill cannot express and a scatter must refuse
  Field f{doc, Extents{20, 20}};
  for (int y = 0; y < 20; ++y)
  {
    for (int x = 0; x < 20; ++x)
    {
      f.ids.at(x, y) = static_cast<std::uint16_t>((x >= 10) ? 1 : 0);
    }
  }
  const ScatterObject objects[] = {object(2)};
  scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{.ids = &f.ids, .id = 1},
    ScatterParams{.count = 60, .spacing = 0, .attempts = 200},
    4,
    scratch,
    f.tiles);
  ASSERT_GT(drawn(f.tiles), 0U);
  for (int y = 0; y < 20; ++y)
  {
    for (int x = 0; x < 10; ++x)
    {
      EXPECT_TRUE(f.tiles.at(x, y).empty()) << x << "," << y;
    }
  }
  // Every tree that landed is whole: its trunk is the row under its crown. Its two top corners are
  // gaps, which another tree may interlock into, so they are not expected to be empty -- what is
  // expected is that no cell was written twice, which is the four solid cells per placement
  std::size_t crowns = 0;
  for (int y = 0; y < 20; ++y)
  {
    for (int x = 0; x < 20; ++x)
    {
      if (f.tiles.at(x, y) != TileRef{0, 4})
      {
        continue;
      }
      ++crowns;
      EXPECT_EQ(f.tiles.get(x - 1, y + 1), (TileRef{0, 11})) << x << "," << y;
      EXPECT_EQ(f.tiles.get(x, y + 1), (TileRef{0, 12})) << x << "," << y;
      EXPECT_EQ(f.tiles.get(x + 1, y + 1), (TileRef{0, 13})) << x << "," << y;
    }
  }
  EXPECT_EQ(drawn(f.tiles), crowns * 4U);
}

TEST_F(Procgen, ScatterWeightsDecideTheShares)
{
  Objects o{doc};
  Field f{doc, Extents{80, 80}};
  // The house and the rock, three to one
  const ScatterObject objects[] = {object(0, 3.0F), object(1, 1.0F)};
  const ScatterResult result = scatter_groups(
    o.tilesets,
    objects,
    2,
    ScatterMask{},
    ScatterParams{.count = 400, .spacing = 0, .attempts = 100},
    11,
    scratch,
    f.tiles);
  ASSERT_GT(result.placed, 300U);
  std::size_t houses = 0;
  std::size_t rocks = 0;
  for (const TileRef tile : f.tiles.values)
  {
    houses += (tile == TileRef{0, 1}) ? 1U : 0U;
    rocks += (tile == TileRef{0, 3}) ? 1U : 0U;
  }
  ASSERT_GT(houses + rocks, 0U);
  const float share = static_cast<float>(houses) / static_cast<float>(houses + rocks);
  EXPECT_NEAR(share, 0.75F, 0.08F);

  // A weight of zero keeps an object out altogether
  Field only{doc, Extents{40, 40}};
  const ScatterObject without[] = {object(0, 0.0F), object(1, 1.0F)};
  scatter_groups(
    o.tilesets,
    without,
    2,
    ScatterMask{},
    ScatterParams{.count = 40, .spacing = 0, .attempts = 100},
    2,
    scratch,
    only.tiles);
  for (const TileRef tile : only.tiles.values)
  {
    EXPECT_NE(tile, (TileRef{0, 1}));
  }
  EXPECT_GT(drawn(only.tiles), 0U);
}

TEST_F(Procgen, ScatterSparesWhatIsDrawnAndHonoursTheRegion)
{
  Objects o{doc};
  const ScatterObject objects[] = {object(1)};

  // only_empty: the cells that already hold a tile are left as they are
  Field f{doc, Extents{20, 20}};
  for (int x = 0; x < 20; ++x)
  {
    f.tiles.at(x, 0) = fill_tile(0);
  }
  scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{.only_empty = true},
    ScatterParams{.count = 400, .spacing = 0, .attempts = 100},
    6,
    scratch,
    f.tiles);
  for (int x = 0; x < 20; ++x)
  {
    EXPECT_EQ(f.tiles.at(x, 0), fill_tile(0)) << x;
  }

  // A region narrows it to a rectangle, and nothing lands outside
  Field narrow{doc, Extents{20, 20}};
  const ScatterResult result = scatter_groups(
    o.tilesets,
    objects,
    1,
    ScatterMask{.region = Region{.x = 4, .y = 5, .width = 3, .height = 2}},
    ScatterParams{.count = 50, .spacing = 0, .attempts = 200},
    9,
    scratch,
    narrow.tiles);
  EXPECT_EQ(result.placed, 6U);
  EXPECT_EQ(drawn(narrow.tiles), 6U);
  for (int y = 0; y < 20; ++y)
  {
    for (int x = 0; x < 20; ++x)
    {
      const bool inside = (x >= 4 and x < 7 and y >= 5 and y < 7);
      EXPECT_EQ(narrow.tiles.at(x, y).empty(), !inside) << x << "," << y;
    }
  }
}

TEST_F(Procgen, ScatterSkipsObjectsItCannotPlace)
{
  Objects o{doc};
  Field f{doc, Extents{20, 20}};
  // A group that is not there, and one too big for the region: both skipped rather than refused
  const ScatterObject objects[] = {
    ScatterObject{.tileset = 0, .group = 99, .weight = 1.0F},
    ScatterObject{.tileset = 7, .group = 0, .weight = 1.0F},
    object(2)};
  const ScatterResult result = scatter_groups(
    o.tilesets,
    objects,
    3,
    ScatterMask{.region = Region{.x = 0, .y = 0, .width = 2, .height = 2}},
    ScatterParams{.count = 5, .spacing = 0, .attempts = 50},
    1,
    scratch,
    f.tiles);
  // The 3x2 tree does not fit a 2x2 region either, so nothing is left to place
  EXPECT_EQ(result.placed, 0U);
  EXPECT_EQ(drawn(f.tiles), 0U);
  EXPECT_EQ(scratch.utilized, 0UL);

  // No objects at all, and a count of zero, both write nothing
  EXPECT_EQ(scatter_groups(o.tilesets, nullptr, 0, ScatterMask{}, ScatterParams{}, 1, scratch, f.tiles).placed, 0U);
  const ScatterObject one[] = {object(1)};
  EXPECT_EQ(
    scatter_groups(o.tilesets, one, 1, ScatterMask{}, ScatterParams{.count = 0}, 1, scratch, f.tiles).placed, 0U);
}
