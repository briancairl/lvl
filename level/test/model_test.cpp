// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/level/level.hpp"

using namespace lvl;
using namespace lvl::level;

class LevelModel : public ::testing::Test
{
protected:
  void SetUp() override { memory::create(arena, 1 << 24, "document"); }
  void TearDown() override { memory::destroy(arena); }
  memory::Arena arena;
};

TEST_F(LevelModel, DefaultLevelHasCatalogsAndLayers)
{
  Level level{arena};
  create_default(level, Settings{.name = "test", .extents = {8, 4}});
  EXPECT_EQ(level.semantics.size(), 1UL);
  EXPECT_EQ(level.materials.size(), 1UL);
  EXPECT_EQ(level.seasons.size(), 1UL);
  ASSERT_EQ(level.layers.size(), 4UL);
  EXPECT_EQ(extents(level.layers[1]), (Extents{8, 4}));
  EXPECT_LT(find_layer(level, LayerKind::kCollision), level.layers.size());
}

TEST_F(LevelModel, ResetRewindsArena)
{
  Level level{arena};
  const auto base = arena.utilized;
  create_default(level, Settings{.extents = {256, 256}});
  EXPECT_GT(arena.utilized, base + 256UL * 256UL);
  reset(level);
  EXPECT_LE(arena.utilized, base + 1024UL);
}

TEST_F(LevelModel, ResizeKeepsOverlap)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  auto& ground = plane<TileGrid>(level.layers[1]);
  ground.at(1, 2) = TileRef{0, 7};
  resize(level, Extents{2, 8});
  EXPECT_EQ(ground.extents, (Extents{2, 8}));
  EXPECT_EQ(ground.at(1, 2), (TileRef{0, 7}));
  EXPECT_TRUE(ground.at(1, 7).empty());
}

TEST_F(LevelModel, MoveAndRemoveLayers)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  move_layer(level, 0, 3);
  EXPECT_EQ(level.layers[3].kind, LayerKind::kHeight);
  move_layer(level, 3, 0);
  EXPECT_EQ(level.layers[0].kind, LayerKind::kHeight);
  remove_layer(level, 0);
  EXPECT_EQ(level.layers.size(), 3UL);
}

TEST_F(LevelModel, SlicingAndSeasonalVariants)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  const TileSetId id = add_tileset(level, "forest");
  auto& ts = level.tilesets[id];
  ts.slice =
    SliceParams{.tile_width = 16, .tile_height = 16, .margin_x = 1, .margin_y = 1, .spacing_x = 2, .spacing_y = 2};
  ts.image_width = 1 + 4 * 16 + 3 * 2;
  ts.image_height = 1 + 2 * 16 + 1 * 2;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 8UL);
  const TileRect r = tile_rect(ts.slice, ts.image_width, ts.image_height, 5);
  EXPECT_EQ(r.x, 1 + 18);
  EXPECT_EQ(r.y, 1 + 18);

  const MaterialId snow = intern_material(level, "snow");
  ts.tiles[2].material = intern_material(level, "grass");
  set_seasonal_variant(ts, SeasonalVariant{.tile = 2, .season = "winter", .alternate = 6, .material = snow});
  EXPECT_EQ(seasonal_tile(ts, 2, "winter"), 6);
  EXPECT_EQ(seasonal_tile(ts, 2, "summer"), 2);
  EXPECT_EQ(seasonal_material(ts, 2, "winter"), snow);
  EXPECT_EQ(seasonal_material(ts, 2, "summer"), ts.tiles[2].material);
  auto lookup = memory::make_vector<TileIndex>(arena);
  resolve_season(ts, "winter", lookup);
  EXPECT_EQ(lookup[2], 6);
  EXPECT_EQ(lookup[3], 3);
}

