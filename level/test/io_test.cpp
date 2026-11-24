// C++ Standard Library
#include <array>
#include <cstdio>
#include <new>
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/level/io.hpp"
#include "lvl/project/project.hpp"

using namespace lvl;
using namespace lvl::level;

namespace
{

constexpr std::size_t kWorkers = 4;

class LevelIo : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(root, 512UL << 20, "root");
    memory::push(root, doc_a, 64UL << 20, "doc_a");
    memory::push(root, doc_b, 64UL << 20, "doc_b");
    memory::push(root, proj_a, 8UL << 20, "proj_a");
    memory::push(root, proj_b, 8UL << 20, "proj_b");
    pr_a = new (pr_a_storage) project::Project{proj_a};
    pr_b = new (pr_b_storage) project::Project{proj_b};
    for (std::size_t w = 0; w < kWorkers; ++w)
    {
      memory::push(root, workers[w].json, 16UL << 20, "json");
      memory::push(root, workers[w].scratch, 16UL << 20, "scratch");
    }
    // Deliberately not wired into the context: most tests exercise the fallback, worker 0's scratch
    memory::push(root, shared, 128UL << 20, "io-shared");
    ctx = IoContext{.workers = workers.data(), .worker_count = kWorkers, .progress = &progress};
  }

  void TearDown() override
  {
    pr_a->~Project();
    pr_b->~Project();
    memory::destroy(root);
  }

  static std::string temp(const char* name) { return std::string{::testing::TempDir()} + name; }

  /// A level using every feature, and the project data it draws on
  void populate(project::Project& project, Level& level)
  {
    // The fixture's projects outlive the local Levels, and some tests populate twice in a loop;
    // everything below appends, so start from an empty project rather than adding to the last one
    project::reset(project);
    create_default(
      level, Settings{.name = "meadow", .extents = {37, 23}, .cell_width = 16, .cell_height = 16, .seed = 99});
    // A fresh project starts with one "default" season; this level wants its own two
    project.seasons.clear();
    project.seasons.push_back(Season{.name = "summer"});
    project.seasons.push_back(
      Season{.name = "winter", .duration_days = 45, .temperature = -5.0F, .tint = {200, 220, 255, 255}});
    const SemanticId road = project::intern_semantic(project, "road");
    project.semantics[road].color = {120, 100, 80, 255};
    const MaterialId snow = project::intern_material(project, "snow");
    project.materials[snow].friction = 0.3F;
    const MaterialId grass = project::intern_material(project, "grass");

    const TileSetId id = project::add_tileset(project, "forest");
    TileSet& ts = project.tilesets[id];
    ts.image = "assets/forest.png";
    ts.slice = SliceParams{.tile_width = 16, .tile_height = 16, .spacing_x = 1, .spacing_y = 1};
    ts.image_width = 16 * 8 + 7;
    ts.image_height = 16 * 4 + 3;
    reslice(ts);
    ts.tiles[3] = TileInfo{
      .name = "rock", .semantic = road, .material = grass, .collision = collision::kSolid | collision::kSightBlocker};
    ts.tiles[4].hidden = true;
    ts.tiles[4].scatter_weight = 0.25F;
    ts.properties.push_back(TileProperty{.tile = 3, .key = "damage", .value = "2"});
    set_seasonal_variant(ts, SeasonalVariant{.tile = 3, .season = "winter", .alternate = 11, .material = snow});

    auto& height = plane<ScalarGrid>(level.layers[0]);
    for (std::size_t i = 0; i < height.values.size(); ++i)
    {
      height.values[i] = static_cast<float>(i % 97) / 97.0F + 0.123456789F;
    }
    auto& ground = plane<TileGrid>(level.layers[1]);
    for (std::size_t i = 0; i < ground.values.size(); ++i)
    {
      ground.values[i] = (i % 5 == 0) ? kEmptyTile : TileRef{id, static_cast<TileIndex>(i % ts.tiles.size())};
    }
    plane<MaskGrid>(level.layers[3]).at(4, 5) = collision::kSolid;
    const auto water = add_layer(level, LayerKind::kWater, "rivers");
    plane<ScalarGrid>(level.layers[water]).at(1, 2) = 0.5F;
    level.layers[water].preview = Preview::kHeat;
    const auto sem = add_layer(level, LayerKind::kSemantic, "semantics");
    plane<IdGrid>(level.layers[sem]).at(2, 2) = road;
    const auto lvls = add_layer(level, LayerKind::kLevel, "levels");
    plane<IdGrid>(level.layers[lvls]).at(3, 1) = 4;

    project.rules[0].wfc.name = "forest rules";
    level.rules = "forest rules";
    const WfcTileId a = add_tile(project.rules[0].wfc, TileRef{id, 1});
    const WfcTileId b = add_tile(project.rules[0].wfc, TileRef{id, 2});
    project.rules[0].wfc.tiles[b] = WfcTile{
      .tile = {id, 2},
      .weight = 3.0F,
      .height_min = 0.2F,
      .height_max = 0.6F,
      .water = WaterRule::kShore,
      .semantic = road};
    set_allowed(project.rules[0].wfc, a, b, Direction::kNorth, true);
    set_allowed(project.rules[0].wfc, a, a, Direction::kEast, true);
    set_allowed(project.rules[0].wfc, a, b, Direction::kNorthWest, true);
    project.rules[0].wfc.diagonals = true;

    // Auto-tiling terrains: a nine-slice blob and a side-only one
    AutoTerrain& shore =
      project.rules[0].auto_tiles.terrains[add_terrain(project.rules[0].auto_tiles, *project.arena, "shore")];
    shore.expand = 1;
    set_variant(shore, kAllNeighbors, TileRef{id, 3});
    set_variant(
      shore, mask_of(Direction::kEast) | mask_of(Direction::kSouth) | mask_of(Direction::kSouthEast), TileRef{id, 4});
    AutoTerrain& road_tiles =
      project.rules[0].auto_tiles.terrains[add_terrain(project.rules[0].auto_tiles, *project.arena, "road")];
    road_tiles.neighborhood = Neighborhood::kSides;
    road_tiles.expand = 0;
    set_variant(road_tiles, kSideNeighbors, TileRef{id, 1});

    // Height-step terrains: a band of levels each, with their eighteen tiles
    StepTerrain& low = project.rules[0].steps.terrains[add_step_terrain(project.rules[0].steps, "lowland")];
    low.level_lo = 0;
    low.level_hi = 1;
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      low.tiles[i] = TileRef{id, static_cast<TileIndex>(i % 5)};
    }
    StepTerrain& high = project.rules[0].steps.terrains[add_step_terrain(project.rules[0].steps, "highland")];
    high.level_lo = 2;
    high.level_hi = 7;
    high.tiles[static_cast<std::size_t>(StepType::kFlat)] = TileRef{id, 2};

    create_portal(
      level.registry,
      "cave",
      {3, 4},
      {2, 1},
      Portal{.target_level = "cave.json", .target_portal = "exit", .exit = Direction::kWest, .interact = true});
    create_marker(level.registry, "spawn", "player", {10.5F, 11.25F});
    const GraphId g0 = add_graph(level, "roads", GraphKind::kRoad);
    const GraphId g1 = add_graph(level, "guards", GraphKind::kPatrol);
    const Entity n0 = create_path_node(level.registry, g0, {0, 0});
    const Entity n1 = create_path_node(level.registry, g0, {5, 0});
    const Entity n2 = create_path_node(level.registry, g1, {1, 1});
    const Entity n3 = create_path_node(level.registry, g1, {2, 2});
    level.registry.get<PathNode>(n3).wait = 4.0F;
    create_path_edge(level.registry, n0, n1, 2.0F, false);
    create_path_edge(level.registry, n2, n3);
  }

  /// A level whose layers are each too big for one worker's arenas
  void populate_big(project::Project& project, Level& level)
  {
    project::reset(project);
    create_default(level, Settings{.name = "big", .extents = {1280, 1280}, .cell_width = 8, .cell_height = 8});
    const TileSetId id = project::add_tileset(project, "forest");
    TileSet& ts = project.tilesets[id];
    ts.slice = SliceParams{.tile_width = 8, .tile_height = 8};
    ts.image_width = 8 * 16;
    ts.image_height = 8 * 16;
    reslice(ts);
    auto& height = plane<ScalarGrid>(level.layers[0]);
    auto& ground = plane<TileGrid>(level.layers[1]);
    for (std::size_t i = 0; i < height.values.size(); ++i)
    {
      height.values[i] = static_cast<float>(i % 7919) * 0.1234567F;
      ground.values[i] = TileRef{id, static_cast<TileIndex>(i % ts.tiles.size())};
    }
  }

  /// The two files hold the same bytes
  void expect_same_file(const std::string& a_path, const std::string& b_path)
  {
    memory::Scope scope{shared};
    char* a = nullptr;
    char* b = nullptr;
    std::size_t na = 0;
    std::size_t nb = 0;
    ASSERT_TRUE(file::read(a_path.c_str(), shared, a, na).ok);
    ASSERT_TRUE(file::read(b_path.c_str(), shared, b, nb).ok);
    EXPECT_EQ(std::string_view(a, na), std::string_view(b, nb));
  }

  static void expect_equal_layers(const Level& a, const Level& b)
  {
    ASSERT_EQ(a.layers.size(), b.layers.size());
    for (std::size_t i = 0; i < a.layers.size(); ++i)
    {
      const Layer& la = a.layers[i];
      const Layer& lb = b.layers[i];
      EXPECT_EQ(la.name, lb.name);
      EXPECT_EQ(la.kind, lb.kind);
      EXPECT_EQ(la.visible, lb.visible);
      EXPECT_EQ(la.preview, lb.preview);
      EXPECT_FLOAT_EQ(la.opacity, lb.opacity);
      std::visit(
        [&lb](const auto& ga) {
          using G = std::decay_t<decltype(ga)>;
          const auto& gb = std::get<G>(lb.data);
          ASSERT_EQ(ga.values.size(), gb.values.size());
          for (std::size_t k = 0; k < ga.values.size(); ++k)
          {
            ASSERT_EQ(ga.values[k], gb.values[k]) << "cell " << k;
          }
        },
        la.data);
    }
  }

  static void expect_equal(const project::Project& pa, const Level& a, const project::Project& pb, const Level& b)
  {
    EXPECT_EQ(a.settings.name, b.settings.name);
    EXPECT_EQ(a.settings.extents, b.settings.extents);
    EXPECT_EQ(a.settings.seed, b.settings.seed);
    // Which rule set the level uses, which a standalone file says through the rules it carries
    EXPECT_EQ(a.rules, b.rules);
    ASSERT_EQ(pa.semantics.size(), pb.semantics.size());
    ASSERT_EQ(pa.materials.size(), pb.materials.size());
    for (std::size_t i = 0; i < pa.materials.size(); ++i)
    {
      EXPECT_EQ(pa.materials[i].name, pb.materials[i].name);
      EXPECT_FLOAT_EQ(pa.materials[i].friction, pb.materials[i].friction);
    }
    EXPECT_EQ(pa.semantics[1].color, pb.semantics[1].color);
    ASSERT_EQ(pa.seasons.size(), pb.seasons.size());
    EXPECT_EQ(pa.seasons[1].name, pb.seasons[1].name);
    EXPECT_FLOAT_EQ(pa.seasons[1].temperature, pb.seasons[1].temperature);
    EXPECT_EQ(pa.seasons[1].tint, pb.seasons[1].tint);

    ASSERT_EQ(pa.tilesets.size(), pb.tilesets.size());
    const TileSet& ta = pa.tilesets[0];
    const TileSet& tb = pb.tilesets[0];
    EXPECT_EQ(ta.image, tb.image);
    EXPECT_EQ(ta.slice.spacing_x, tb.slice.spacing_x);
    ASSERT_EQ(ta.tiles.size(), tb.tiles.size());
    EXPECT_EQ(tb.tiles[3].name, ta.tiles[3].name);
    EXPECT_EQ(tb.tiles[3].semantic, ta.tiles[3].semantic);
    EXPECT_EQ(tb.tiles[3].material, ta.tiles[3].material);
    EXPECT_EQ(tb.tiles[3].collision, ta.tiles[3].collision);
    EXPECT_TRUE(tb.tiles[4].hidden);
    EXPECT_FLOAT_EQ(tb.tiles[4].scatter_weight, 0.25F);
    EXPECT_FLOAT_EQ(tb.tiles[3].scatter_weight, 1.0F);
    ASSERT_EQ(tb.properties.size(), 1UL);
    EXPECT_EQ(tb.properties[0].value, ta.properties[0].value);
    EXPECT_EQ(seasonal_tile(tb, 3, "winter"), 11);
    EXPECT_EQ(seasonal_material(tb, 3, "winter"), seasonal_material(ta, 3, "winter"));

    expect_equal_layers(a, b);

    EXPECT_EQ(pa.rules[0].wfc.name, pb.rules[0].wfc.name);
    ASSERT_EQ(pa.rules[0].wfc.tiles.size(), pb.rules[0].wfc.tiles.size());
    EXPECT_EQ(pb.rules[0].wfc.tiles[1].water, WaterRule::kShore);
    EXPECT_FLOAT_EQ(pb.rules[0].wfc.tiles[1].height_max, 0.6F);
    EXPECT_EQ(pb.rules[0].wfc.tiles[1].semantic, pa.rules[0].wfc.tiles[1].semantic);
    EXPECT_EQ(pa.rules[0].wfc.adjacency.size(), pb.rules[0].wfc.adjacency.size());
    EXPECT_TRUE(allowed(pb.rules[0].wfc, 0, 1, Direction::kNorth));
    EXPECT_TRUE(allowed(pb.rules[0].wfc, 1, 0, Direction::kSouthEast));
    EXPECT_FALSE(allowed(pb.rules[0].wfc, 0, 1, Direction::kSouthEast));
    EXPECT_TRUE(pb.rules[0].wfc.diagonals);

    ASSERT_EQ(pb.rules[0].auto_tiles.terrains.size(), 2UL);
    const AutoTerrain& shore = pb.rules[0].auto_tiles.terrains[0];
    EXPECT_EQ(shore.name, std::string_view{"shore"});
    EXPECT_EQ(shore.neighborhood, Neighborhood::kBlob);
    EXPECT_EQ(shore.expand, 1);
    ASSERT_EQ(shore.variants.size(), 2UL);
    EXPECT_EQ(resolve(shore, kAllNeighbors), (TileRef{0, 3}));
    EXPECT_EQ(
      resolve(shore, mask_of(Direction::kEast) | mask_of(Direction::kSouth) | mask_of(Direction::kSouthEast)),
      (TileRef{0, 4}));
    EXPECT_EQ(pb.rules[0].auto_tiles.terrains[1].neighborhood, Neighborhood::kSides);
    EXPECT_EQ(pb.rules[0].auto_tiles.terrains[1].expand, 0);
    EXPECT_EQ(terrain_of(pb.rules[0].auto_tiles, TileRef{0, 4}), 0U);

    const std::size_t lvls = find_layer(b, LayerKind::kLevel);
    ASSERT_LT(lvls, b.layers.size());
    EXPECT_EQ(b.layers[lvls].name, std::string_view{"levels"});
    EXPECT_EQ(plane<IdGrid>(b.layers[lvls]).at(3, 1), 4);

    ASSERT_EQ(pb.rules[0].steps.terrains.size(), 2UL);
    const StepTerrain& low = pb.rules[0].steps.terrains[0];
    EXPECT_EQ(low.name, std::string_view{"lowland"});
    EXPECT_EQ(low.level_lo, 0);
    EXPECT_EQ(low.level_hi, 1);
    for (std::size_t i = 0; i < kStepTypeCount; ++i)
    {
      EXPECT_EQ(low.tiles[i], (TileRef{0, static_cast<TileIndex>(i % 5)})) << "step tile " << i;
    }
    EXPECT_EQ(pb.rules[0].steps.terrains[1].level_hi, 7);
    // The slots the highland terrain never filled come back empty rather than as tile 0
    EXPECT_EQ(pb.rules[0].steps.terrains[1].tiles[static_cast<std::size_t>(StepType::kFlat)], (TileRef{0, 2}));
    EXPECT_TRUE(pb.rules[0].steps.terrains[1].tiles[static_cast<std::size_t>(StepType::kOutwardTop)].empty());
    EXPECT_EQ(step_terrain_at(pb.rules[0].steps, 5), 1);

    ASSERT_EQ(a.graphs.size(), b.graphs.size());
    EXPECT_EQ(b.graphs[1].kind, GraphKind::kPatrol);
    EXPECT_EQ(b.registry.view<PathNode>().size(), 4UL);
    EXPECT_EQ(b.registry.view<PathEdge>().size(), 2UL);
    bool found_wait = false;
    for (auto [e, node] : b.registry.view<PathNode>().each())
    {
      found_wait = found_wait or (node.graph == 1 and node.wait == 4.0F);
    }
    EXPECT_TRUE(found_wait);
    for (auto [e, edge] : b.registry.view<PathEdge>().each())
    {
      EXPECT_EQ(b.registry.get<PathNode>(edge.from).graph, b.registry.get<PathNode>(edge.to).graph);
    }
    const Entity cave = find_portal(b.registry, "cave");
    ASSERT_NE(cave, kNullEntity);
    EXPECT_EQ(b.registry.get<Portal>(cave).target_portal, std::string_view{"exit"});
    EXPECT_EQ(b.registry.get<Portal>(cave).exit, Direction::kWest);
    EXPECT_TRUE(b.registry.get<Portal>(cave).interact);
    EXPECT_FLOAT_EQ(b.registry.get<Extent>(cave).width, 2.0F);
    EXPECT_EQ(b.registry.view<Marker>().size(), 1UL);
  }

  memory::Arena root;
  memory::Arena doc_a;
  memory::Arena doc_b;
  memory::Arena proj_a;
  memory::Arena proj_b;
  // The catalogs, seasons, tilesets and rule sets the levels draw on; one per document
  alignas(project::Project) unsigned char pr_a_storage[sizeof(project::Project)] = {};
  alignas(project::Project) unsigned char pr_b_storage[sizeof(project::Project)] = {};
  project::Project* pr_a = nullptr;
  project::Project* pr_b = nullptr;
  memory::Arena shared;
  std::array<IoWorker, kWorkers> workers;
  jobs::Progress progress;
  IoContext ctx;
};

}  // namespace

