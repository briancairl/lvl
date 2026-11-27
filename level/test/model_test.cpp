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
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.slice =
    SliceParams{.tile_width = 16, .tile_height = 16, .margin_x = 1, .margin_y = 1, .spacing_x = 2, .spacing_y = 2};
  ts.image_width = 1 + 4 * 16 + 3 * 2;
  ts.image_height = 1 + 2 * 16 + 1 * 2;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 8UL);
  const TileRect r = tile_rect(ts.slice, ts.image_width, ts.image_height, 5);
  EXPECT_EQ(r.x, 1 + 18);
  EXPECT_EQ(r.y, 1 + 18);

  // Ids into the project's catalogs; this test only needs them to be distinct
  constexpr MaterialId snow = 1;
  constexpr MaterialId grass = 2;
  ts.tiles[2].material = grass;
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

TEST_F(LevelModel, TileGroupsSpanOneCellTable)
{
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.slice = SliceParams{.tile_width = 16, .tile_height = 16};
  ts.image_width = 16 * 8;
  ts.image_height = 16 * 4;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 32UL);

  // A 2x2 house, and a tree whose top corners are holes
  const TileIndex house[] = {0, 1, 8, 9};
  const TileIndex tree[] = {kNoTileIndex, 2, kNoTileIndex, 10, 11, 12};
  const GroupId a = set_group(ts, "house", Extents{2, 2}, house);
  const GroupId b = set_group(ts, "tree", Extents{3, 2}, tree);
  ASSERT_EQ(a, 0);
  ASSERT_EQ(b, 1);
  EXPECT_EQ(find_group(ts, "tree"), b);
  EXPECT_EQ(find_group(ts, "rock"), kNoGroup);
  EXPECT_EQ(ts.group_cells.size(), 4UL + 6UL);
  EXPECT_EQ(ts.groups[b].first, 4U);
  EXPECT_EQ(group_cell(ts, ts.groups[a], 1, 1), 9);
  EXPECT_EQ(group_cell(ts, ts.groups[b], 0, 0), kNoTileIndex);
  EXPECT_EQ(group_cell(ts, ts.groups[b], 1, 0), 2);
  EXPECT_EQ(group_cell(ts, ts.groups[b], 2, 1), 12);
  // Outside the footprint is a hole, not a read of the next group's cells
  EXPECT_EQ(group_cell(ts, ts.groups[a], 2, 0), kNoTileIndex);

  // A group is filed under a folder, which is organisation and not identity: find_group still finds
  // it by name, and re-taking it from the palette keeps where it was filed
  ts.groups[a].folder = "nature/huts";
  EXPECT_EQ(find_group(ts, "house"), a);

  // A group of the same name is replaced, and a footprint that grew does not strand its old cells
  const TileIndex bigger[] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
  EXPECT_NE(set_group(ts, "house", Extents{3, 3}, bigger), kNoGroup);
  EXPECT_EQ(ts.groups.size(), 2UL);
  EXPECT_EQ(ts.group_cells.size(), 6UL + 9UL);
  // "tree" kept its cells although "house" moved to the end of the table
  const GroupId moved = find_group(ts, "tree");
  EXPECT_EQ(group_cell(ts, ts.groups[moved], 2, 1), 12);
  EXPECT_EQ(group_cell(ts, ts.groups[find_group(ts, "house")], 2, 2), 18);
  EXPECT_EQ(ts.groups[find_group(ts, "house")].folder, std::string_view{"nature/huts"});
  EXPECT_EQ(validate_groups(ts), 0UL);

  remove_group(ts, find_group(ts, "tree"));
  ASSERT_EQ(ts.groups.size(), 1UL);
  EXPECT_EQ(ts.group_cells.size(), 9UL);
  EXPECT_EQ(ts.groups[0].first, 0U);
  EXPECT_EQ(group_cell(ts, ts.groups[0], 2, 2), 18);
}