TEST_F(LevelModel, BatchTileEditWritesOnlyTheFieldsItIsGiven)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  const TileSetId id = add_tileset(level, "forest");
  auto& ts = level.tilesets[id];
  ts.image_width = 4 * 16;
  ts.image_height = 16;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 4UL);
  const SemanticId grass = intern_semantic(level, "grass");
  ts.tiles[0].material = intern_material(level, "dirt");
  ts.tiles[0].hidden = true;
  ts.tiles[0].scatter_weight = 3.0F;
  set_property(ts, 0, "sound", "step");

  // Two of the four tiles, plus an index past the end, which is skipped
  const TileIndex tiles[] = {0, 2, 9};
  const TileEdit edit{
    .set_semantic = true,
    .semantic = grass,
    .set_collision = true,
    .collision = collision::kSolid,
    .set_property = true,
    .property_key = "sound",
    .property_value = "grass"};
  EXPECT_EQ(apply_edit(ts, edit, tiles, 3), 2UL);
  EXPECT_EQ(ts.tiles[0].semantic, grass);
  EXPECT_EQ(ts.tiles[2].semantic, grass);
  EXPECT_EQ(ts.tiles[1].semantic, kNoSemantic);
  EXPECT_EQ(ts.tiles[0].collision, collision::kSolid);
  // Fields the edit does not carry are left alone
  EXPECT_EQ(ts.tiles[0].material, intern_material(level, "dirt"));
  EXPECT_TRUE(ts.tiles[0].hidden);
  EXPECT_FALSE(ts.tiles[2].hidden);
  EXPECT_FLOAT_EQ(ts.tiles[0].scatter_weight, 3.0F);
  EXPECT_FLOAT_EQ(ts.tiles[2].scatter_weight, 1.0F);
  ASSERT_NE(find_property(ts, 0, "sound"), nullptr);
  EXPECT_EQ(*find_property(ts, 0, "sound"), std::string_view{"grass"});
  ASSERT_NE(find_property(ts, 2, "sound"), nullptr);
  EXPECT_EQ(ts.properties.size(), 2UL);

  // Removing by key, and an edit that writes nothing
  // The scatter weight is written only when the edit carries it
  const TileEdit reweigh{.set_scatter_weight = true, .scatter_weight = 0.25F};
  EXPECT_EQ(apply_edit(ts, reweigh, tiles, 3), 2UL);
  EXPECT_FLOAT_EQ(ts.tiles[0].scatter_weight, 0.25F);
  EXPECT_FLOAT_EQ(ts.tiles[2].scatter_weight, 0.25F);
  EXPECT_FLOAT_EQ(ts.tiles[1].scatter_weight, 1.0F);

  const TileEdit remove{.clear_property = true, .property_key = "sound"};
  EXPECT_EQ(apply_edit(ts, remove, tiles, 3), 2UL);
  EXPECT_TRUE(ts.properties.empty());
  EXPECT_EQ(apply_edit(ts, TileEdit{}, tiles, 3), 2UL);
  EXPECT_EQ(ts.tiles[0].semantic, grass);
}

TEST_F(LevelModel, DeriveCollisionAndMaterialsFromTiles)
{
  Level level{arena};
  create_default(level, Settings{.extents = {3, 1}});
  const TileSetId id = add_tileset(level, "t");
  auto& ts = level.tilesets[id];
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[1].collision = collision::kSolid;
  ts.tiles[1].material = intern_material(level, "stone");
  auto& ground = plane<TileGrid>(level.layers[1]);
  ground.at(0, 0) = TileRef{id, 0};
  ground.at(1, 0) = TileRef{id, 1};
  const auto col = find_layer(level, LayerKind::kCollision);
  derive_collision(level, col, "default");
  const auto& mask = plane<MaskGrid>(level.layers[col]);
  EXPECT_EQ(mask.at(0, 0), collision::kNone);
  EXPECT_EQ(mask.at(1, 0), collision::kSolid);

  const auto mat = add_layer(level, LayerKind::kMaterial, "materials");
  derive_materials(level, mat, "default");
  EXPECT_EQ(plane<IdGrid>(level.layers[mat]).at(1, 0), ts.tiles[1].material);
}

TEST_F(LevelModel, RemovingTilesetClearsCells)
{
  Level level{arena};
  create_default(level, Settings{.extents = {2, 1}});
  add_tileset(level, "a");
  add_tileset(level, "b");
  auto& ground = plane<TileGrid>(level.layers[1]);
  ground.at(0, 0) = TileRef{0, 3};
  ground.at(1, 0) = TileRef{1, 4};
  remove_tileset(level, 0);
  EXPECT_TRUE(ground.at(0, 0).empty());
  EXPECT_EQ(ground.at(1, 0), (TileRef{0, 4}));
}