TEST_F(LevelIo, ProjectScopeLeavesTheSharedSectionsOutOfTheFile)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto path = temp("scoped.json");
  const auto saved = save_level(path.c_str(), *pr_a, original, ctx, LevelScope::kProject);
  ASSERT_TRUE(saved.ok) << saved;

  memory::Scope scope{shared};
  char* text = nullptr;
  std::size_t size = 0;
  ASSERT_TRUE(file::read(path.c_str(), shared, text, size).ok);
  const std::string_view doc{text, size};
  // Every top-level key sits at two spaces of indent on a line of its own, which is what tells one
  // apart from the same word appearing as a value -- a layer really is named "semantics" below.
  const auto top_level_key = [](const char* key) { return "\n  \"" + std::string{key} + "\""; };
  // What the project owns is not in the level's file
  for (const auto* key : {"semantics", "materials", "seasons", "tilesets", "wfc", "auto_tiles", "steps"})
  {
    EXPECT_EQ(doc.find(top_level_key(key)), std::string_view::npos) << key << " should not be in a project-scope level";
  }
  // What the level owns still is
  for (const auto* key : {"settings", "paths", "portals", "markers", "layers"})
  {
    EXPECT_NE(doc.find(top_level_key(key)), std::string_view::npos) << key;
  }
}

