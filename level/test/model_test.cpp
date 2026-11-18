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
