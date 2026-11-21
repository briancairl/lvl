// C++ Standard Library
#include <array>
#include <cstdio>
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/level/io.hpp"

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
    for (std::size_t w = 0; w < kWorkers; ++w)
    {
      memory::push(root, workers[w].json, 16UL << 20, "json");
      memory::push(root, workers[w].scratch, 16UL << 20, "scratch");
    }
    // Deliberately not wired into the context: most tests exercise the fallback, worker 0's scratch
    memory::push(root, shared, 128UL << 20, "io-shared");
    ctx = IoContext{.workers = workers.data(), .worker_count = kWorkers, .progress = &progress};
  }

  void TearDown() override { memory::destroy(root); }

  static std::string temp(const char* name) { return std::string{::testing::TempDir()} + name; }

  /// A level using every feature
  void populate(Level& level)
  {
    create_default(
      level, Settings{.name = "meadow", .extents = {37, 23}, .cell_width = 16, .cell_height = 16, .seed = 99});
    level.seasons[0].name = "summer";
    level.seasons.push_back(
      Season{.name = "winter", .duration_days = 45, .temperature = -5.0F, .tint = {200, 220, 255, 255}});
    const SemanticId road = intern_semantic(level, "road");
    level.semantics[road].color = {120, 100, 80, 255};
    const MaterialId snow = intern_material(level, "snow");
    level.materials[snow].friction = 0.3F;
    const MaterialId grass = intern_material(level, "grass");

    const TileSetId id = add_tileset(level, "forest");
    TileSet& ts = level.tilesets[id];
    ts.image = "assets/forest.png";
    ts.slice = SliceParams{.tile_width = 16, .tile_height = 16, .spacing_x = 1, .spacing_y = 1};
    ts.image_width = 16 * 8 + 7;
    ts.image_height = 16 * 4 + 3;
    reslice(ts);
    ts.tiles[3] = TileInfo{
      .name = "rock", .semantic = road, .material = grass, .collision = collision::kSolid | collision::kSightBlocker};
    ts.tiles[4].hidden = true;
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

    level.wfc.name = "forest rules";
    const WfcTileId a = add_tile(level.wfc, TileRef{id, 1});
    const WfcTileId b = add_tile(level.wfc, TileRef{id, 2});
    level.wfc.tiles[b] = WfcTile{
      .tile = {id, 2},
      .weight = 3.0F,
      .height_min = 0.2F,
      .height_max = 0.6F,
      .water = WaterRule::kShore,
      .semantic = road};
    set_allowed(level.wfc, a, b, Direction::kNorth, true);
    set_allowed(level.wfc, a, a, Direction::kEast, true);
    set_allowed(level.wfc, a, b, Direction::kNorthWest, true);
    level.wfc.diagonals = true;

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
  void populate_big(Level& level)
  {
    create_default(level, Settings{.name = "big", .extents = {1280, 1280}, .cell_width = 8, .cell_height = 8});
    const TileSetId id = add_tileset(level, "forest");
    TileSet& ts = level.tilesets[id];
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

  static void expect_equal(const Level& a, const Level& b)
  {
    EXPECT_EQ(a.settings.name, b.settings.name);
    EXPECT_EQ(a.settings.extents, b.settings.extents);
    EXPECT_EQ(a.settings.seed, b.settings.seed);
    ASSERT_EQ(a.semantics.size(), b.semantics.size());
    ASSERT_EQ(a.materials.size(), b.materials.size());
    for (std::size_t i = 0; i < a.materials.size(); ++i)
    {
      EXPECT_EQ(a.materials[i].name, b.materials[i].name);
      EXPECT_FLOAT_EQ(a.materials[i].friction, b.materials[i].friction);
    }
    EXPECT_EQ(a.semantics[1].color, b.semantics[1].color);
    ASSERT_EQ(a.seasons.size(), b.seasons.size());
    EXPECT_EQ(a.seasons[1].name, b.seasons[1].name);
    EXPECT_FLOAT_EQ(a.seasons[1].temperature, b.seasons[1].temperature);
    EXPECT_EQ(a.seasons[1].tint, b.seasons[1].tint);

    ASSERT_EQ(a.tilesets.size(), b.tilesets.size());
    const TileSet& ta = a.tilesets[0];
    const TileSet& tb = b.tilesets[0];
    EXPECT_EQ(ta.image, tb.image);
    EXPECT_EQ(ta.slice.spacing_x, tb.slice.spacing_x);
    ASSERT_EQ(ta.tiles.size(), tb.tiles.size());
    EXPECT_EQ(tb.tiles[3].name, ta.tiles[3].name);
    EXPECT_EQ(tb.tiles[3].semantic, ta.tiles[3].semantic);
    EXPECT_EQ(tb.tiles[3].material, ta.tiles[3].material);
    EXPECT_EQ(tb.tiles[3].collision, ta.tiles[3].collision);
    EXPECT_TRUE(tb.tiles[4].hidden);
    ASSERT_EQ(tb.properties.size(), 1UL);
    EXPECT_EQ(tb.properties[0].value, ta.properties[0].value);
    EXPECT_EQ(seasonal_tile(tb, 3, "winter"), 11);
    EXPECT_EQ(seasonal_material(tb, 3, "winter"), seasonal_material(ta, 3, "winter"));

    expect_equal_layers(a, b);

    EXPECT_EQ(a.wfc.name, b.wfc.name);
    ASSERT_EQ(a.wfc.tiles.size(), b.wfc.tiles.size());
    EXPECT_EQ(b.wfc.tiles[1].water, WaterRule::kShore);
    EXPECT_FLOAT_EQ(b.wfc.tiles[1].height_max, 0.6F);
    EXPECT_EQ(b.wfc.tiles[1].semantic, a.wfc.tiles[1].semantic);
    EXPECT_EQ(a.wfc.adjacency.size(), b.wfc.adjacency.size());
    EXPECT_TRUE(allowed(b.wfc, 0, 1, Direction::kNorth));
    EXPECT_TRUE(allowed(b.wfc, 1, 0, Direction::kSouthEast));
    EXPECT_FALSE(allowed(b.wfc, 0, 1, Direction::kSouthEast));
    EXPECT_TRUE(b.wfc.diagonals);

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
  memory::Arena shared;
  std::array<IoWorker, kWorkers> workers;
  jobs::Progress progress;
  IoContext ctx;
};

}  // namespace

TEST_F(LevelIo, JsonRoundTrip)
{
  Level original{doc_a};
  populate(original);
  const auto path = temp("level.json");
  const auto saved = save_level(path.c_str(), original, ctx);
  ASSERT_TRUE(saved.ok) << saved;
  EXPECT_FLOAT_EQ(progress.fraction(), 1.0F);

  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal(original, loaded);
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
  populate(original);
  const auto path = temp("level.lvlb");
  ASSERT_TRUE(save_level(path.c_str(), original, ctx).ok);
  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal(original, loaded);
}

TEST_F(LevelIo, JsonToBinaryToJsonIsStable)
{
  Level original{doc_a};
  populate(original);
  const auto json_a = temp("a.json");
  const auto bin = temp("a.lvlb");
  const auto json_b = temp("b.json");
  ASSERT_TRUE(save_level(json_a.c_str(), original, ctx).ok);
  Level loaded{doc_b};
  ASSERT_TRUE(load_level(json_a.c_str(), loaded, ctx).ok);
  ASSERT_TRUE(save_level(bin.c_str(), loaded, ctx).ok);
  ASSERT_TRUE(load_level(bin.c_str(), original, ctx).ok);
  ASSERT_TRUE(save_level(json_b.c_str(), original, ctx).ok);

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
    populate(source);
    const auto path = temp(name);
    ASSERT_TRUE(save_tileset(path.c_str(), source, 0, ctx).ok);

    Level target{doc_b};
    create_default(target, Settings{});
    intern_material(target, "lava");  // shifts material ids relative to the source level
    TileSetId id = kNoTileSet;
    const auto status = load_tileset(path.c_str(), target, ctx, id);
    ASSERT_TRUE(status.ok) << status;
    ASSERT_EQ(id, 0);
    const TileSet& ts = target.tilesets[id];
    EXPECT_EQ(ts.tiles.size(), source.tilesets[0].tiles.size());
    EXPECT_EQ(target.materials[ts.tiles[3].material].name, std::string_view{"grass"});
    EXPECT_EQ(target.semantics[ts.tiles[3].semantic].name, std::string_view{"road"});
    EXPECT_EQ(target.materials[seasonal_material(ts, 3, "winter")].name, std::string_view{"snow"});
    EXPECT_FLOAT_EQ(target.materials[seasonal_material(ts, 3, "winter")].friction, 0.3F);
  }
}