TEST_F(LevelIo, ProjectScopeRoundTripsAgainstAProjectThatAlreadyHoldsTheSharedData)
{
  for (const char* name : {"scoped.json", "scoped.lvlb"})
  {
    Level original{doc_a};
    populate(*pr_a, original);
    const auto path = temp(name);
    ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx, LevelScope::kProject).ok);

    // The cells hold project ids, so the target project must already have what they name. Build it
    // the same way, which is what a manifest will do.
    Level loaded{doc_b};
    populate(*pr_b, loaded);
    const auto before_tilesets = pr_b->tilesets.size();
    const auto before_rule_tiles = pr_b->rules[0].wfc.tiles.size();

    const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
    ASSERT_TRUE(status.ok) << status;
    // The level came back
    EXPECT_EQ(loaded.settings.name, original.settings.name);
    ASSERT_EQ(loaded.layers.size(), original.layers.size());
    EXPECT_EQ(plane<TileGrid>(loaded.layers[1]).values, plane<TileGrid>(original.layers[1]).values);
    EXPECT_NE(find_portal(loaded.registry, "cave"), kNullEntity);
    EXPECT_EQ(loaded.registry.view<PathNode>().size(), original.registry.view<PathNode>().size());
    // And the project was left alone rather than reloaded from the level
    EXPECT_EQ(pr_b->tilesets.size(), before_tilesets);
    EXPECT_EQ(pr_b->rules[0].wfc.tiles.size(), before_rule_tiles);
  }
}