TEST_F(LevelModel, PortalsAndPathGraphs)
{
  Level level{arena};
  create_default(level, Settings{.extents = {32, 32}});
  const Entity door = create_portal(level.registry, "door", {2, 3}, {1, 2}, Portal{.target_level = "cave.json"});
  EXPECT_EQ(find_portal(level.registry, "door"), door);
  EXPECT_EQ(pick(level.registry, Position{2.5F, 4.0F}, 0.5F), door);

  const GraphId road = add_graph(level, "road", GraphKind::kRoad);
  const Entity a = create_path_node(level.registry, road, {0, 0});
  const Entity b = create_path_node(level.registry, road, {10, 0});
  const Entity c = create_path_node(level.registry, road, {10, 10});
  const Entity d = create_path_node(level.registry, road, {0, 10});
  create_path_edge(level.registry, a, b);
  create_path_edge(level.registry, b, c);
  create_path_edge(level.registry, a, d, 5.0F);
  create_path_edge(level.registry, d, c, 5.0F);

  auto path = memory::make_vector<Entity>(arena);
  shortest_path(level.registry, a, c, path);
  ASSERT_EQ(path.size(), 3UL);
  EXPECT_EQ(path[1], b);

  destroy_path_node(level.registry, b);
  shortest_path(level.registry, a, c, path);
  ASSERT_EQ(path.size(), 3UL);
  EXPECT_EQ(path[1], d);

  remove_graph(level, road);
  EXPECT_TRUE(level.registry.view<PathNode>().empty());
  EXPECT_TRUE(level.registry.view<PathEdge>().empty());
}

TEST_F(LevelModel, WfcRulesLearnFromExample)
{
  Level level{arena};
  create_default(level, Settings{.extents = {3, 2}});
  auto& ground = plane<TileGrid>(level.layers[1]);
  // a b a
  // c c c
  ground.at(0, 0) = {0, 1};
  ground.at(1, 0) = {0, 2};
  ground.at(2, 0) = {0, 1};
  ground.at(0, 1) = {0, 3};
  ground.at(1, 1) = {0, 3};
  ground.at(2, 1) = {0, 3};
  learn(level.wfc, ground);
  ASSERT_EQ(level.wfc.tiles.size(), 3UL);
  const WfcTileId a = find_tile(level.wfc, {0, 1});
  const WfcTileId b = find_tile(level.wfc, {0, 2});
  const WfcTileId c = find_tile(level.wfc, {0, 3});
  EXPECT_FLOAT_EQ(level.wfc.tiles[c].weight, 3.0F);
  EXPECT_TRUE(allowed(level.wfc, a, b, Direction::kEast));
  EXPECT_TRUE(allowed(level.wfc, b, a, Direction::kWest));
  EXPECT_TRUE(allowed(level.wfc, b, a, Direction::kEast));
  EXPECT_TRUE(allowed(level.wfc, c, a, Direction::kNorth));
  EXPECT_FALSE(allowed(level.wfc, a, a, Direction::kEast));
  EXPECT_FALSE(allowed(level.wfc, a, c, Direction::kNorth));
  set_allowed(level.wfc, a, a, Direction::kEast, true);
  EXPECT_TRUE(allowed(level.wfc, a, a, Direction::kWest));
  remove_tile(level.wfc, a);
  EXPECT_EQ(level.wfc.tiles.size(), 2UL);
}

TEST_F(LevelModel, BatchWfcConstraintsWriteOnlyTheFieldsTheyAreGiven)
{
  WfcRules rules{arena};
  const WfcTileId a = add_tile(rules, TileRef{0, 1});
  const WfcTileId b = add_tile(rules, TileRef{0, 2});
  add_tile(rules, TileRef{0, 3});
  rules.tiles[a].semantic = 4;
  rules.tiles[b].weight = 7.0F;

  const WfcTileId tiles[] = {a, b, 9};
  const WfcTileEdit edit{
    .set_height = true, .height_min = 0.25F, .height_max = 0.75F, .set_water = true, .water = WaterRule::kDry};
  EXPECT_EQ(apply_edit(rules, edit, tiles, 3), 2UL);
  EXPECT_FLOAT_EQ(rules.tiles[a].height_min, 0.25F);
  EXPECT_FLOAT_EQ(rules.tiles[b].height_max, 0.75F);
  EXPECT_EQ(rules.tiles[a].water, WaterRule::kDry);
  // Untouched fields and tiles keep what they had
  EXPECT_FLOAT_EQ(rules.tiles[b].weight, 7.0F);
  EXPECT_EQ(rules.tiles[a].semantic, 4);
  EXPECT_FLOAT_EQ(rules.tiles[2].height_max, 1.0F);
  EXPECT_EQ(rules.tiles[2].water, WaterRule::kAny);
}