TEST_F(LevelIo, StandaloneWfcRulesAndPaths)
{
  for (const char* ext : {".json", ".bin"})
  {
    Level source{doc_a};
    populate(source);
    const auto wfc_path = temp((std::string{"rules"} + ext).c_str());
    const auto paths_path = temp((std::string{"paths"} + ext).c_str());
    ASSERT_TRUE(save_wfc_rules(wfc_path.c_str(), source, ctx).ok);
    ASSERT_TRUE(save_paths(paths_path.c_str(), source, ctx).ok);

    Level target{doc_b};
    create_default(target, Settings{});
    // WFC rules need their tilesets
    EXPECT_FALSE(load_wfc_rules(wfc_path.c_str(), target, ctx).ok);
    add_tileset(target, "forest");
    const auto status = load_wfc_rules(wfc_path.c_str(), target, ctx);
    ASSERT_TRUE(status.ok) << status;
    EXPECT_EQ(target.wfc.tiles.size(), 2UL);
    EXPECT_TRUE(allowed(target.wfc, 0, 1, Direction::kNorth));
    EXPECT_EQ(target.semantics[target.wfc.tiles[1].semantic].name, std::string_view{"road"});

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
  EXPECT_FALSE(load_level("/nonexistent.json", level, ctx).ok);

  const auto garbage = temp("garbage.json");
  ASSERT_TRUE(file::write(garbage.c_str(), "{\"format\": \"lvl-level\", \"layers\": [", 35).ok);
  EXPECT_FALSE(load_level(garbage.c_str(), level, ctx).ok);

  const auto wrong_kind = temp("wrong.json");
  const char text[] = R"({"format": "lvl-tileset", "version": 100})";
  ASSERT_TRUE(file::write(wrong_kind.c_str(), text, sizeof(text) - 1).ok);
  const auto status = load_level(wrong_kind.c_str(), level, ctx);
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.view().find("lvl-tileset"), std::string_view::npos) << status;

  // A layer with the wrong number of cells
  const auto bad_layer = temp("bad_layer.json");
  const char layer_text[] = R"({"format": "lvl-level", "version": 100,
    "settings": {"name": "x", "size": [2, 2]},
    "layers": [{"name": "h", "kind": "height", "data": [1, 2, 3]}]})";
  ASSERT_TRUE(file::write(bad_layer.c_str(), layer_text, sizeof(layer_text) - 1).ok);
  const auto layer_status = load_level(bad_layer.c_str(), level, ctx);
  EXPECT_FALSE(layer_status.ok);
  EXPECT_NE(layer_status.message.view().find("expected 4"), std::string_view::npos) << layer_status;

  // A truncated binary file
  Level original{doc_b};
  populate(original);
  const auto bin = temp("trunc.lvlb");
  ASSERT_TRUE(save_level(bin.c_str(), original, ctx).ok);
  char* data = nullptr;
  std::size_t size = 0;
  ASSERT_TRUE(file::read(bin.c_str(), workers[0].scratch, data, size).ok);
  ASSERT_TRUE(file::write(bin.c_str(), data, size - 10).ok);
  memory::clear(workers[0].scratch);
  EXPECT_FALSE(load_level(bin.c_str(), level, ctx).ok);
}