TEST_F(LevelIo, AProjectScopeLevelNamesTheRuleSetItUses)
{
  for (const char* name : {"which_rules.json", "which_rules.lvlb"})
  {
    Level original{doc_a};
    populate(*pr_a, original);
    // A second set, as a project whose levels differ by biome has
    const std::size_t cave = project::add_rules(*pr_a, "cave rules");
    add_tile(pr_a->rules[cave].wfc, TileRef{0, 1});
    original.rules = "cave rules";

    const auto path = temp(name);
    ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx, LevelScope::kProject).ok);

    // The target project holds the sets at other indices than the one that wrote the file, so
    // resolving by name is what makes the level's cells tile with the rules it was drawn with
    Level loaded{doc_b};
    project::reset(*pr_b);
    project::add_tileset(*pr_b, "forest");
    project::intern_rules(*pr_b, "forest rules");
    const std::size_t cave_b = project::intern_rules(*pr_b, "cave rules");
    ASSERT_NE(cave_b, cave);
    ASSERT_EQ(project::find_rules(*pr_b, "cave rules"), cave_b);

    const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
    ASSERT_TRUE(status.ok) << status;
    EXPECT_EQ(loaded.rules, std::string_view{"cave rules"});
    EXPECT_EQ(project::rules_of(*pr_b, loaded), cave_b);
    // And the sets themselves were left alone: a project-scope file carries none of them
    EXPECT_EQ(pr_b->rules.size(), 3U);
    EXPECT_TRUE(pr_b->rules[cave_b].wfc.tiles.empty());
  }
}