TEST_F(LevelModel, TileGroupsAreRefusedAndRepaired)
{
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.slice = SliceParams{.tile_width = 16, .tile_height = 16};
  ts.image_width = 16 * 4;
  ts.image_height = 16 * 2;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 8UL);

  const TileIndex one[] = {3};
  const TileIndex holes[] = {kNoTileIndex, kNoTileIndex};
  EXPECT_EQ(set_group(ts, "", Extents{1, 1}, one), kNoGroup);
  EXPECT_EQ(set_group(ts, "empty", Extents{0, 2}, one), kNoGroup);
  EXPECT_EQ(set_group(ts, "huge", Extents{kMaxGroupExtent + 1, 1}, one), kNoGroup);
  // A group of nothing but holes would stamp nothing
  EXPECT_EQ(set_group(ts, "holes", Extents{2, 1}, holes), kNoGroup);
  EXPECT_TRUE(ts.groups.empty());

  // A cell naming a tile the tileset does not have is taken as a hole rather than kept
  const TileIndex past[] = {3, 900};
  ASSERT_NE(set_group(ts, "edge", Extents{2, 1}, past), kNoGroup);
  EXPECT_EQ(group_cell(ts, ts.groups[0], 1, 0), kNoTileIndex);

  // Re-slicing to a coarser tile keeps the group's shape and holes the tiles it lost
  ts.slice.tile_width = 32;
  ts.slice.tile_height = 32;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 2UL);
  ASSERT_EQ(ts.groups.size(), 1UL);
  EXPECT_EQ(ts.groups[0].size, (Extents{2, 1}));
  EXPECT_EQ(group_cell(ts, ts.groups[0], 0, 0), kNoTileIndex);

  // Groups whose spans do not pack the cell table cannot say which cells are whose, so they all go
  ts.groups[0].first = 7;
  EXPECT_EQ(validate_groups(ts), 1UL);
  EXPECT_TRUE(ts.groups.empty());
  EXPECT_TRUE(ts.group_cells.empty());
}

TEST_F(LevelModel, StepTerrainReadsEitherBlockShape)
{
  // Three across and six down, as gft's sheets hold them: read across and down
  StepRules rules{arena};
  StepTerrain& stacked = rules.terrains[add_step_terrain(rules, "stacked")];
  Grid<TileRef> tall{arena};
  tall.assign(Extents{3, 6}, kEmptyTile);
  for (int y = 0; y < 6; ++y)
  {
    for (int x = 0; x < 3; ++x)
    {
      tall.at(x, y) = TileRef{0, static_cast<TileIndex>(y * 3 + x)};
    }
  }
  EXPECT_EQ(learn_step_layout(stacked, tall), kStepTypeCount);
  for (std::size_t i = 0; i < kStepTypeCount; ++i)
  {
    EXPECT_EQ(stacked.tiles[i], (TileRef{0, static_cast<TileIndex>(i)})) << "type " << i;
  }

  // Six across and three down: the step-ups in the left 3x3, the step-downs in the right one
  StepTerrain& side = rules.terrains[add_step_terrain(rules, "side")];
  Grid<TileRef> wide{arena};
  wide.assign(Extents{6, 3}, kEmptyTile);
  for (int y = 0; y < 3; ++y)
  {
    for (int x = 0; x < 6; ++x)
    {
      // Left block holds the outward rows (0, 2, 4), right block the inward ones (1, 3, 5)
      const int half = (x < 3) ? 0 : 1;
      wide.at(x, y) = TileRef{0, static_cast<TileIndex>((y * 2 + half) * 3 + (x - half * 3))};
    }
  }
  EXPECT_EQ(learn_step_layout(side, wide), kStepTypeCount);
  // Both layouts name the same tile for every type
  for (std::size_t i = 0; i < kStepTypeCount; ++i)
  {
    EXPECT_EQ(side.tiles[i], stacked.tiles[i]) << "type " << i;
  }
  // The left block is the outward nine-slice, read as one
  EXPECT_EQ(side.tiles[static_cast<std::size_t>(StepType::kOutwardTop)], (TileRef{0, 1}));
  EXPECT_EQ(side.tiles[static_cast<std::size_t>(StepType::kFlat)], (TileRef{0, 7}));
  EXPECT_EQ(side.tiles[static_cast<std::size_t>(StepType::kInwardBottomRight)], (TileRef{0, 3}));

  // Any other shape is refused and leaves the terrain alone
  StepTerrain& untouched = rules.terrains[add_step_terrain(rules, "untouched")];
  Grid<TileRef> square{arena};
  square.assign(Extents{4, 4}, TileRef{0, 1});
  EXPECT_EQ(learn_step_layout(untouched, square), 0UL);
  EXPECT_TRUE(untouched.tiles[0].empty());
}