TEST_F(LevelModel, WfcRulesFromAPaletteLayout)
{
  Level level{arena};
  create_default(level, Settings{.extents = {2, 2}});
  WfcRules& rules = level.wfc;
  // The tile an unrelated rule uses, and two tiles laid out side by side:  a b
  const WfcTileId other = add_tile(rules, TileRef{0, 9});
  memory::Arena scratch;
  memory::create(scratch, 1 << 16, "scratch");
  Grid<TileRef> layout{arena};
  layout.assign(Extents{2, 1}, kEmptyTile);
  layout.at(0, 0) = TileRef{0, 1};
  layout.at(1, 0) = TileRef{0, 2};
  EXPECT_EQ(constrain_from_layout(rules, layout, LayoutMode::kAllow, scratch), 1UL);
  const WfcTileId a = find_tile(rules, TileRef{0, 1});
  const WfcTileId b = find_tile(rules, TileRef{0, 2});
  EXPECT_TRUE(allowed(rules, a, b, Direction::kEast));
  EXPECT_TRUE(allowed(rules, b, a, Direction::kWest));
  EXPECT_FALSE(allowed(rules, a, b, Direction::kSouth));
  // Corners are ignored until the rules use them
  layout.assign(Extents{2, 2}, kEmptyTile);
  layout.at(0, 0) = TileRef{0, 1};
  layout.at(1, 1) = TileRef{0, 2};
  EXPECT_EQ(constrain_from_layout(rules, layout, LayoutMode::kAllow, scratch), 0UL);
  rules.diagonals = true;
  EXPECT_EQ(constrain_from_layout(rules, layout, LayoutMode::kAllow, scratch), 1UL);
  EXPECT_TRUE(allowed(rules, a, b, Direction::kSouthEast));
  rules.diagonals = false;

  // Anything else east of a was allowed before; exclusive mode leaves only what the layout shows
  set_allowed(rules, a, other, Direction::kEast, true);
  set_allowed(rules, a, other, Direction::kNorth, true);
  layout.assign(Extents{2, 1}, kEmptyTile);
  layout.at(0, 0) = TileRef{0, 1};
  layout.at(1, 0) = TileRef{0, 2};
  EXPECT_EQ(constrain_from_layout(rules, layout, LayoutMode::kExclusive, scratch), 1UL);
  EXPECT_TRUE(allowed(rules, a, b, Direction::kEast));
  EXPECT_FALSE(allowed(rules, a, other, Direction::kEast));
  // The layout says nothing about what is north of a, or about tiles it does not contain
  EXPECT_TRUE(allowed(rules, a, other, Direction::kNorth));
  EXPECT_TRUE(allowed(rules, a, b, Direction::kSouthEast));
  EXPECT_EQ(scratch.utilized, 0UL);
  memory::destroy(scratch);
}