TEST_F(LevelIo, AStandaloneLevelTakesItsRuleSetNameFromTheRulesItCarries)
{
  for (const char* name : {"carried.json", "carried.lvlb"})
  {
    Level original{doc_a};
    populate(*pr_a, original);
    // Two sets, and the level uses the second
    const std::size_t cave = project::add_rules(*pr_a, "cave rules");
    add_tile(pr_a->rules[cave].wfc, TileRef{0, 1});
    original.rules = "cave rules";

    const auto path = temp(name);
    ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx).ok);

    Level loaded{doc_b};
    project::reset(*pr_b);
    const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
    ASSERT_TRUE(status.ok) << status;
    // One set, the one the level used, and the level names it -- a standalone file carries exactly
    // the rules its cells were tiled with, so the other set is not in it at all
    ASSERT_EQ(pr_b->rules.size(), 1U);
    EXPECT_EQ(pr_b->rules[0].wfc.name, std::string_view{"cave rules"});
    EXPECT_EQ(pr_b->rules[0].wfc.tiles.size(), 1U);
    EXPECT_EQ(loaded.rules, std::string_view{"cave rules"});
  }
}

TEST_F(LevelIo, StandaloneScopeIsWhatALevelFileHasAlwaysBeen)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto implied = temp("implied.json");
  const auto explicit_path = temp("explicit.json");
  // kStandalone is the default, so an unscoped save and an explicit one are the same file
  ASSERT_TRUE(save_level(implied.c_str(), *pr_a, original, ctx).ok);
  ASSERT_TRUE(save_level(explicit_path.c_str(), *pr_a, original, ctx, LevelScope::kStandalone).ok);
  expect_same_file(implied, explicit_path);

  // And it still reads back on its own, into a project that knows nothing
  Level loaded{doc_b};
  project::reset(*pr_b);
  const auto status = load_level(implied.c_str(), *pr_b, loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal(*pr_a, original, *pr_b, loaded);
}