TEST_F(LevelModel, BatchTileEditWritesOnlyTheFieldsItIsGiven)
{
  Level level{arena};
  create_default(level, Settings{.extents = {4, 4}});
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.image_width = 4 * 16;
  ts.image_height = 16;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 4UL);
  constexpr SemanticId grass = 1;
  constexpr MaterialId dirt = 2;
  ts.tiles[0].material = dirt;
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
  EXPECT_EQ(ts.tiles[0].material, dirt);
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
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  constexpr TileSetId id = 0;
  auto& ts = tilesets.back();
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[1].collision = collision::kSolid;
  ts.tiles[1].material = 1;
  auto& ground = plane<TileGrid>(level.layers[1]);
  ground.at(0, 0) = TileRef{id, 0};
  ground.at(1, 0) = TileRef{id, 1};
  const auto col = find_layer(level, LayerKind::kCollision);
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kReplace, arena), 1UL);
  const auto& mask = plane<MaskGrid>(level.layers[col]);
  EXPECT_EQ(mask.at(0, 0), collision::kNone);
  EXPECT_EQ(mask.at(1, 0), collision::kSolid);
  // Re-running changes nothing, so the count tells a real refresh from one that had nothing to do
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kReplace, arena), 0UL);

  const auto mat = add_layer(level, LayerKind::kMaterial, "materials");
  derive_materials(tilesets, level, mat, "default");
  EXPECT_EQ(plane<IdGrid>(level.layers[mat]).at(1, 0), ts.tiles[1].material);
}

TEST_F(LevelModel, DeriveCollisionReplacesOrOnlyAdds)
{
  Level level{arena};
  create_default(level, Settings{.extents = {3, 1}});
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  constexpr TileSetId id = 0;
  auto& ts = tilesets.back();
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[1].collision = collision::kSolid;
  plane<TileGrid>(level.layers[1]).at(1, 0) = TileRef{id, 1};

  // A wall of the level's own, which no tile accounts for: the water-shedding pass writes these, and
  // so does painting the mask by hand
  const auto col = find_layer(level, LayerKind::kCollision);
  auto& mask = plane<MaskGrid>(level.layers[col]);
  mask.at(2, 0) = collision::kSolid;

  // Adding sets the tile's cell and leaves the hand-painted one alone
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kAdd, arena), 1UL);
  EXPECT_EQ(mask.at(1, 0), collision::kSolid);
  EXPECT_EQ(mask.at(2, 0), collision::kSolid);
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kAdd, arena), 0UL);

  // Adding never takes a bit away, so the wall survives its tile going
  plane<TileGrid>(level.layers[1]).at(1, 0) = kEmptyTile;
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kAdd, arena), 0UL);
  EXPECT_EQ(mask.at(1, 0), collision::kSolid);

  // Replacing says exactly what the tiles say, so both of them go
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kReplace, arena), 2UL);
  EXPECT_EQ(mask.at(1, 0), collision::kNone);
  EXPECT_EQ(mask.at(2, 0), collision::kNone);

  // A hidden tile layer counts for neither mode
  plane<TileGrid>(level.layers[1]).at(1, 0) = TileRef{id, 1};
  level.layers[1].visible = false;
  EXPECT_EQ(derive_collision(tilesets, level, col, "default", CollisionDerive::kAdd, arena), 0UL);
  EXPECT_EQ(mask.at(1, 0), collision::kNone);
}