TEST_F(LevelModel, WfcDiagonalRules)
{
  for (const Direction d : kNeighbors)
  {
    EXPECT_EQ(opposite(opposite(d)), d);
    EXPECT_NE(opposite(d), d);
    EXPECT_EQ(diagonal(d), diagonal(opposite(d)));
    const Cell n = step(step(Cell{5, 5}, d), opposite(d));
    EXPECT_EQ(n.x, 5);
    EXPECT_EQ(n.y, 5);
  }
  EXPECT_EQ(opposite(Direction::kNorthEast), Direction::kSouthWest);
  EXPECT_EQ(opposite(Direction::kNorthWest), Direction::kSouthEast);

  Level level{arena};
  create_default(level, Settings{.extents = {3, 2}});
  auto& ground = plane<TileGrid>(level.layers[1]);
  // a b a
  // c c c
  ground.at(0, 0) = {0, 1};
  ground.at(1, 0) = {0, 2};
  ground.at(2, 0) = {0, 1};
  ground.at(0, 1) = {0, 3};
  ground.at(1, 1) = {0, 3};
  ground.at(2, 1) = {0, 3};
  learn(level.wfc, ground);
  const WfcTileId a = find_tile(level.wfc, {0, 1});
  const WfcTileId b = find_tile(level.wfc, {0, 2});
  const WfcTileId c = find_tile(level.wfc, {0, 3});
  EXPECT_TRUE(allowed(level.wfc, a, c, Direction::kSouthEast));
  EXPECT_TRUE(allowed(level.wfc, a, c, Direction::kSouthWest));
  EXPECT_TRUE(allowed(level.wfc, c, a, Direction::kNorthWest));
  EXPECT_TRUE(allowed(level.wfc, c, b, Direction::kNorthEast));
  EXPECT_FALSE(allowed(level.wfc, a, b, Direction::kSouthEast));
  EXPECT_FALSE(allowed(level.wfc, c, c, Direction::kNorthEast));
  EXPECT_EQ(diagonal_count(level.wfc), 4UL);  // a-c SE, a-c SW, b-c SE, b-c SW

  // Corners derived from sides: a east of a, b south of a => b south-east of a
  WfcRules rules{arena};
  const WfcTileId x = add_tile(rules, TileRef{0, 1});
  const WfcTileId y = add_tile(rules, TileRef{0, 2});
  set_allowed(rules, x, x, Direction::kEast, true);
  set_allowed(rules, x, y, Direction::kSouth, true);
  memory::Arena scratch;
  memory::create(scratch, 1 << 16, "scratch");
  derive_diagonals(rules, scratch);
  EXPECT_TRUE(allowed(rules, x, y, Direction::kSouthEast));
  EXPECT_TRUE(allowed(rules, x, y, Direction::kSouthWest));
  EXPECT_TRUE(allowed(rules, y, x, Direction::kNorthWest));
  EXPECT_FALSE(allowed(rules, x, x, Direction::kSouthEast));
  EXPECT_FALSE(allowed(rules, y, y, Direction::kSouthEast));
  EXPECT_EQ(diagonal_count(rules), 2UL);
  EXPECT_EQ(scratch.utilized, 0UL);
  memory::destroy(scratch);
}

TEST_F(LevelModel, RemovingCatalogEntriesRemapsEverythingThatUsesThem)
{
  Level level{arena};
  create_default(level, Settings{.extents = {2, 1}});
  const SemanticId road = intern_semantic(level, "road");
  const SemanticId river = intern_semantic(level, "river");
  const MaterialId dirt = intern_material(level, "dirt");
  const MaterialId snow = intern_material(level, "snow");
  const auto sem = add_layer(level, LayerKind::kSemantic, "semantics");
  const auto mat = add_layer(level, LayerKind::kMaterial, "materials");
  plane<IdGrid>(level.layers[sem]).at(0, 0) = road;
  plane<IdGrid>(level.layers[sem]).at(1, 0) = river;
  plane<IdGrid>(level.layers[mat]).at(0, 0) = dirt;
  plane<IdGrid>(level.layers[mat]).at(1, 0) = snow;
  const TileSetId id = add_tileset(level, "t");
  auto& ts = level.tilesets[id];
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[0] = TileInfo{.semantic = river, .material = snow};
  ts.tiles[1] = TileInfo{.semantic = road, .material = dirt};
  set_seasonal_variant(ts, SeasonalVariant{.tile = 0, .season = "winter", .alternate = 1, .material = snow});
  const WfcTileId w = add_tile(level.wfc, TileRef{id, 0});
  level.wfc.tiles[w].semantic = river;

  EXPECT_EQ(semantic_uses(level, road), 2UL);
  EXPECT_EQ(semantic_uses(level, river), 3UL);
  EXPECT_EQ(material_uses(level, snow), 3UL);

  remove_semantic(level, road);
  ASSERT_EQ(level.semantics.size(), 2UL);
  EXPECT_EQ(level.semantics[1].name, std::string_view{"river"});
  EXPECT_EQ(plane<IdGrid>(level.layers[sem]).at(0, 0), kNoSemantic);
  EXPECT_EQ(plane<IdGrid>(level.layers[sem]).at(1, 0), 1);
  EXPECT_EQ(ts.tiles[0].semantic, 1);
  EXPECT_EQ(ts.tiles[1].semantic, kNoSemantic);
  EXPECT_EQ(level.wfc.tiles[w].semantic, 1);

  remove_material(level, dirt);
  ASSERT_EQ(level.materials.size(), 2UL);
  EXPECT_EQ(plane<IdGrid>(level.layers[mat]).at(0, 0), kNoMaterial);
  EXPECT_EQ(plane<IdGrid>(level.layers[mat]).at(1, 0), 1);
  EXPECT_EQ(ts.tiles[0].material, 1);
  EXPECT_EQ(ts.seasonal[0].material, 1);
  EXPECT_EQ(material_uses(level, 1), 3UL);
}