TEST_F(LevelIo, JsonRoundTrip)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto path = temp("level.json");
  const auto saved = save_level(path.c_str(), *pr_a, original, ctx);
  ASSERT_TRUE(saved.ok) << saved;
  EXPECT_FLOAT_EQ(progress.fraction(), 1.0F);

  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal(*pr_a, original, *pr_b, loaded);
  EXPECT_FLOAT_EQ(progress.fraction(), 1.0F);
  for (const auto& w : workers)
  {
    EXPECT_EQ(w.scratch.utilized, 0UL);
    EXPECT_EQ(w.json.utilized, 0UL);
  }
}

TEST_F(LevelIo, BinaryRoundTrip)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto path = temp("level.lvlb");
  ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx).ok);
  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal(*pr_a, original, *pr_b, loaded);
}

TEST_F(LevelIo, JsonToBinaryToJsonIsStable)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto json_a = temp("a.json");
  const auto bin = temp("a.lvlb");
  const auto json_b = temp("b.json");
  ASSERT_TRUE(save_level(json_a.c_str(), *pr_a, original, ctx).ok);
  Level loaded{doc_b};
  ASSERT_TRUE(load_level(json_a.c_str(), *pr_b, loaded, ctx).ok);
  ASSERT_TRUE(save_level(bin.c_str(), *pr_b, loaded, ctx).ok);
  ASSERT_TRUE(load_level(bin.c_str(), *pr_a, original, ctx).ok);
  ASSERT_TRUE(save_level(json_b.c_str(), *pr_a, original, ctx).ok);

  char* a = nullptr;
  char* b = nullptr;
  std::size_t na = 0;
  std::size_t nb = 0;
  ASSERT_TRUE(file::read(json_a.c_str(), workers[0].scratch, a, na).ok);
  ASSERT_TRUE(file::read(json_b.c_str(), workers[0].scratch, b, nb).ok);
  EXPECT_EQ(std::string_view(a, na), std::string_view(b, nb));
  memory::clear(workers[0].scratch);
}

TEST_F(LevelIo, StandaloneTilesetMovesBetweenLevels)
{
  for (const char* name : {"forest.tileset.json", "forest.tileset.bin"})
  {
    Level source{doc_a};
    populate(*pr_a, source);
    const auto path = temp(name);
    ASSERT_TRUE(save_tileset(path.c_str(), *pr_a, 0, ctx).ok);

    Level target{doc_b};
    create_default(target, Settings{});
    project::reset(*pr_b);
    project::intern_material(*pr_b, "lava");  // shifts material ids relative to the source project
    TileSetId id = kNoTileSet;
    const auto status = load_tileset(path.c_str(), *pr_b, ctx, id);
    ASSERT_TRUE(status.ok) << status;
    ASSERT_EQ(id, 0);
    const TileSet& ts = pr_b->tilesets[id];
    EXPECT_EQ(ts.tiles.size(), pr_a->tilesets[0].tiles.size());
    EXPECT_EQ(pr_b->materials[ts.tiles[3].material].name, std::string_view{"grass"});
    EXPECT_EQ(pr_b->semantics[ts.tiles[3].semantic].name, std::string_view{"road"});
    EXPECT_EQ(pr_b->materials[seasonal_material(ts, 3, "winter")].name, std::string_view{"snow"});
    EXPECT_FLOAT_EQ(pr_b->materials[seasonal_material(ts, 3, "winter")].friction, 0.3F);
  }
}