TEST_F(LevelIo, JsonSaveSplitsLayersIntoWaves)
{
  Level original{doc_a};
  populate(original);
  const auto one_wave = temp("waves_one.json");
  const auto many_waves = temp("waves_many.json");

  ctx.shared = &shared;
  ASSERT_TRUE(save_level(one_wave.c_str(), original, ctx).ok);
  // Every layer's text at once needs more than the arena the same save gets below
  constexpr std::size_t kTight = 24UL << 10;
  EXPECT_GT(shared.peak, kTight);

  memory::Arena tight;
  memory::push(shared, tight, kTight, "tight");
  ctx.shared = &tight;
  const auto status = save_level(many_waves.c_str(), original, ctx);
  ASSERT_TRUE(status.ok) << status;
  ctx.shared = nullptr;
  memory::pop(shared, tight);

  // Splitting the layers across waves is invisible in the file
  expect_same_file(one_wave, many_waves);

  Level loaded{doc_b};
  ctx.shared = &shared;
  ASSERT_TRUE(load_level(many_waves.c_str(), loaded, ctx).ok);
  expect_equal(original, loaded);
}

TEST_F(LevelIo, RoundTripsALevelBiggerThanOneWorkersShare)
{
  Level original{doc_a};
  populate_big(original);
  ctx.shared = &shared;

  for (const char* name : {"big.json", "big.lvlb"})
  {
    const auto path = temp(name);
    const auto saved = save_level(path.c_str(), original, ctx);
    ASSERT_TRUE(saved.ok) << saved;
    // What the shared arena is for: neither the file nor one layer's text fits a worker's scratch
    std::size_t size = 0;
    ASSERT_TRUE(file::size(path.c_str(), size).ok);
    EXPECT_GT(size, workers[0].scratch.capacity);

    Level loaded{doc_b};
    const auto status = load_level(path.c_str(), loaded, ctx);
    ASSERT_TRUE(status.ok) << status;
    expect_equal_layers(original, loaded);
  }
}