TEST_F(LevelModel, RemovingSeasonsKeepsAtLeastOne)
{
  Level level{arena};
  create_default(level, Settings{.extents = {2, 2}});
  level.seasons.push_back(Season{.name = "winter"});
  remove_season(level, 0);
  ASSERT_EQ(level.seasons.size(), 1UL);
  EXPECT_EQ(level.seasons[0].name, std::string_view{"winter"});
  EXPECT_DEATH(remove_season(level, 0), "at least one season");
}

TEST_F(LevelModel, AutoTileRulesFromANineSliceLayout)
{
  AutoTileRules rules{arena};
  const TerrainId id = add_terrain(rules, arena, "path");
  ASSERT_EQ(id, 0U);
  AutoTerrain& path = rules.terrains[id];
  // The nine tiles of a large patch of path: corners, edges, middle
  Grid<TileRef> layout{arena};
  layout.assign(Extents{3, 3}, kEmptyTile);
  for (int y = 0; y < 3; ++y)
  {
    for (int x = 0; x < 3; ++x)
    {
      layout.at(x, y) = TileRef{0, static_cast<TileIndex>(y * 3 + x)};
    }
  }
  EXPECT_EQ(learn_from_layout(path, layout), 9U);
  ASSERT_EQ(path.variants.size(), 9U);
  // The middle tile is surrounded; the edges and outer corners are not
  EXPECT_EQ(resolve(path, kAllNeighbors), (TileRef{0, 4}));
  EXPECT_EQ(
    resolve(path, mask_of(Direction::kEast) | mask_of(Direction::kSouth) | mask_of(Direction::kSouthEast)),
    (TileRef{0, 0}));
  EXPECT_EQ(
    resolve(
      path,
      mask_of(Direction::kEast) | mask_of(Direction::kWest) | mask_of(Direction::kSouth) |
        mask_of(Direction::kSouthEast) | mask_of(Direction::kSouthWest)),
    (TileRef{0, 1}));
  EXPECT_EQ(
    resolve(path, mask_of(Direction::kWest) | mask_of(Direction::kNorth) | mask_of(Direction::kNorthWest)),
    (TileRef{0, 8}));
  // A pattern no tile covers falls back to the nearest one rather than leaving the cell empty
  EXPECT_FALSE(resolve(path, kNoNeighbors).empty());
  EXPECT_EQ(terrain_of(rules, TileRef{0, 4}), id);
  EXPECT_EQ(terrain_of(rules, TileRef{0, 40}), kNoTerrain);
  EXPECT_EQ(terrain_of(rules, kEmptyTile), kNoTerrain);
}

TEST_F(LevelModel, AutoTileInnerCornersNeedTheBlobNeighborhood)
{
  AutoTileRules rules{arena};
  AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, "water")];
  Grid<TileRef> inner{arena};
  inner.assign(Extents{2, 2}, kEmptyTile);
  inner.at(0, 0) = TileRef{0, 10};
  inner.at(1, 0) = TileRef{0, 11};
  inner.at(0, 1) = TileRef{0, 12};
  inner.at(1, 1) = TileRef{0, 13};

  EXPECT_EQ(learn_from_layout(terrain, inner), 4U);
  // Surrounded except at one corner: the one the tile sits at in the block
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kNorthWest))), (TileRef{0, 10}));
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kSouthEast))), (TileRef{0, 13}));

  // Side-only tiling cannot tell them apart, so it refuses the block instead of overwriting one slot
  AutoTerrain& sides = rules.terrains[add_terrain(rules, arena, "road")];
  sides.neighborhood = Neighborhood::kSides;
  EXPECT_EQ(learn_from_layout(sides, inner), 0U);
  EXPECT_TRUE(sides.variants.empty());
}