TEST_F(LevelIo, StandaloneWfcRulesAndPaths)
{
  for (const char* ext : {".json", ".bin"})
  {
    Level source{doc_a};
    populate(*pr_a, source);
    const auto wfc_path = temp((std::string{"rules"} + ext).c_str());
    const auto paths_path = temp((std::string{"paths"} + ext).c_str());
    ASSERT_TRUE(save_wfc_rules(wfc_path.c_str(), *pr_a, 0, ctx).ok);
    ASSERT_TRUE(save_paths(paths_path.c_str(), source, ctx).ok);

    Level target{doc_b};
    create_default(target, Settings{});
    project::reset(*pr_b);
    // WFC rules need their tilesets
    std::size_t set = project::kNoRules;
    EXPECT_FALSE(load_wfc_rules(wfc_path.c_str(), *pr_b, ctx, set).ok);
    project::add_tileset(*pr_b, "forest");
    const auto status = load_wfc_rules(wfc_path.c_str(), *pr_b, ctx, set);
    ASSERT_TRUE(status.ok) << status;
    // The file's set is added beside the project's own, which the reader does not touch
    EXPECT_EQ(set, 1U);
    ASSERT_EQ(pr_b->rules.size(), 2U);
    EXPECT_EQ(pr_b->rules[1].wfc.name, std::string_view{"forest rules"});
    EXPECT_EQ(pr_b->rules[set].wfc.tiles.size(), 2UL);
    EXPECT_TRUE(allowed(pr_b->rules[set].wfc, 0, 1, Direction::kNorth));
    EXPECT_EQ(pr_b->semantics[pr_b->rules[set].wfc.tiles[1].semantic].name, std::string_view{"road"});
    // The same file carries the auto-tiling terrains, remapped onto this level's tilesets
    ASSERT_EQ(pr_b->rules[set].auto_tiles.terrains.size(), 2UL);
    EXPECT_EQ(pr_b->rules[set].auto_tiles.terrains[0].name, std::string_view{"shore"});
    EXPECT_EQ(resolve(pr_b->rules[set].auto_tiles.terrains[0], kAllNeighbors), (TileRef{0, 3}));
    EXPECT_EQ(pr_b->rules[set].auto_tiles.terrains[1].neighborhood, Neighborhood::kSides);
    // As it does the height-step terrains
    ASSERT_EQ(pr_b->rules[set].steps.terrains.size(), 2UL);
    EXPECT_EQ(pr_b->rules[set].steps.terrains[0].name, std::string_view{"lowland"});
    EXPECT_EQ(pr_b->rules[set].steps.terrains[0].level_hi, 1);
    EXPECT_EQ(pr_b->rules[set].steps.terrains[0].tiles[static_cast<std::size_t>(StepType::kFlat)], (TileRef{0, 2}));
    EXPECT_TRUE(pr_b->rules[set].steps.terrains[1].tiles[static_cast<std::size_t>(StepType::kOutwardTop)].empty());
    // Loading again replaces rather than duplicates: the file names its set, and that set is there
    ASSERT_TRUE(load_wfc_rules(wfc_path.c_str(), *pr_b, ctx, set).ok);
    EXPECT_EQ(set, 1U);
    EXPECT_EQ(pr_b->rules.size(), 2U);
    EXPECT_EQ(pr_b->rules[set].auto_tiles.terrains.size(), 2UL);
    EXPECT_EQ(pr_b->rules[set].steps.terrains.size(), 2UL);

    const auto paths_status = load_paths(paths_path.c_str(), target, ctx);
    ASSERT_TRUE(paths_status.ok) << paths_status;
    EXPECT_EQ(target.graphs.size(), 2UL);
    EXPECT_EQ(target.registry.view<PathNode>().size(), 4UL);
    EXPECT_EQ(target.registry.view<PathEdge>().size(), 2UL);
    // Loading again replaces rather than duplicates
    ASSERT_TRUE(load_paths(paths_path.c_str(), target, ctx).ok);
    EXPECT_EQ(target.registry.view<PathNode>().size(), 4UL);
  }
}

TEST_F(LevelIo, BadFilesAreReportedNotFatal)
{
  Level level{doc_a};
  EXPECT_FALSE(load_level("/nonexistent.json", *pr_b, level, ctx).ok);

  const auto garbage = temp("garbage.json");
  ASSERT_TRUE(file::write(garbage.c_str(), "{\"format\": \"lvl-level\", \"layers\": [", 35).ok);
  EXPECT_FALSE(load_level(garbage.c_str(), *pr_b, level, ctx).ok);

  const auto wrong_kind = temp("wrong.json");
  const char text[] = R"({"format": "lvl-tileset", "version": 100})";
  ASSERT_TRUE(file::write(wrong_kind.c_str(), text, sizeof(text) - 1).ok);
  const auto status = load_level(wrong_kind.c_str(), *pr_b, level, ctx);
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.view().find("lvl-tileset"), std::string_view::npos) << status;

  // A layer with the wrong number of cells
  const auto bad_layer = temp("bad_layer.json");
  const char layer_text[] = R"({"format": "lvl-level", "version": 100,
    "settings": {"name": "x", "size": [2, 2]},
    "layers": [{"name": "h", "kind": "height", "data": [1, 2, 3]}]})";
  ASSERT_TRUE(file::write(bad_layer.c_str(), layer_text, sizeof(layer_text) - 1).ok);
  const auto layer_status = load_level(bad_layer.c_str(), *pr_b, level, ctx);
  EXPECT_FALSE(layer_status.ok);
  EXPECT_NE(layer_status.message.view().find("expected 4"), std::string_view::npos) << layer_status;

  // A truncated binary file
  Level original{doc_b};
  populate(*pr_a, original);
  const auto bin = temp("trunc.lvlb");
  ASSERT_TRUE(save_level(bin.c_str(), *pr_a, original, ctx).ok);
  char* data = nullptr;
  std::size_t size = 0;
  ASSERT_TRUE(file::read(bin.c_str(), workers[0].scratch, data, size).ok);
  ASSERT_TRUE(file::write(bin.c_str(), data, size - 10).ok);
  memory::clear(workers[0].scratch);
  EXPECT_FALSE(load_level(bin.c_str(), *pr_b, level, ctx).ok);
}

TEST_F(LevelIo, JsonSaveSplitsLayersIntoWaves)
{
  Level original{doc_a};
  populate(*pr_a, original);
  const auto one_wave = temp("waves_one.json");
  const auto many_waves = temp("waves_many.json");

  ctx.shared = &shared;
  ASSERT_TRUE(save_level(one_wave.c_str(), *pr_a, original, ctx).ok);
  // Every layer's text at once needs more than the arena the same save gets below
  constexpr std::size_t kTight = 24UL << 10;
  EXPECT_GT(shared.peak, kTight);

  memory::Arena tight;
  memory::push(shared, tight, kTight, "tight");
  ctx.shared = &tight;
  const auto status = save_level(many_waves.c_str(), *pr_a, original, ctx);
  ASSERT_TRUE(status.ok) << status;
  ctx.shared = nullptr;
  memory::pop(shared, tight);

  // Splitting the layers across waves is invisible in the file
  expect_same_file(one_wave, many_waves);

  Level loaded{doc_b};
  ctx.shared = &shared;
  ASSERT_TRUE(load_level(many_waves.c_str(), *pr_b, loaded, ctx).ok);
  expect_equal(*pr_a, original, *pr_b, loaded);
}

