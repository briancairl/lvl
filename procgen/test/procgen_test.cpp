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