TEST_F(LevelModel, AutoTileNeighborhoodsDistinguishTheExpectedPatterns)
{
  // Corner bits only count where both of the corner's sides are terrain too
  EXPECT_EQ(reduce(kAllNeighbors, Neighborhood::kSides), kSideNeighbors);
  EXPECT_EQ(reduce(kAllNeighbors, Neighborhood::kBlob), kAllNeighbors);
  EXPECT_EQ(reduce(mask_of(Direction::kNorthEast), Neighborhood::kBlob), kNoNeighbors);
  EXPECT_EQ(
    reduce(
      mask_of(Direction::kNorth) | mask_of(Direction::kEast) | mask_of(Direction::kNorthEast), Neighborhood::kBlob),
    mask_of(Direction::kNorth) | mask_of(Direction::kEast) | mask_of(Direction::kNorthEast));
  EXPECT_EQ(patterns(Neighborhood::kSides, arena).size(), 16U);
  EXPECT_EQ(patterns(Neighborhood::kBlob, arena).size(), 47U);

  // A side-only terrain stores its tiles under the reduced patterns, so a nine-slice fills all 16
  AutoTileRules rules{arena};
  AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, "sides")];
  terrain.neighborhood = Neighborhood::kSides;
  Grid<TileRef> layout{arena};
  layout.assign(Extents{3, 3}, kEmptyTile);
  for (int i = 0; i < 9; ++i)
  {
    layout.values[i] = TileRef{0, static_cast<TileIndex>(i)};
  }
  EXPECT_EQ(learn_from_layout(terrain, layout), 9U);
  // The nine tiles of the block cover nine of the sixteen side patterns; the rest come from the
  // nearest-pattern fallback
  EXPECT_EQ(terrain.variants.size(), 9U);
  for (const NeighborMask mask : patterns(Neighborhood::kSides, arena))
  {
    EXPECT_FALSE(resolve(terrain, mask).empty()) << "pattern " << static_cast<int>(mask);
  }
}

TEST_F(LevelModel, AutoTileVariantsAreSetRemovedAndFollowTilesets)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  add_tileset(level, "first");
  add_tileset(level, "second");
  AutoTerrain& terrain = level.auto_tiles.terrains[add_terrain(level.auto_tiles, arena, "grass")];
  set_variant(terrain, kAllNeighbors, TileRef{1, 3});
  set_variant(terrain, kSideNeighbors, TileRef{0, 5});
  ASSERT_EQ(terrain.variants.size(), 2U);
  ASSERT_NE(find_variant(terrain, kAllNeighbors), nullptr);
  EXPECT_EQ(find_variant(terrain, kAllNeighbors)->tile, (TileRef{1, 3}));
  // Setting the same pattern again replaces the tile; an empty tile clears the slot
  set_variant(terrain, kAllNeighbors, TileRef{1, 4});
  EXPECT_EQ(find_variant(terrain, kAllNeighbors)->tile, (TileRef{1, 4}));
  set_variant(terrain, kSideNeighbors, kEmptyTile);
  EXPECT_EQ(find_variant(terrain, kSideNeighbors), nullptr);

  // Removing a tileset drops the variants that used it and renumbers the rest
  set_variant(terrain, kNoNeighbors, TileRef{0, 2});
  remove_tileset(level, 0);
  ASSERT_EQ(terrain.variants.size(), 1U);
  EXPECT_EQ(terrain.variants[0].tile, (TileRef{0, 4}));

  remove_terrain(level.auto_tiles, 0);
  EXPECT_TRUE(level.auto_tiles.terrains.empty());
}

