// C++ Standard Library
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/level/auto_tile_rules.hpp"
#include "lvl/level/level.hpp"
#include "lvl/procgen/autotile.hpp"
#include "lvl/procgen/fill.hpp"
#include "lvl/procgen/noise.hpp"
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

/// A terrain covering every level, whose tiles are the step type itself
StepRules all_levels_rules(memory::Arena& arena)
{
  StepRules rules{arena};
  StepTerrain& t = rules.terrains[add_step_terrain(rules, "ground")];
  t.level_lo = 0;
  t.level_hi = 9;
  for (std::size_t i = 0; i < kStepTypeCount; ++i)
  {
    t.tiles[i] = TileRef{0, static_cast<TileIndex>(i)};
  }
  return rules;
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

TEST_F(Procgen, StepsDrawTheRimOfAPlateau)
{
  const StepRules rules = all_levels_rules(doc);
  // A 3x3 plateau of level 1 in a field of level 0
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01110", "00000"});
  Grid<TileRef> tiles{doc};
  tiles.assign(levels.extents, kEmptyTile);
  const StepFillResult result = tile_steps(rules, levels, StepFillParams{}, tiles, nullptr);
  EXPECT_EQ(result.missing, 0U);
  EXPECT_EQ(result.untermed, 0U);
  EXPECT_EQ(result.tiled, 25U);
  const auto type_at = [&tiles](int x, int y) { return static_cast<StepType>(tiles.at(x, y).tile); };
  // The rim: edges along the sides, convex corners at the four corners
  EXPECT_EQ(type_at(2, 1), StepType::kOutwardTop);
  EXPECT_EQ(type_at(2, 3), StepType::kOutwardBottom);
  EXPECT_EQ(type_at(1, 2), StepType::kOutwardLeft);
  EXPECT_EQ(type_at(3, 2), StepType::kOutwardRight);
  EXPECT_EQ(type_at(1, 1), StepType::kOutwardTopRight);
  EXPECT_EQ(type_at(3, 1), StepType::kOutwardTopLeft);
  EXPECT_EQ(type_at(1, 3), StepType::kOutwardBottomLeft);
  EXPECT_EQ(type_at(3, 3), StepType::kOutwardBottomRight);
  // Everything at the foot of the step stays flat: only the higher side is drawn
  for (int x = 0; x < 5; ++x)
  {
    EXPECT_EQ(type_at(x, 0), StepType::kFlat) << "at (" << x << ", 0)";
    EXPECT_EQ(type_at(x, 4), StepType::kFlat) << "at (" << x << ", 4)";
  }
}

TEST_F(Procgen, StepsDrawInwardCornersWhereTheGroundWraps)
{
  const StepRules rules = all_levels_rules(doc);
  // A plateau with one corner cell missing, so the ground wraps around a hole
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01100", "00000"});
  Grid<TileRef> tiles{doc};
  tiles.assign(levels.extents, kEmptyTile);
  tile_steps(rules, levels, StepFillParams{}, tiles, nullptr);
  // (2, 2) has only its SE neighbor below -- the hole at (3, 3) -- which is the concave corner
  EXPECT_EQ(static_cast<StepType>(tiles.at(2, 2).tile), StepType::kInwardBottomRight);
  // Its neighbors along the same rim are ordinary edges
  EXPECT_EQ(static_cast<StepType>(tiles.at(2, 1).tile), StepType::kOutwardTop);
}

TEST_F(Procgen, StepsTakeTheMapEdgeAsLevelGround)
{
  const StepRules rules = all_levels_rules(doc);
  // A plateau running off the left edge: no cliff is drawn along the edge itself
  Grid<std::uint16_t> levels = levels_of(doc, {"11000", "11000", "11000"});
  Grid<TileRef> tiles{doc};
  tiles.assign(levels.extents, kEmptyTile);
  tile_steps(rules, levels, StepFillParams{}, tiles, nullptr);
  EXPECT_EQ(static_cast<StepType>(tiles.at(0, 1).tile), StepType::kFlat);
  EXPECT_EQ(static_cast<StepType>(tiles.at(1, 1).tile), StepType::kOutwardRight);
}

TEST_F(Procgen, StepTerrainsAreChosenByLevel)
{
  StepRules rules{doc};
  StepTerrain& water = rules.terrains[add_step_terrain(rules, "water")];
  water.level_lo = 0;
  water.level_hi = 0;
  water.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{0, 100};
  StepTerrain& grass = rules.terrains[add_step_terrain(rules, "grass")];
  grass.level_lo = 1;
  grass.level_hi = 2;
  grass.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{0, 200};
  grass.tiles[static_cast<std::size_t>(StepType::kOutwardTop)] = TileRef{0, 201};

  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "33330"});
  Grid<TileRef> tiles{doc};
  tiles.assign(levels.extents, kEmptyTile);
  const StepFillResult result = tile_steps(rules, levels, StepFillParams{}, tiles, nullptr);
  EXPECT_EQ(tiles.at(0, 0), (TileRef{0, 100}));
  EXPECT_EQ(tiles.at(2, 1), (TileRef{0, 201}));
  EXPECT_EQ(tiles.at(2, 2), (TileRef{0, 200}));
  // Level 3 has no terrain, and those cells keep what they had
  EXPECT_EQ(result.untermed, 4U);
  EXPECT_EQ(tiles.at(0, 3), kEmptyTile);
  // A pattern the terrain has no tile for is reported rather than guessed at
  EXPECT_GT(result.missing, 0U);
  EXPECT_EQ(find_step_terrain(rules, "grass"), 1);
  EXPECT_EQ(step_terrain_at(rules, 2), 1);
  EXPECT_EQ(step_terrain_at(rules, 7), kNoStepTerrain);
  EXPECT_EQ(step_terrain_of(rules, (TileRef{0, 201})), 1);
}