TEST_F(LevelIo, JsonSaveStreamsLayersTooBigToBuffer)
{
  Level original{doc_a};
  populate_big(original);
  const auto buffered = temp("streamed_a.json");
  const auto streamed = temp("streamed_b.json");

  ctx.shared = &shared;
  ASSERT_TRUE(save_level(buffered.c_str(), original, ctx).ok);

  // Not even one layer's text fits, so every layer goes straight to the file
  memory::Arena tiny;
  memory::push(shared, tiny, 1UL << 20, "tiny");
  ctx.shared = &tiny;
  const auto status = save_level(streamed.c_str(), original, ctx);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_LT(tiny.peak, 1UL << 20);
  ctx.shared = nullptr;
  memory::pop(shared, tiny);

  // Streaming a layer is invisible in the file
  ctx.shared = &shared;
  expect_same_file(buffered, streamed);

  Level loaded{doc_b};
  const auto reloaded = load_level(streamed.c_str(), loaded, ctx);
  ASSERT_TRUE(reloaded.ok) << reloaded;
  expect_equal_layers(original, loaded);
}

TEST_F(LevelIo, LoadsLayersWithoutStagingThemInWorkerArenas)
{
  Level original{doc_a};
  populate_big(original);
  const auto path = temp("no_staging.json");
  ctx.shared = &shared;
  ASSERT_TRUE(save_level(path.c_str(), original, ctx).ok);

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
  const auto status = load_level(path.c_str(), loaded, small_ctx);
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
  const auto saved = save_level(path.c_str(), original, ctx);
  ASSERT_TRUE(saved.ok) << saved;

  Level loaded{doc_b};
  const auto status = load_level(path.c_str(), loaded, ctx);
  ASSERT_TRUE(status.ok) << status;
  expect_equal_layers(original, loaded);
}

TEST_F(LevelIo, SingleWorkerWorksToo)
{
  ctx.worker_count = 1;
  Level original{doc_a};
  populate(original);
  const auto path = temp("single.json");
  ASSERT_TRUE(save_level(path.c_str(), original, ctx).ok);
  Level loaded{doc_b};
  ASSERT_TRUE(load_level(path.c_str(), loaded, ctx).ok);
  expect_equal(original, loaded);
}