TEST_F(LevelIo, RoundTripsALevelBiggerThanOneWorkersShare)
{
  Level original{doc_a};
  populate_big(*pr_a, original);
  ctx.shared = &shared;

  for (const char* name : {"big.json", "big.lvlb"})
  {
    const auto path = temp(name);
    const auto saved = save_level(path.c_str(), *pr_a, original, ctx);
    ASSERT_TRUE(saved.ok) << saved;
    // What the shared arena is for: neither the file nor one layer's text fits a worker's scratch
    std::size_t size = 0;
    ASSERT_TRUE(file::size(path.c_str(), size).ok);
    EXPECT_GT(size, workers[0].scratch.capacity);

    Level loaded{doc_b};
    const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
    ASSERT_TRUE(status.ok) << status;
    expect_equal_layers(original, loaded);
  }
}

TEST_F(LevelIo, JsonSaveStreamsLayersTooBigToBuffer)
{
  Level original{doc_a};
  populate_big(*pr_a, original);
  const auto buffered = temp("streamed_a.json");
  const auto streamed = temp("streamed_b.json");

  ctx.shared = &shared;
  ASSERT_TRUE(save_level(buffered.c_str(), *pr_a, original, ctx).ok);

  // Not even one layer's text fits, so every layer goes straight to the file
  memory::Arena tiny;
  memory::push(shared, tiny, 1UL << 20, "tiny");
  ctx.shared = &tiny;
  const auto status = save_level(streamed.c_str(), *pr_a, original, ctx);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_LT(tiny.peak, 1UL << 20);
  ctx.shared = nullptr;
  memory::pop(shared, tiny);

  // Streaming a layer is invisible in the file
  ctx.shared = &shared;
  expect_same_file(buffered, streamed);

  Level loaded{doc_b};
  const auto reloaded = load_level(streamed.c_str(), *pr_b, loaded, ctx);
  ASSERT_TRUE(reloaded.ok) << reloaded;
  expect_equal_layers(original, loaded);
}

TEST_F(LevelIo, LoadsLayersWithoutStagingThemInWorkerArenas)
{
  Level original{doc_a};
  populate_big(*pr_a, original);
  const auto path = temp("no_staging.json");
  ctx.shared = &shared;
  ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx).ok);

  // Worker arenas far too small to hold a layer's cells, which is no longer where they land: the
  // workers scan the file's text straight into the document's planes
  std::array<IoWorker, 2> small;
  for (auto& worker : small)
  {
    memory::push(shared, worker.json, 1UL << 20, "small-json");
    memory::push(shared, worker.scratch, 256UL << 10, "small-scratch");
  }
  const std::size_t cells = plane<ScalarGrid>(original.layers[0]).values.size() * sizeof(float);
  ASSERT_LT(small[0].scratch.capacity, cells);
  IoContext small_ctx{.workers = small.data(), .worker_count = small.size(), .shared = &shared, .progress = &progress};

  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), *pr_b, loaded, small_ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal_layers(original, loaded);
}

TEST_F(LevelIo, RoundTripsLayerHeadersAtTheirWidest)
{
  Level original{doc_a};
  // One cell per layer, so a layer's text is almost all header: what the encode buffer is sized for
  create_default(original, Settings{.name = "widest", .extents = {1, 1}});
  // Names whose every character grows when written as JSON, and the longest a float's shortest
  // round-trip form gets
  constexpr float kWidest = -1.12854834e-32F;
  ASSERT_TRUE(original.layers[0].name.assign(std::string(Name::capacity() - 1, '"')));
  ASSERT_TRUE(original.layers[1].name.assign(std::string(Name::capacity() - 1, '\\')));
  for (auto& layer : original.layers)
  {
    layer.opacity = kWidest;
    layer.range_min = kWidest;
    layer.range_max = kWidest;
    layer.contour_interval = kWidest;
  }

  const auto path = temp("widest.json");
  ctx.shared = &shared;
  const auto saved = save_level(path.c_str(), *pr_a, original, ctx);
  ASSERT_TRUE(saved.ok) << saved;

  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), *pr_b, loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal_layers(original, loaded);
}

TEST_F(LevelIo, SingleWorkerWorksToo)
{
  ctx.worker_count = 1;
  Level original{doc_a};
  populate(*pr_a, original);
  const auto path = temp("single.json");
  ASSERT_TRUE(save_level(path.c_str(), *pr_a, original, ctx).ok);
  Level loaded{doc_b};
  ASSERT_TRUE(load_level(path.c_str(), *pr_b, loaded, ctx).ok);
  expect_equal(*pr_a, original, *pr_b, loaded);
}