TEST_F(LevelModel, DeriveSemanticsSquashesTheChosenTileLayersTopFirst)
{
  constexpr SemanticId kGrass = 1;
  constexpr SemanticId kPath = 2;
  constexpr SemanticId kRock = 3;

  Level level{arena};
  create_default(level, Settings{.extents = {4, 1}});
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  constexpr TileSetId id = 0;
  auto& ts = tilesets.back();
  ts.image_width = 16 * 4;
  ts.image_height = 16;
  reslice(ts);
  ASSERT_EQ(ts.tiles.size(), 4UL);
  // Tile 0 has no semantic type at all, so it never has a say
  ts.tiles[1].semantic = kGrass;
  ts.tiles[2].semantic = kPath;
  ts.tiles[3].semantic = kRock;

  // Layer 1 is the default ground layer; two more go above it. Every plane is taken after the last
  // add_layer, since adding one may move the layer vector
  const auto middle = add_layer(level, LayerKind::kTiles, "middle");
  const auto top = add_layer(level, LayerKind::kTiles, "top");
  const auto semantic = add_layer(level, LayerKind::kSemantic, "semantics");
  auto& ground = plane<TileGrid>(level.layers[1]);
  auto& middle_tiles = plane<TileGrid>(level.layers[middle]);
  auto& top_tiles = plane<TileGrid>(level.layers[top]);

  // Cell 0: grass only. Cell 1: grass with a path over it. Cell 2: grass, a path, and rock on top.
  // Cell 3: grass under a tile that has no semantic type of its own
  ground.at(0, 0) = TileRef{id, 1};
  ground.at(1, 0) = TileRef{id, 1};
  ground.at(2, 0) = TileRef{id, 1};
  ground.at(3, 0) = TileRef{id, 1};
  middle_tiles.at(1, 0) = TileRef{id, 2};
  middle_tiles.at(2, 0) = TileRef{id, 2};
  top_tiles.at(2, 0) = TileRef{id, 3};
  top_tiles.at(3, 0) = TileRef{id, 0};

  // All three layers, listed bottom-up
  const std::size_t all[] = {1, middle, top};
  std::size_t changed = derive_semantics(tilesets, level, semantic, all, 3, "default", false, arena);
  const auto& ids = plane<IdGrid>(level.layers[semantic]);
  EXPECT_EQ(changed, 4UL);
  EXPECT_EQ(ids.at(0, 0), kGrass);
  EXPECT_EQ(ids.at(1, 0), kPath);
  EXPECT_EQ(ids.at(2, 0), kRock);
  // A tile with no semantic type does not mask the one below it
  EXPECT_EQ(ids.at(3, 0), kGrass);

  // Precedence is the level's order, not the order the layers are listed in
  const std::size_t reversed[] = {top, middle, 1};
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, reversed, 3, "default", false, arena), 0UL);
  EXPECT_EQ(ids.at(2, 0), kRock);

  // Only the bottom layer: the path and the rock stop counting
  const std::size_t bottom[] = {1};
  changed = derive_semantics(tilesets, level, semantic, bottom, 1, "default", false, arena);
  EXPECT_EQ(changed, 2UL);
  for (int x = 0; x < 4; ++x)
  {
    EXPECT_EQ(ids.at(x, 0), kGrass) << x;
  }

  // Running it twice over changes nothing
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, bottom, 1, "default", false, arena), 0UL);
}

TEST_F(LevelModel, DeriveSemanticsClearsOrKeepsWhatTheTilesDoNotAccountFor)
{
  constexpr SemanticId kGrass = 1;
  constexpr SemanticId kPainted = 2;

  Level level{arena};
  create_default(level, Settings{.extents = {3, 1}});
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[1].semantic = kGrass;
  const auto semantic = add_layer(level, LayerKind::kSemantic, "semantics");
  plane<TileGrid>(level.layers[1]).at(0, 0) = TileRef{0, 1};
  auto& ids = plane<IdGrid>(level.layers[semantic]);
  // Cells 1 and 2 were painted by hand; no tile accounts for them
  ids.at(1, 0) = kPainted;
  ids.at(2, 0) = kPainted;

  const std::size_t layers[] = {1};
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, layers, 1, "default", true, arena), 1UL);
  EXPECT_EQ(ids.at(0, 0), kGrass);
  EXPECT_EQ(ids.at(1, 0), kPainted);
  EXPECT_EQ(ids.at(2, 0), kPainted);

  // Without keep_unmatched the layer says exactly what the tiles say
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, layers, 1, "default", false, arena), 2UL);
  EXPECT_EQ(ids.at(0, 0), kGrass);
  EXPECT_EQ(ids.at(1, 0), kNoSemantic);
  EXPECT_EQ(ids.at(2, 0), kNoSemantic);

  // No layers named at all: every cell is unmatched, so the layer is cleared entire or left alone
  ids.at(1, 0) = kPainted;
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, nullptr, 0, "default", true, arena), 0UL);
  EXPECT_EQ(ids.at(1, 0), kPainted);
  EXPECT_EQ(derive_semantics(tilesets, level, semantic, nullptr, 0, "default", false, arena), 2UL);
  EXPECT_EQ(ids.at(0, 0), kNoSemantic);
  EXPECT_EQ(ids.at(1, 0), kNoSemantic);
}