TEST_F(Procgen, StepCollisionBlocksOneSideOfTheCliff)
{
  const StepRules rules = all_levels_rules(doc);
  Grid<std::uint16_t> levels = levels_of(doc, {"00000", "01110", "01110", "01110", "00000"});
  Grid<TileRef> tiles{doc};
  tiles.assign(levels.extents, kEmptyTile);
  Grid<std::uint8_t> mask{doc};
  mask.assign(levels.extents, std::uint8_t{0});
  // Below: the foot of the cliff is blocked and the plateau stays walkable
  tile_steps(rules, levels, StepFillParams{.collision = StepCollision::kBelow}, tiles, &mask);
  EXPECT_EQ(mask.at(0, 0), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 0), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 1), 0);
  EXPECT_EQ(mask.at(2, 2), 0);

  // Above: the rim is blocked instead, and the old marks are cleared rather than left behind
  tile_steps(rules, levels, StepFillParams{.collision = StepCollision::kAbove}, tiles, &mask);
  EXPECT_EQ(mask.at(0, 0), 0);
  EXPECT_EQ(mask.at(2, 1), level::collision::kSolid);
  EXPECT_EQ(mask.at(2, 2), 0);

  // Other bits of the mask are left alone
  mask.at(0, 0) = level::collision::kTrigger;
  tile_steps(rules, levels, StepFillParams{.collision = StepCollision::kBelow}, tiles, &mask);
  EXPECT_EQ(mask.at(0, 0), level::collision::kTrigger | level::collision::kSolid);
  // And nothing is touched at all when the collision layer is not asked for
  tile_steps(rules, levels, StepFillParams{}, tiles, &mask);
  EXPECT_EQ(mask.at(2, 1), 0);
}

TEST_F(Procgen, TerraceCutsTheRangeIntoLevels)
{
  Grid<float> height{doc};
  height.assign(Extents{11, 1}, 0.0F);
  for (int x = 0; x < 11; ++x)
  {
    height.at(x, 0) = static_cast<float>(x) / 10.0F;
  }
  Grid<std::uint16_t> levels{doc};
  levels.assign(height.extents, std::uint16_t{0});

  // Four bands over a range of 0..1, so each is 0.25 tall
  const TerraceResult result = terrace(height, TerraceParams{.levels = 4}, levels);
  EXPECT_EQ(result.top, 3);
  EXPECT_FLOAT_EQ(result.rate, 0.25F);
  EXPECT_EQ(result.changed, 8U);
  const std::array<std::uint16_t, 11> expected = {0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 3};
  for (int x = 0; x < 11; ++x)
  {
    EXPECT_EQ(levels.at(x, 0), expected[static_cast<std::size_t>(x)]) << "at " << x;
  }
  // The very top of the range belongs to the last band, not to one of its own
  EXPECT_EQ(levels.at(10, 0), 3);
  // Running it again changes nothing
  EXPECT_EQ(terrace(height, TerraceParams{.levels = 4}, levels).changed, 0U);

  // By rate instead: bands 0.2 tall over the same range
  const TerraceResult by_rate = terrace(height, TerraceParams{.mode = TerraceMode::kRate, .rate = 0.2F}, levels);
  EXPECT_EQ(by_rate.top, 4);
  EXPECT_EQ(levels.at(0, 0), 0);
  EXPECT_EQ(levels.at(5, 0), 2);
  EXPECT_EQ(levels.at(10, 0), 4);
}

TEST_F(Procgen, TerraceHandlesFlatMapsAndGivenRanges)
{
  Grid<float> height{doc};
  height.assign(Extents{4, 1}, 0.5F);
  Grid<std::uint16_t> levels{doc};
  levels.assign(height.extents, std::uint16_t{7});
  // A flat map is one level, whatever was asked for
  const TerraceResult flat = terrace(height, TerraceParams{.levels = 6}, levels);
  EXPECT_EQ(flat.top, 0);
  EXPECT_EQ(flat.changed, 4U);
  for (const std::uint16_t v : levels.values)
  {
    EXPECT_EQ(v, 0);
  }

  // A range given explicitly is used instead of the data's own, and values outside it are clamped
  for (int x = 0; x < 4; ++x)
  {
    height.at(x, 0) = static_cast<float>(x);
  }
  terrace(height, TerraceParams{.levels = 3, .lo = 0.0F, .hi = 3.0F}, levels);
  EXPECT_EQ(levels.at(0, 0), 0);
  EXPECT_EQ(levels.at(3, 0), 2);
  terrace(height, TerraceParams{.levels = 4, .lo = 0.0F, .hi = 2.0F}, levels);
  EXPECT_EQ(levels.at(3, 0), 3) << "a cell above the range is clamped to the top band";

  // The ceiling holds levels down whatever the rate says
  terrace(height, TerraceParams{.mode = TerraceMode::kRate, .rate = 0.1F, .max_level = 5}, levels);
  for (const std::uint16_t v : levels.values)
  {
    EXPECT_LE(v, 5);
  }
}