TEST_F(LevelModel, AutoTileTakesTheWholeThirteenTileSetFromOneSelection)
{
  AutoTileRules rules{arena};
  AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, "path")];
  // The nine-slice, the inner corners beside it, and two further tiles: alternative middles
  Grid<TileRef> layout{arena};
  layout.assign(Extents{5, 3}, kEmptyTile);
  for (int y = 0; y < 3; ++y)
  {
    for (int x = 0; x < 3; ++x)
    {
      layout.at(x, y) = TileRef{0, static_cast<TileIndex>(y * 3 + x)};
    }
  }
  layout.at(3, 0) = TileRef{0, 9};
  layout.at(4, 0) = TileRef{0, 10};
  layout.at(3, 1) = TileRef{0, 11};
  layout.at(4, 1) = TileRef{0, 12};
  layout.at(3, 2) = TileRef{0, 13};
  layout.at(4, 2) = TileRef{0, 14};

  EXPECT_EQ(learn_from_layout(terrain, layout), 15U);
  // The nine-slice's edges and outer corners
  EXPECT_EQ(
    resolve(terrain, mask_of(Direction::kEast) | mask_of(Direction::kSouth) | mask_of(Direction::kSouthEast)),
    (TileRef{0, 0}));
  EXPECT_EQ(
    resolve(terrain, mask_of(Direction::kWest) | mask_of(Direction::kNorth) | mask_of(Direction::kNorthWest)),
    (TileRef{0, 8}));
  // The 2x2 block beside it: surrounded except at the corner the tile sits at
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kNorthWest))), (TileRef{0, 9}));
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kNorthEast))), (TileRef{0, 10}));
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kSouthWest))), (TileRef{0, 11}));
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kSouthEast))), (TileRef{0, 12}));
  // What is left over joins the middle's pool, which a key picks between
  EXPECT_EQ(variant_count(terrain, kAllNeighbors), 3U);
  bool all_three = true;
  for (const TileIndex expected : {TileIndex{4}, TileIndex{13}, TileIndex{14}})
  {
    bool found = false;
    for (std::uint64_t key = 0; key < 3; ++key)
    {
      found = found or resolve(terrain, kAllNeighbors, key) == TileRef{0, expected};
    }
    all_three = all_three and found;
  }
  EXPECT_TRUE(all_three);
  // The same key always gives the same tile
  EXPECT_EQ(resolve(terrain, kAllNeighbors, 7), resolve(terrain, kAllNeighbors, 7));
  // Taking the blocks one at a time comes to the same thing
  AutoTerrain& twice = rules.terrains[add_terrain(rules, arena, "twice")];
  Grid<TileRef> nine{arena};
  nine.assign(Extents{3, 3}, kEmptyTile);
  Grid<TileRef> inner{arena};
  inner.assign(Extents{2, 2}, kEmptyTile);
  for (int i = 0; i < 9; ++i)
  {
    nine.values[i] = TileRef{0, static_cast<TileIndex>(i)};
  }
  for (int i = 0; i < 4; ++i)
  {
    inner.values[i] = TileRef{0, static_cast<TileIndex>(9 + i)};
  }
  EXPECT_EQ(learn_from_layout(twice, nine), 9U);
  EXPECT_EQ(learn_from_layout(twice, inner), 4U);
  EXPECT_EQ(twice.variants.size(), 13U);
  EXPECT_EQ(
    resolve(twice, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kSouthEast))), (TileRef{0, 12}));
}

TEST_F(LevelModel, AutoTileAnswersAMissingPatternWithTheRightEdge)
{
  AutoTileRules rules{arena};
  AutoTerrain& terrain = rules.terrains[add_terrain(rules, arena, "path")];
  Grid<TileRef> layout{arena};
  layout.assign(Extents{3, 3}, kEmptyTile);
  for (int i = 0; i < 9; ++i)
  {
    layout.values[i] = TileRef{0, static_cast<TileIndex>(i)};
  }
  learn_from_layout(terrain, layout);
  // A nine-slice has no tile for "surrounded but for one corner": the sides decide, so the middle
  // answers, never an edge tile that happens to share corners
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kNorthWest))), (TileRef{0, 4}));
  // A north edge missing a far corner is still the north edge
  const NeighborMask north_edge = mask_of(Direction::kEast) | mask_of(Direction::kWest) | mask_of(Direction::kSouth) |
    mask_of(Direction::kSouthEast) | mask_of(Direction::kSouthWest);
  EXPECT_EQ(resolve(terrain, static_cast<NeighborMask>(north_edge & ~mask_of(Direction::kSouthWest))), (TileRef{0, 1}));
  // With the inner corners added, that first pattern has its own tile
  Grid<TileRef> inner{arena};
  inner.assign(Extents{2, 2}, kEmptyTile);
  for (int i = 0; i < 4; ++i)
  {
    inner.values[i] = TileRef{0, static_cast<TileIndex>(9 + i)};
  }
  learn_from_layout(terrain, inner);
  EXPECT_EQ(
    resolve(terrain, static_cast<NeighborMask>(kAllNeighbors & ~mask_of(Direction::kNorthWest))), (TileRef{0, 9}));
}