TEST_F(LevelModel, DeriveSemanticsReadsTheTileAsDrawnInTheSeason)
{
  constexpr SemanticId kGrass = 1;
  constexpr SemanticId kSnow = 2;

  Level level{arena};
  create_default(level, Settings{.extents = {1, 1}});
  TileSets tilesets = memory::make_vector<TileSet>(arena);
  tilesets.emplace_back(arena);
  auto& ts = tilesets.back();
  ts.image_width = 32;
  ts.image_height = 16;
  reslice(ts);
  ts.tiles[0].semantic = kGrass;
  ts.tiles[1].semantic = kSnow;
  set_seasonal_variant(ts, SeasonalVariant{.tile = 0, .season = "winter", .alternate = 1});
  const auto semantic = add_layer(level, LayerKind::kSemantic, "semantics");
  plane<TileGrid>(level.layers[1]).at(0, 0) = TileRef{0, 0};
  const auto& ids = plane<IdGrid>(level.layers[semantic]);
  const std::size_t layers[] = {1};

  derive_semantics(tilesets, level, semantic, layers, 1, "summer", false, arena);
  EXPECT_EQ(ids.at(0, 0), kGrass);
  derive_semantics(tilesets, level, semantic, layers, 1, "winter", false, arena);
  EXPECT_EQ(ids.at(0, 0), kSnow);
}

TEST_F(LevelModel, ClearingTilesetReferencesEmptiesAndRenumbersCells)
{
  Level level{arena};
  create_default(level, Settings{.extents = {2, 1}});
  auto& ground = plane<TileGrid>(level.layers[1]);
  ground.at(0, 0) = TileRef{0, 3};
  ground.at(1, 0) = TileRef{1, 4};
  clear_tileset_refs(level, 0);
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
  WfcRules wfc{arena};
  learn(wfc, ground);
  ASSERT_EQ(wfc.tiles.size(), 3UL);
  const WfcTileId a = find_tile(wfc, {0, 1});
  const WfcTileId b = find_tile(wfc, {0, 2});
  const WfcTileId c = find_tile(wfc, {0, 3});
  EXPECT_FLOAT_EQ(wfc.tiles[c].weight, 3.0F);
  EXPECT_TRUE(allowed(wfc, a, b, Direction::kEast));
  EXPECT_TRUE(allowed(wfc, b, a, Direction::kWest));
  EXPECT_TRUE(allowed(wfc, b, a, Direction::kEast));
  EXPECT_TRUE(allowed(wfc, c, a, Direction::kNorth));
  EXPECT_FALSE(allowed(wfc, a, a, Direction::kEast));
  EXPECT_FALSE(allowed(wfc, a, c, Direction::kNorth));
  set_allowed(wfc, a, a, Direction::kEast, true);
  EXPECT_TRUE(allowed(wfc, a, a, Direction::kWest));
  remove_tile(wfc, a);
  EXPECT_EQ(wfc.tiles.size(), 2UL);
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
  WfcRules rules{arena};
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
  WfcRules wfc{arena};
  learn(wfc, ground);
  const WfcTileId a = find_tile(wfc, {0, 1});
  const WfcTileId b = find_tile(wfc, {0, 2});
  const WfcTileId c = find_tile(wfc, {0, 3});
  EXPECT_TRUE(allowed(wfc, a, c, Direction::kSouthEast));
  EXPECT_TRUE(allowed(wfc, a, c, Direction::kSouthWest));
  EXPECT_TRUE(allowed(wfc, c, a, Direction::kNorthWest));
  EXPECT_TRUE(allowed(wfc, c, b, Direction::kNorthEast));
  EXPECT_FALSE(allowed(wfc, a, b, Direction::kSouthEast));
  EXPECT_FALSE(allowed(wfc, c, c, Direction::kNorthEast));
  EXPECT_EQ(diagonal_count(wfc), 4UL);  // a-c SE, a-c SW, b-c SE, b-c SW

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
  AutoTileRules auto_tiles{arena};
  AutoTerrain& terrain = auto_tiles.terrains[add_terrain(auto_tiles, arena, "grass")];
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
  clear_tileset_refs(auto_tiles, 0);
  ASSERT_EQ(terrain.variants.size(), 1U);
  EXPECT_EQ(terrain.variants[0].tile, (TileRef{0, 4}));

  remove_terrain(auto_tiles, 0);
  EXPECT_TRUE(auto_tiles.terrains.empty());
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
