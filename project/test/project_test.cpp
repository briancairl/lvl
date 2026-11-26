/**
 * @copyright 2026-present Brian Cairl
 *
 * @file project_test.cpp
 *
 * The project manifest, the folder tree, path composition and copying assets into a project.
 */

// C++ Standard Library
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/json.hpp"
#include "lvl/level/level.hpp"
#include "lvl/project/io.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/project/project.hpp"

using namespace lvl;

namespace
{

/// One arena for the project, one for scratch, and the I/O workers, as the editor lays them out
struct Fixture
{
  Fixture()
  {
    memory::create(root, 32UL << 20, "test");
    memory::push(root, project_arena, 8UL << 20, "project");
    memory::push(root, scratch, 4UL << 20, "scratch");
    memory::push(root, worker.json, 4UL << 20, "io-json");
    memory::push(root, worker.scratch, 4UL << 20, "io-scratch");
    memory::push(root, shared, "io-shared");
    ctx.workers = &worker;
    ctx.worker_count = 1;
    ctx.shared = &shared;
  }

  ~Fixture() { memory::destroy(root); }

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  memory::Arena root;
  memory::Arena project_arena;
  memory::Arena scratch;
  memory::Arena shared;
  level::IoWorker worker;
  level::IoContext ctx;
};

/// A fresh empty directory under the test's temp dir
std::string temp_dir(const char* name)
{
  const std::filesystem::path dir = std::filesystem::path{::testing::TempDir()} / name;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

bool is_directory(const std::string& path)
{
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path{path}, ec);
}

bool exists(const std::string& path) { return file::exists(path.c_str()); }

void write_text_file(const std::string& path, std::string_view text)
{
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
  ASSERT_TRUE(file::write(path.c_str(), text.data(), text.size()).ok) << path;
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// Path composition
// -------------------------------------------------------------------------------------------------

TEST(Paths, FolderPaths)
{
  EXPECT_EQ(project::folder_path(project::Folder::kRoot), "");
  EXPECT_EQ(project::folder_path(project::Folder::kLevels), "levels");
  EXPECT_EQ(project::folder_path(project::Folder::kTileSets), "tilesets");
  EXPECT_EQ(project::folder_path(project::Folder::kRules), "rules");
  EXPECT_EQ(project::folder_path(project::Folder::kPaths), "paths");
  EXPECT_EQ(project::folder_path(project::Folder::kAtlases), "assets/atlases");
  EXPECT_EQ(project::folder_path(project::Folder::kImages), "assets/images");
  EXPECT_EQ(project::folder_path(project::Folder::kArtifacts), "artifacts");
}

TEST(Paths, Components)
{
  EXPECT_EQ(project::file_name("assets/atlases/a.png"), "a.png");
  EXPECT_EQ(project::file_name("a.png"), "a.png");
  EXPECT_EQ(project::file_name("/"), "");
  EXPECT_EQ(project::extension("a.tileset.json"), ".json");
  EXPECT_EQ(project::extension("a"), "");
  EXPECT_EQ(project::extension(".gitignore"), "");
  EXPECT_EQ(project::stem("a.tileset.json"), "a.tileset");
  EXPECT_EQ(project::stem("dir/a.png"), "a");
  EXPECT_EQ(project::parent_path("/x/y/z.json"), "/x/y");
  EXPECT_EQ(project::parent_path("z.json"), "");
  EXPECT_TRUE(project::is_absolute("/x/y"));
  EXPECT_TRUE(project::is_absolute("C:\\x"));
  EXPECT_FALSE(project::is_absolute("x/y"));
  EXPECT_FALSE(project::is_absolute(""));
}

TEST(Paths, Join)
{
  Path out;
  ASSERT_TRUE(project::join("/p", "levels", out).ok);
  EXPECT_EQ(out, std::string_view{"/p/levels"});
  // A trailing separator on the base and a leading one on the tail collapse to one
  ASSERT_TRUE(project::join("/p/", "/levels", out).ok);
  EXPECT_EQ(out, std::string_view{"/p/levels"});
  ASSERT_TRUE(project::join("/p", "", out).ok);
  EXPECT_EQ(out, std::string_view{"/p"});
  ASSERT_TRUE(project::join("", "levels", out).ok);
  EXPECT_EQ(out, std::string_view{"levels"});
}

TEST(Paths, JoinRefusesToTruncate)
{
  // A truncated path would name a different file, so it is a failure rather than a short string
  const std::string long_name(Path::kMaxLength, 'a');
  Path out;
  const Status s = project::join("/p", long_name, out);
  EXPECT_FALSE(s.ok);
  EXPECT_FALSE(s.message.empty());
}

TEST(Paths, ProjectFileMakesALegalNameFromAnyName)
{
  Path out;
  ASSERT_TRUE(project::project_file(project::Folder::kTileSets, "forest", project::kTileSetSuffix, out).ok);
  EXPECT_EQ(out, std::string_view{"tilesets/forest.tileset.json"});
  ASSERT_TRUE(project::project_file(project::Folder::kRules, "forest", project::kRuleSetSuffix, out).ok);
  EXPECT_EQ(out, std::string_view{"rules/forest.rules.json"});
  // The name is the user's: a run of characters a file name should not hold collapses to one '-',
  // and a leading or trailing one is dropped
  ASSERT_TRUE(project::project_file(project::Folder::kTileSets, " cliffs / rock! ", ".json", out).ok);
  EXPECT_EQ(out, std::string_view{"tilesets/cliffs-rock.json"});
  // Nothing usable left, so the file is still a legal one
  ASSERT_TRUE(project::project_file(project::Folder::kTileSets, "///", ".json", out).ok);
  EXPECT_EQ(out, std::string_view{"tilesets/unnamed.json"});
  ASSERT_TRUE(project::project_file(project::Folder::kTileSets, "", ".json", out).ok);
  EXPECT_EQ(out, std::string_view{"tilesets/unnamed.json"});
  // A name longer than a Name is cut to one rather than refused
  ASSERT_TRUE(project::project_file(project::Folder::kRoot, std::string(200, 'a'), ".json", out).ok);
  EXPECT_EQ(out.size(), Name::kMaxLength + 5);
}

TEST(Paths, ResolveAndRelativize)
{
  Path out;
  ASSERT_TRUE(project::resolve("/p", "assets/atlases/a.png", out).ok);
  EXPECT_EQ(out, std::string_view{"/p/assets/atlases/a.png"});
  // An absolute path is already resolved
  ASSERT_TRUE(project::resolve("/p", "/elsewhere/a.png", out).ok);
  EXPECT_EQ(out, std::string_view{"/elsewhere/a.png"});

  ASSERT_TRUE(project::relativize("/p", "/p/assets/atlases/a.png", out).ok);
  EXPECT_EQ(out, std::string_view{"assets/atlases/a.png"});
  // Outside the project it stays absolute, which is how an external reference is expressed
  ASSERT_TRUE(project::relativize("/p", "/elsewhere/a.png", out).ok);
  EXPECT_EQ(out, std::string_view{"/elsewhere/a.png"});
  // Project files always use '/'
  ASSERT_TRUE(project::relativize("/p", "/p/assets\\atlases\\a.png", out).ok);
  EXPECT_EQ(out, std::string_view{"assets/atlases/a.png"});
  // A sibling whose name merely starts with the root is not inside it
  EXPECT_FALSE(project::is_inside("/p", "/pp/a.png"));
  EXPECT_TRUE(project::is_inside("/p", "/p/a.png"));
  EXPECT_FALSE(project::is_inside("/p", "/p"));
}

TEST(Paths, RoundTripsThroughTheProjectRoot)
{
  Path absolute;
  Path relative;
  ASSERT_TRUE(project::resolve("/p", "assets/images/seed.png", absolute).ok);
  ASSERT_TRUE(project::relativize("/p", absolute.view(), relative).ok);
  EXPECT_EQ(relative, std::string_view{"assets/images/seed.png"});
}

// -------------------------------------------------------------------------------------------------
// The folder tree
// -------------------------------------------------------------------------------------------------

TEST(Tree, EnsureTreeIsIdempotent)
{
  const std::string root = temp_dir("tree");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  // Again, on a tree that is already there
  ASSERT_TRUE(project::ensure_tree(root).ok);
  for (std::size_t i = 0; i < project::kFolderCount; ++i)
  {
    Path dir;
    ASSERT_TRUE(project::subfolder(root, static_cast<project::Folder>(i), dir).ok);
    EXPECT_TRUE(is_directory(dir.c_str())) << dir.c_str();
  }
  EXPECT_FALSE(project::ensure_tree("").ok);
}

TEST(Tree, IsProjectRoot)
{
  const std::string root = temp_dir("is_root");
  EXPECT_FALSE(project::is_project_root(root));
  write_text_file(root + "/project.json", "{}");
  EXPECT_TRUE(project::is_project_root(root));
}

// -------------------------------------------------------------------------------------------------
// Copying assets into the project
// -------------------------------------------------------------------------------------------------

TEST(CopyIn, CopiesAnOutsideFileAndStoresItRelative)
{
  Fixture f;
  const std::string root = temp_dir("copy_in");
  const std::string outside = temp_dir("copy_src");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(outside + "/forest.png", "PNG-A");

  project::CopyPlan plan;
  ASSERT_TRUE(project::plan_copy(root, project::Folder::kAtlases, outside + "/forest.png", f.scratch, plan).ok);
  EXPECT_EQ(plan.destination, std::string_view{"assets/atlases/forest.png"});
  EXPECT_FALSE(plan.renamed);
  EXPECT_FALSE(plan.identical);
  EXPECT_FALSE(plan.inside);

  ASSERT_TRUE(project::apply_copy(root, plan, outside + "/forest.png", f.scratch).ok);
  EXPECT_TRUE(exists(root + "/assets/atlases/forest.png"));
}

TEST(CopyIn, ReusesIdenticalBytesRatherThanLitteringTheTree)
{
  Fixture f;
  const std::string root = temp_dir("copy_same");
  const std::string outside = temp_dir("copy_same_src");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(outside + "/forest.png", "PNG-A");
  write_text_file(root + "/assets/atlases/forest.png", "PNG-A");

  project::CopyPlan plan;
  ASSERT_TRUE(project::plan_copy(root, project::Folder::kAtlases, outside + "/forest.png", f.scratch, plan).ok);
  EXPECT_TRUE(plan.identical);
  EXPECT_FALSE(plan.renamed);
  EXPECT_EQ(plan.destination, std::string_view{"assets/atlases/forest.png"});
  // Nothing is written, and the one file that was there is untouched
  ASSERT_TRUE(project::apply_copy(root, plan, outside + "/forest.png", f.scratch).ok);
  EXPECT_FALSE(exists(root + "/assets/atlases/forest-2.png"));
}

TEST(CopyIn, RenamesAroundADifferentFileOfTheSameName)
{
  Fixture f;
  const std::string root = temp_dir("copy_clash");
  const std::string outside = temp_dir("copy_clash_src");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(outside + "/forest.png", "PNG-A");
  write_text_file(root + "/assets/atlases/forest.png", "PNG-DIFFERENT");

  project::CopyPlan plan;
  ASSERT_TRUE(project::plan_copy(root, project::Folder::kAtlases, outside + "/forest.png", f.scratch, plan).ok);
  EXPECT_TRUE(plan.renamed);
  EXPECT_FALSE(plan.identical);
  EXPECT_EQ(plan.destination, std::string_view{"assets/atlases/forest-2.png"});
  ASSERT_TRUE(project::apply_copy(root, plan, outside + "/forest.png", f.scratch).ok);
  EXPECT_TRUE(exists(root + "/assets/atlases/forest-2.png"));
}

TEST(CopyIn, AFileAlreadyInsideIsJustRelativized)
{
  Fixture f;
  const std::string root = temp_dir("copy_inside");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(root + "/assets/atlases/forest.png", "PNG-A");

  project::CopyPlan plan;
  ASSERT_TRUE(
    project::plan_copy(root, project::Folder::kAtlases, root + "/assets/atlases/forest.png", f.scratch, plan).ok);
  EXPECT_TRUE(plan.inside);
  EXPECT_EQ(plan.destination, std::string_view{"assets/atlases/forest.png"});
  EXPECT_TRUE(project::apply_copy(root, plan, root + "/assets/atlases/forest.png", f.scratch).ok);
}

TEST(CopyIn, AnOutsideFileIsOutsideWhateverIsRelative)
{
  // Every other copy test uses absolute paths for both, which is what a file dialog gives. The
  // editor's root is whatever the manifest was written to, though, and a source may be whatever was
  // typed, so either can be relative -- and resolving the source against the *root* made an outside
  // file look like one already inside, stored under a path that named nothing.
  Fixture f;
  const std::string root = temp_dir("copy_relative");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string outside = temp_dir("copy_relative_src");
  write_text_file(outside + "/forest.png", "PNG-A");

  // Both named relative to the working directory, which is what they are measured against
  std::error_code ec;
  const std::filesystem::path previous = std::filesystem::current_path(ec);
  ASSERT_FALSE(ec);
  std::filesystem::current_path(std::filesystem::path{::testing::TempDir()}, ec);
  ASSERT_FALSE(ec);

  project::CopyPlan plan;
  const Status planned =
    project::plan_copy("copy_relative", project::Folder::kAtlases, "copy_relative_src/forest.png", f.scratch, plan);
  EXPECT_TRUE(planned.ok) << planned.message.c_str();
  EXPECT_FALSE(plan.inside) << "a file beside the project is not in the project";
  EXPECT_EQ(plan.destination, std::string_view{"assets/atlases/forest.png"});
  const Status copied = project::apply_copy("copy_relative", plan, "copy_relative_src/forest.png", f.scratch);
  EXPECT_TRUE(copied.ok) << copied.message.c_str();
  EXPECT_TRUE(file::exists("copy_relative/assets/atlases/forest.png"));

  // And the copy, now in the tree, is recognised as inside it -- by a relative path either way
  project::CopyPlan second;
  ASSERT_TRUE(
    project::plan_copy(
      "copy_relative", project::Folder::kAtlases, "copy_relative/assets/atlases/forest.png", f.scratch, second)
      .ok);
  EXPECT_TRUE(second.inside);
  EXPECT_EQ(second.destination, std::string_view{"assets/atlases/forest.png"});

  std::filesystem::current_path(previous, ec);
}

TEST(CopyIn, RefusesAnEmptyChoice)
{
  Fixture f;
  const std::string root = temp_dir("copy_empty");
  project::CopyPlan plan;
  EXPECT_FALSE(project::plan_copy(root, project::Folder::kAtlases, "", f.scratch, plan).ok);
}

// -------------------------------------------------------------------------------------------------
// The level index
// -------------------------------------------------------------------------------------------------

TEST(Index, AddsFindsAndRenamesMembers)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  EXPECT_EQ(p.name, std::string_view{"shoreline"});

  const std::size_t over = project::add_level(p, "overworld", "levels/overworld.json");
  const std::size_t cave = project::add_level(p, "cave", "levels/cave.lvlb");
  ASSERT_NE(over, project::kNoLevel);
  ASSERT_NE(cave, project::kNoLevel);
  EXPECT_EQ(project::find_level(p, "cave"), cave);
  EXPECT_EQ(project::find_level(p, "nowhere"), project::kNoLevel);
  EXPECT_EQ(project::find_level_by_file(p, "levels/cave.lvlb"), cave);
  // How a portal that named its destination by path is matched to a member
  EXPECT_EQ(project::find_level_by_file_name(p, "../old/cave.lvlb"), cave);

  // A name already in use is refused, since portals address levels by name
  EXPECT_EQ(project::add_level(p, "cave", "levels/other.json"), project::kNoLevel);
  EXPECT_EQ(project::add_level(p, "", "levels/other.json"), project::kNoLevel);

  EXPECT_TRUE(project::rename_level(p, cave, "caverns"));
  EXPECT_EQ(project::find_level(p, "caverns"), cave);
  EXPECT_FALSE(project::rename_level(p, cave, "overworld"));
  EXPECT_EQ(p.levels[cave].name, std::string_view{"caverns"});
}

TEST(Index, HoldsEachMembersObjectNames)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  const std::size_t over = project::add_level(p, "overworld", "levels/overworld.json");
  const std::size_t cave = project::add_level(p, "cave", "levels/cave.json");

  const Name over_portals[] = {Name{"west_gate"}, Name{"cave_mouth"}};
  const Name over_markers[] = {Name{"spawn"}};
  const Name cave_portals[] = {Name{"exit"}};
  ASSERT_TRUE(
    project::set_level_objects(
      p,
      over,
      project::LevelObjects{
        .extents = {256, 256}, .portals = over_portals, .portal_count = 2, .markers = over_markers, .marker_count = 1},
      f.scratch)
      .ok);
  ASSERT_TRUE(
    project::set_level_objects(
      p, cave, project::LevelObjects{.extents = {64, 48}, .portals = cave_portals, .portal_count = 1}, f.scratch)
      .ok);

  EXPECT_EQ(p.levels[over].extents.width, 256);
  EXPECT_EQ(p.levels[cave].extents.height, 48);
  ASSERT_EQ(p.levels[over].portal_count, 2U);
  EXPECT_EQ(project::portals_of(p, p.levels[over])[0], std::string_view{"west_gate"});
  EXPECT_EQ(project::portals_of(p, p.levels[over])[1], std::string_view{"cave_mouth"});
  ASSERT_EQ(p.levels[over].marker_count, 1U);
  EXPECT_EQ(project::markers_of(p, p.levels[over])[0], std::string_view{"spawn"});
  ASSERT_EQ(p.levels[cave].portal_count, 1U);
  EXPECT_EQ(project::portals_of(p, p.levels[cave])[0], std::string_view{"exit"});

  // Re-saving the first level with fewer portals moves the second one's span, not its contents
  const Name fewer[] = {Name{"west_gate"}};
  ASSERT_TRUE(project::set_level_objects(
                p, over, project::LevelObjects{.extents = {256, 256}, .portals = fewer, .portal_count = 1}, f.scratch)
                .ok);
  EXPECT_EQ(p.levels[over].portal_count, 1U);
  ASSERT_EQ(p.levels[cave].portal_count, 1U);
  EXPECT_EQ(project::portals_of(p, p.levels[cave])[0], std::string_view{"exit"});
}

TEST(Index, ValidatesAPortalLinkWithoutLoadingTheTargetLevel)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  const std::size_t cave = project::add_level(p, "cave", "levels/cave.json");
  const Name cave_portals[] = {Name{"exit"}};
  ASSERT_TRUE(
    project::set_level_objects(
      p, cave, project::LevelObjects{.extents = {64, 48}, .portals = cave_portals, .portal_count = 1}, f.scratch)
      .ok);

  EXPECT_EQ(project::portal_link(p, "cave", "exit"), project::LinkState::kResolved);
  EXPECT_EQ(project::portal_link(p, "cave", ""), project::LinkState::kResolved);
  EXPECT_EQ(project::portal_link(p, "cave", "front_door"), project::LinkState::kUnknownPortal);
  EXPECT_EQ(project::portal_link(p, "attic", "exit"), project::LinkState::kUnknownLevel);
  EXPECT_EQ(project::portal_link(p, "", "anywhere"), project::LinkState::kSelf);
}

TEST(Index, RemovingAMemberCompactsTheNameTable)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  const std::size_t a = project::add_level(p, "a", "levels/a.json");
  const std::size_t b = project::add_level(p, "b", "levels/b.json");
  const Name a_portals[] = {Name{"a1"}, Name{"a2"}};
  const Name b_portals[] = {Name{"b1"}};
  ASSERT_TRUE(
    project::set_level_objects(p, a, project::LevelObjects{.portals = a_portals, .portal_count = 2}, f.scratch).ok);
  ASSERT_TRUE(
    project::set_level_objects(p, b, project::LevelObjects{.portals = b_portals, .portal_count = 1}, f.scratch).ok);
  ASSERT_EQ(p.object_names.size(), 3U);

  project::remove_level(p, a, f.scratch);
  ASSERT_EQ(p.levels.size(), 1U);
  EXPECT_EQ(p.levels[0].name, std::string_view{"b"});
  EXPECT_EQ(p.object_names.size(), 1U);
  ASSERT_EQ(p.levels[0].portal_count, 1U);
  EXPECT_EQ(project::portals_of(p, p.levels[0])[0], std::string_view{"b1"});
}

TEST(Catalogs, RemovingAnEntryRetiresItAndMovesNoOtherId)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  memory::Arena doc;
  memory::push(f.shared, doc, 4UL << 20, "doc");
  level::Level level{doc};
  level::create_default(level, level::Settings{.extents = {2, 1}});

  const level::SemanticId road = project::intern_semantic(p, "road");
  const level::SemanticId river = project::intern_semantic(p, "river");
  const level::MaterialId dirt = project::intern_material(p, "dirt");
  const level::MaterialId snow = project::intern_material(p, "snow");
  const auto sem = level::add_layer(level, level::LayerKind::kSemantic, "semantics");
  const auto mat = level::add_layer(level, level::LayerKind::kMaterial, "materials");
  level::plane<level::IdGrid>(level.layers[sem]).at(0, 0) = road;
  level::plane<level::IdGrid>(level.layers[sem]).at(1, 0) = river;
  level::plane<level::IdGrid>(level.layers[mat]).at(0, 0) = dirt;
  level::plane<level::IdGrid>(level.layers[mat]).at(1, 0) = snow;
  const level::TileSetId id = project::add_tileset(p, "t");
  auto& ts = p.tilesets[id];
  ts.image_width = 32;
  ts.image_height = 16;
  level::reslice(ts);
  ts.tiles[0] = level::TileInfo{.semantic = river, .material = snow};
  ts.tiles[1] = level::TileInfo{.semantic = road, .material = dirt};
  level::set_seasonal_variant(
    ts, level::SeasonalVariant{.tile = 0, .season = "winter", .alternate = 1, .material = snow});
  const level::WfcTileId w = level::add_tile(p.rules[0].wfc, level::TileRef{id, 0});
  p.rules[0].wfc.tiles[w].semantic = river;

  EXPECT_EQ(project::semantic_uses(p, level, road), 2UL);
  EXPECT_EQ(project::semantic_uses(p, level, river), 3UL);
  EXPECT_EQ(project::material_uses(p, level, snow), 3UL);

  project::remove_semantic(p, level, road);
  // The entry keeps its place, so nothing after it moves: the project's other levels are not
  // resident, and their cells would otherwise keep an id that had come to mean something else
  ASSERT_EQ(p.semantics.size(), 3UL);
  EXPECT_TRUE(p.semantics[road].retired);
  EXPECT_EQ(p.semantics[road].name, std::string_view{"road"});
  EXPECT_FALSE(p.semantics[river].retired);
  EXPECT_EQ(p.semantics[river].name, std::string_view{"river"});
  // What the project and the open level hold is cleared outright
  EXPECT_EQ(level::plane<level::IdGrid>(level.layers[sem]).at(0, 0), level::kNoSemantic);
  EXPECT_EQ(level::plane<level::IdGrid>(level.layers[sem]).at(1, 0), river);
  EXPECT_EQ(ts.tiles[0].semantic, river);
  EXPECT_EQ(ts.tiles[1].semantic, level::kNoSemantic);
  EXPECT_EQ(p.rules[0].wfc.tiles[w].semantic, river);

  project::remove_material(p, level, dirt);
  ASSERT_EQ(p.materials.size(), 3UL);
  EXPECT_TRUE(p.materials[dirt].retired);
  EXPECT_EQ(level::plane<level::IdGrid>(level.layers[mat]).at(0, 0), level::kNoMaterial);
  EXPECT_EQ(level::plane<level::IdGrid>(level.layers[mat]).at(1, 0), snow);
  EXPECT_EQ(ts.tiles[0].material, snow);
  EXPECT_EQ(ts.seasonal[0].material, snow);
  EXPECT_EQ(project::material_uses(p, level, snow), 3UL);

  // Interning the name again is that entry's id, not a duplicate -- and does not bring it back:
  // reading a file is what interns, and a file must not undo the project's decision to remove it
  EXPECT_EQ(project::intern_semantic(p, "road"), road);
  EXPECT_TRUE(p.semantics[road].retired);
  EXPECT_EQ(p.semantics.size(), 3UL);
}

TEST(Catalogs, CompactingRenumbersEveryLevelAtOnce)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  memory::Arena doc_a;
  memory::Arena doc_b;
  memory::push(f.shared, doc_a, 4UL << 20, "doc-a");
  memory::push(f.shared, doc_b, 4UL << 20, "doc-b");
  level::Level open{doc_a};
  level::Level sibling{doc_b};
  for (level::Level* l : {&open, &sibling})
  {
    level::create_default(*l, level::Settings{.extents = {3, 1}});
    level::add_layer(*l, level::LayerKind::kSemantic, "semantics");
  }

  const level::SemanticId road = project::intern_semantic(p, "road");
  const level::SemanticId river = project::intern_semantic(p, "river");
  const level::SemanticId rock = project::intern_semantic(p, "rock");
  const auto sem_of = [](level::Level& l) -> level::IdGrid& {
    return level::plane<level::IdGrid>(l.layers[level::find_layer(l, level::LayerKind::kSemantic)]);
  };
  for (level::Level* l : {&open, &sibling})
  {
    sem_of(*l).at(0, 0) = road;
    sem_of(*l).at(1, 0) = river;
    sem_of(*l).at(2, 0) = rock;
  }
  const level::TileSetId id = project::add_tileset(p, "t");
  p.tilesets[id].image_width = 32;
  p.tilesets[id].image_height = 16;
  level::reslice(p.tilesets[id]);
  p.tilesets[id].tiles[0].semantic = rock;

  EXPECT_FALSE(project::needs_compaction(p));
  project::remove_semantic(p, open, river);
  EXPECT_TRUE(project::needs_compaction(p));
  // The sibling is not resident, so its cells still name the retired entry and the one after it
  EXPECT_EQ(sem_of(sibling).at(1, 0), river);
  EXPECT_EQ(sem_of(sibling).at(2, 0), rock);

  // Planned once, then applied to every level, and only then do the entries go -- until then the
  // plan's old ids are what the project's own vectors are indexed by
  project::CatalogPlan plan{f.scratch};
  project::plan_compaction(p, plan);
  EXPECT_EQ(plan.retired_semantics, 1UL);
  EXPECT_EQ(plan.retired_materials, 0UL);
  project::apply_compaction(plan, open);
  project::apply_compaction(plan, sibling);
  project::finish_compaction(p, plan);

  ASSERT_EQ(p.semantics.size(), 3UL);
  EXPECT_EQ(p.semantics[1].name, std::string_view{"road"});
  EXPECT_EQ(p.semantics[2].name, std::string_view{"rock"});
  EXPECT_FALSE(project::needs_compaction(p));
  // Both levels moved together, and the retired entry's cells are "none"
  for (level::Level* l : {&open, &sibling})
  {
    EXPECT_EQ(sem_of(*l).at(0, 0), 1) << "road";
    EXPECT_EQ(sem_of(*l).at(1, 0), level::kNoSemantic) << "the removed entry";
    EXPECT_EQ(sem_of(*l).at(2, 0), 2) << "rock";
  }
  // As did what the project itself holds
  EXPECT_EQ(p.tilesets[id].tiles[0].semantic, 2);
}

TEST(Catalogs, RemovingATilesetDoesBothHalvesAndRefusesAcrossSeveralLevels)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  memory::Arena doc;
  memory::push(f.shared, doc, 4UL << 20, "doc");
  level::Level level{doc};
  level::create_default(level, level::Settings{.extents = {2, 1}});

  ASSERT_EQ(project::add_tileset(p, "a"), 0);
  ASSERT_EQ(project::add_tileset(p, "b"), 1);
  EXPECT_EQ(project::find_tileset(p, "b"), 1U);
  EXPECT_EQ(project::find_tileset(p, "nowhere"), p.tilesets.size());
  auto& ground = level::plane<level::TileGrid>(level.layers[1]);
  ground.at(0, 0) = level::TileRef{0, 3};
  ground.at(1, 0) = level::TileRef{1, 4};

  // The tileset leaves the project and the level's references follow
  ASSERT_TRUE(project::remove_tileset(p, level, 0).ok);
  ASSERT_EQ(p.tilesets.size(), 1U);
  EXPECT_EQ(p.tilesets[0].name, std::string_view{"b"});
  EXPECT_TRUE(ground.at(0, 0).empty());
  EXPECT_EQ(ground.at(1, 0), (level::TileRef{0, 4}));

  // With several member levels only the open one could be renumbered, so it is refused outright
  project::add_level(p, "one", "levels/one.json");
  project::add_level(p, "two", "levels/two.json");
  const Status s = project::remove_tileset(p, level, 0);
  EXPECT_FALSE(s.ok);
  EXPECT_EQ(p.tilesets.size(), 1U);
}

TEST(Index, SeasonsAreAddedFoundAndRemoved)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  // A project starts with one season, as a level used to
  ASSERT_EQ(p.seasons.size(), 1U);
  EXPECT_EQ(p.seasons[0].name, std::string_view{"default"});

  const level::SeasonId winter = project::add_season(p, "winter");
  EXPECT_EQ(winter, 1);
  EXPECT_EQ(project::find_season(p, "winter"), winter);

  project::remove_season(p, 0);
  ASSERT_EQ(p.seasons.size(), 1U);
  EXPECT_EQ(p.seasons[0].name, std::string_view{"winter"});
  // The cycle must not become empty
  EXPECT_DEATH(project::remove_season(p, 0), "at least one season");
}

TEST(Index, InternsCatalogEntriesByName)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  // Index 0 is the reserved "none" entry, as in a level
  ASSERT_EQ(p.semantics.size(), 1U);
  EXPECT_EQ(p.semantics[0].name, std::string_view{"none"});
  const level::SemanticId grass = project::intern_semantic(p, "grass");
  EXPECT_EQ(grass, 1);
  EXPECT_EQ(project::intern_semantic(p, "grass"), grass);
  EXPECT_EQ(project::intern_material(p, "stone"), 1);
  EXPECT_EQ(project::find_season(p, "default"), 0);
  // Not found reads as one past the end, like level::find_season
  EXPECT_EQ(project::find_season(p, "winter"), static_cast<level::SeasonId>(p.seasons.size()));
}

TEST(Index, ResetEmptiesTheProject)
{
  Fixture f;
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", "/p");
  project::add_level(p, "a", "levels/a.json");
  project::intern_semantic(p, "grass");
  project::reset(p);
  EXPECT_TRUE(p.levels.empty());
  EXPECT_EQ(p.semantics.size(), 1U);
  EXPECT_EQ(p.name, std::string_view{"untitled"});
}

// -------------------------------------------------------------------------------------------------
// The manifest
// -------------------------------------------------------------------------------------------------

namespace
{

/// A project with something in every part of the manifest
void populate(project::Project& p, memory::Arena& scratch, const std::string& root)
{
  project::create_default(p, "shoreline", root);
  p.semantics[project::intern_semantic(p, "grass")].color = {10, 200, 30, 255};
  project::intern_semantic(p, "road");
  p.materials[project::intern_material(p, "stone")].friction = 0.5F;
  p.seasons.clear();
  p.seasons.push_back(level::Season{.name = "summer", .duration_days = 120, .temperature = 24.0F});
  p.seasons.push_back(level::Season{.name = "winter", .duration_days = 60, .temperature = -4.0F});
  // A real tileset and a real rule set: the manifest names their files, and saving the project
  // writes each one through the standalone format it already had
  const level::TileSetId ts = project::add_tileset(p, "forest");
  p.tilesets[ts].image.assign("assets/atlases/forest.png");
  p.tilesets[ts].image_width = 32;
  p.tilesets[ts].image_height = 16;
  level::reslice(p.tilesets[ts]);
  p.tilesets[ts].tiles[1].name.assign("cobble");
  p.tilesets[ts].tiles[1].semantic = project::intern_semantic(p, "road");
  // A tile group: it lives in the tileset, so the manifest carries it through that tileset's file
  const level::TileIndex rock[] = {0, level::kNoTileIndex};
  ASSERT_EQ(level::set_group(p.tilesets[ts], "rock", level::Extents{2, 1}, rock), 0);
  // Two scatter profiles, the second sharing the object table with the first
  const std::size_t forest = project::add_scatter(p, "forest");
  p.scatter[forest].settings = project::ScatterSettings{
    .count = 40,
    .spacing = 2,
    .attempts = 80,
    .use_band = true,
    .lo = 0.3F,
    .hi = 0.6F,
    .use_semantic = true,
    .semantic = project::intern_semantic(p, "grass"),
    .use_collision = true,
    .blocked = level::collision::kSolid | level::collision::kPlatform,
    .only_empty = false,
    .region = false};
  project::add_scatter_object(p, forest, project::ScatterObject{.tileset = "forest", .group = "rock", .weight = 3.0F});
  project::add_scatter_object(p, forest, project::ScatterObject{.tileset = "gone", .group = "hut", .weight = 0.5F});
  const std::size_t boulders = project::add_scatter(p, "boulders");
  project::add_scatter_object(p, boulders, project::ScatterObject{.tileset = "forest", .group = "rock"});
  p.rules[0].wfc.name = "forest";
  level::add_tile(p.rules[0].wfc, level::TileRef{ts, 1});
  level::set_allowed(p.rules[0].wfc, 0, 0, level::Direction::kEast, true);

  const std::size_t over = project::add_level(p, "overworld", "levels/overworld.json");
  const std::size_t cave = project::add_level(p, "cave", "levels/cave.lvlb");
  const Name over_portals[] = {Name{"west_gate"}, Name{"cave_mouth"}};
  const Name over_markers[] = {Name{"spawn"}};
  const Name cave_portals[] = {Name{"exit"}};
  ASSERT_TRUE(
    project::set_level_objects(
      p,
      over,
      project::LevelObjects{
        .extents = {256, 128}, .portals = over_portals, .portal_count = 2, .markers = over_markers, .marker_count = 1},
      scratch)
      .ok);
  ASSERT_TRUE(
    project::set_level_objects(
      p, cave, project::LevelObjects{.extents = {64, 48}, .portals = cave_portals, .portal_count = 1}, scratch)
      .ok);
}

}  // namespace

TEST(Scatter, ProfilesAndTheirObjectsSpanOneTable)
{
  Fixture f;
  project::Project p{f.project_arena};
  EXPECT_TRUE(p.scatter.empty());
  const std::size_t forest = project::add_scatter(p, "forest");
  const std::size_t rocks = project::add_scatter(p, "rocks");
  ASSERT_EQ(forest, 0U);
  ASSERT_EQ(rocks, 1U);
  EXPECT_EQ(project::find_scatter(p, "rocks"), rocks);
  EXPECT_EQ(project::find_scatter(p, "nothing"), project::kNoScatterProfile);
  // A name already taken, and no name at all, are both refused
  EXPECT_EQ(project::add_scatter(p, "forest"), project::kNoScatterProfile);
  EXPECT_EQ(project::add_scatter(p, ""), project::kNoScatterProfile);

  ASSERT_TRUE(project::add_scatter_object(p, forest, project::ScatterObject{.tileset = "t", .group = "oak"}));
  ASSERT_TRUE(project::add_scatter_object(p, rocks, project::ScatterObject{.tileset = "t", .group = "boulder"}));
  // An object added to the first profile moves the second's along, so the table stays packed
  ASSERT_TRUE(
    project::add_scatter_object(p, forest, project::ScatterObject{.tileset = "t", .group = "pine", .weight = 2.0F}));
  EXPECT_EQ(p.scatter_objects.size(), 3U);
  EXPECT_EQ(p.scatter[forest].count, 2U);
  EXPECT_EQ(p.scatter[rocks].count, 1U);
  EXPECT_EQ(p.scatter[rocks].first, 2U);
  EXPECT_EQ(project::objects_of(p, p.scatter[forest])[1].group, std::string_view{"pine"});
  EXPECT_EQ(project::objects_of(p, p.scatter[rocks])[0].group, std::string_view{"boulder"});
  EXPECT_EQ(project::validate_scatter(p), 0U);

  project::remove_scatter_object(p, forest, 0);
  EXPECT_EQ(p.scatter[forest].count, 1U);
  EXPECT_EQ(project::objects_of(p, p.scatter[forest])[0].group, std::string_view{"pine"});
  EXPECT_EQ(p.scatter[rocks].first, 1U);
  EXPECT_EQ(project::objects_of(p, p.scatter[rocks])[0].group, std::string_view{"boulder"});

  // Removing a profile takes its objects and leaves the rest where they were
  project::remove_scatter(p, forest);
  ASSERT_EQ(p.scatter.size(), 1U);
  EXPECT_EQ(p.scatter[0].name, std::string_view{"rocks"});
  EXPECT_EQ(p.scatter[0].first, 0U);
  EXPECT_EQ(p.scatter_objects.size(), 1U);
  EXPECT_EQ(project::objects_of(p, p.scatter[0])[0].group, std::string_view{"boulder"});

  // A profile with no objects has no span to read
  const std::size_t empty = project::add_scatter(p, "empty");
  EXPECT_EQ(project::objects_of(p, p.scatter[empty]), nullptr);

  // Spans that do not pack the table cannot say which objects are whose, so they all go
  p.scatter[0].first = 9;
  EXPECT_EQ(project::validate_scatter(p), 2U);
  EXPECT_TRUE(p.scatter.empty());
  EXPECT_TRUE(p.scatter_objects.empty());
}

TEST(Scatter, CompactingTheCatalogsMovesAProfilesGroundType)
{
  Fixture f;
  project::Project p{f.project_arena};
  level::Level lv{f.scratch};
  level::create_default(lv, level::Settings{.extents = {4, 4}});
  const auto grass = project::intern_semantic(p, "grass");
  const auto road = project::intern_semantic(p, "road");
  const std::size_t index = project::add_scatter(p, "trees");
  p.scatter[index].settings.use_semantic = true;
  p.scatter[index].settings.semantic = road;
  EXPECT_EQ(project::semantic_uses(p, lv, road), 1U);

  // "grass" retired and compacted away: "road" takes its id, and the profile follows it
  project::remove_semantic(p, lv, grass);
  project::CatalogPlan plan{f.scratch};
  project::plan_compaction(p, plan);
  project::apply_compaction(plan, lv);
  project::finish_compaction(p, plan);
  ASSERT_EQ(p.semantics.size(), 2U);
  EXPECT_EQ(p.semantics[1].name, std::string_view{"road"});
  EXPECT_EQ(p.scatter[index].settings.semantic, 1U);
}

TEST(Manifest, RoundTrips)
{
  Fixture f;
  const std::string root = temp_dir("manifest");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string manifest = root + "/project.json";

  project::Project written{f.project_arena};
  populate(written, f.scratch, root);
  ASSERT_TRUE(project::save_project(manifest.c_str(), written, f.ctx).ok);

  // A second arena, so the loaded project cannot be the same memory
  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  const Status s = project::load_project(manifest.c_str(), read, f.ctx);
  ASSERT_TRUE(s.ok) << s.message.c_str();

  EXPECT_EQ(read.name, std::string_view{"shoreline"});
  // The root comes from where the file was found, not from the file
  EXPECT_EQ(read.root, std::string_view{root});
  ASSERT_EQ(read.semantics.size(), 3U);
  EXPECT_EQ(read.semantics[1].name, std::string_view{"grass"});
  EXPECT_EQ(read.semantics[1].color.g, 200);
  EXPECT_EQ(read.semantics[2].name, std::string_view{"road"});
  ASSERT_EQ(read.materials.size(), 2U);
  EXPECT_EQ(read.materials[1].name, std::string_view{"stone"});
  EXPECT_FLOAT_EQ(read.materials[1].friction, 0.5F);
  ASSERT_EQ(read.seasons.size(), 2U);
  EXPECT_EQ(read.seasons[0].name, std::string_view{"summer"});
  EXPECT_EQ(read.seasons[1].duration_days, 60U);
  // The parts are files of their own, named after themselves, and their contents came back
  ASSERT_EQ(read.tileset_files.size(), 1U);
  EXPECT_EQ(read.tileset_files[0], std::string_view{"tilesets/forest.tileset.json"});
  ASSERT_EQ(read.rule_files.size(), 1U);
  EXPECT_EQ(read.rule_files[0], std::string_view{"rules/forest.rules.json"});
  ASSERT_EQ(read.tilesets.size(), 1U);
  EXPECT_EQ(read.tilesets[0].name, std::string_view{"forest"});
  EXPECT_EQ(read.tilesets[0].image, std::string_view{"assets/atlases/forest.png"});
  ASSERT_GT(read.tilesets[0].tiles.size(), 1U);
  EXPECT_EQ(read.tilesets[0].tiles[1].name, std::string_view{"cobble"});
  ASSERT_EQ(read.tilesets[0].groups.size(), 1U);
  EXPECT_EQ(read.tilesets[0].groups[0].name, std::string_view{"rock"});
  EXPECT_EQ(level::group_cell(read.tilesets[0], read.tilesets[0].groups[0], 0, 0), 0);
  EXPECT_EQ(level::group_cell(read.tilesets[0], read.tilesets[0].groups[0], 1, 0), level::kNoTileIndex);
  EXPECT_EQ(read.semantics[read.tilesets[0].tiles[1].semantic].name, std::string_view{"road"});
  EXPECT_EQ(read.rules[0].wfc.name, std::string_view{"forest"});
  ASSERT_EQ(read.rules[0].wfc.tiles.size(), 1U);
  EXPECT_TRUE(level::allowed(read.rules[0].wfc, 0, 0, level::Direction::kEast));

  // The scatter profiles, their criteria and the flat object table they span
  ASSERT_EQ(read.scatter.size(), 2U);
  EXPECT_EQ(read.scatter[0].name, std::string_view{"forest"});
  EXPECT_EQ(read.scatter[1].name, std::string_view{"boulders"});
  EXPECT_EQ(project::validate_scatter(read), 0U);
  const project::ScatterSettings& st = read.scatter[0].settings;
  EXPECT_EQ(st.count, 40);
  EXPECT_EQ(st.spacing, 2);
  EXPECT_EQ(st.attempts, 80);
  EXPECT_TRUE(st.use_band);
  EXPECT_FLOAT_EQ(st.lo, 0.3F);
  EXPECT_FLOAT_EQ(st.hi, 0.6F);
  EXPECT_TRUE(st.use_semantic);
  // Interned by name, so the id is whatever this project gave that entry
  EXPECT_EQ(read.semantics[st.semantic].name, std::string_view{"grass"});
  EXPECT_TRUE(st.use_collision);
  EXPECT_EQ(st.blocked, level::collision::kSolid | level::collision::kPlatform);
  EXPECT_FALSE(st.only_empty);
  EXPECT_FALSE(st.region);
  ASSERT_EQ(read.scatter[0].count, 2U);
  const project::ScatterObject* objects = project::objects_of(read, read.scatter[0]);
  ASSERT_NE(objects, nullptr);
  EXPECT_EQ(objects[0].group, std::string_view{"rock"});
  EXPECT_FLOAT_EQ(objects[0].weight, 3.0F);
  // An object naming a tileset the project does not have comes back as it was written: a profile
  // outlives what it was written against, and the editor lists it as dangling rather than dropping it
  EXPECT_EQ(objects[1].tileset, std::string_view{"gone"});
  ASSERT_EQ(read.scatter[1].count, 1U);
  EXPECT_EQ(project::objects_of(read, read.scatter[1])[0].group, std::string_view{"rock"});
  // A profile that uses none of the optional tests writes none of them
  EXPECT_TRUE(read.scatter[1].settings.use_band == false);
  EXPECT_TRUE(read.scatter[1].settings.use_semantic == false);

  ASSERT_EQ(read.levels.size(), 2U);
  EXPECT_EQ(read.levels[0].name, std::string_view{"overworld"});
  EXPECT_EQ(read.levels[0].file, std::string_view{"levels/overworld.json"});
  EXPECT_EQ(read.levels[0].extents.width, 256);
  EXPECT_EQ(read.levels[0].extents.height, 128);
  ASSERT_EQ(read.levels[0].portal_count, 2U);
  EXPECT_EQ(project::portals_of(read, read.levels[0])[1], std::string_view{"cave_mouth"});
  ASSERT_EQ(read.levels[0].marker_count, 1U);
  EXPECT_EQ(project::markers_of(read, read.levels[0])[0], std::string_view{"spawn"});
  ASSERT_EQ(read.levels[1].portal_count, 1U);
  EXPECT_EQ(project::portals_of(read, read.levels[1])[0], std::string_view{"exit"});
  // Neither level file was written, so both are flagged rather than silently linked
  EXPECT_TRUE(read.levels[0].missing);
  EXPECT_TRUE(read.levels[1].missing);

  // A link into a sibling resolves from the index alone
  EXPECT_EQ(project::portal_link(read, "cave", "exit"), project::LinkState::kResolved);
}

TEST(Manifest, IsStableWhenWrittenTwice)
{
  Fixture f;
  const std::string root = temp_dir("manifest_stable");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string first = root + "/project.json";
  const std::string second = root + "/again.json";

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  ASSERT_TRUE(project::save_project(first.c_str(), p, f.ctx).ok);

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  ASSERT_TRUE(project::load_project(first.c_str(), read, f.ctx).ok);
  ASSERT_TRUE(project::save_project(second.c_str(), read, f.ctx).ok);

  // Byte-for-byte: a load/save cycle must not churn the file
  memory::Scope scope{f.scratch};
  char* a = nullptr;
  char* b = nullptr;
  std::size_t size_a = 0;
  std::size_t size_b = 0;
  ASSERT_TRUE(file::read(first.c_str(), f.scratch, a, size_a).ok);
  ASSERT_TRUE(file::read(second.c_str(), f.scratch, b, size_b).ok);
  ASSERT_EQ(size_a, size_b);
  EXPECT_EQ(std::string_view(a, size_a), std::string_view(b, size_b));
}

TEST(Manifest, TracksWhichLevelFilesAreThere)
{
  Fixture f;
  const std::string root = temp_dir("manifest_missing");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(root + "/levels/overworld.json", "{}");
  const std::string manifest = root + "/project.json";

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  ASSERT_TRUE(project::load_project(manifest.c_str(), read, f.ctx).ok);
  EXPECT_FALSE(read.levels[0].missing);
  EXPECT_TRUE(read.levels[1].missing);
}

TEST(Manifest, CreateProjectWritesTheTreeAndTheManifest)
{
  Fixture f;
  const std::string root = temp_dir("create");
  project::Project p{f.project_arena};
  ASSERT_TRUE(project::create_project(root.c_str(), "shoreline", p, f.ctx).ok);
  EXPECT_TRUE(project::is_project_root(root));
  EXPECT_TRUE(is_directory(root + "/levels"));
  EXPECT_TRUE(is_directory(root + "/assets/atlases"));
  EXPECT_TRUE(is_directory(root + "/artifacts"));
  EXPECT_EQ(p.name, std::string_view{"shoreline"});
  EXPECT_TRUE(p.levels.empty());

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  ASSERT_TRUE(project::open_project(root.c_str(), read, f.ctx).ok);
  EXPECT_EQ(read.name, std::string_view{"shoreline"});
}

TEST(Manifest, NamesTheProjectAfterItsFolderWhenItHasNoName)
{
  Fixture f;
  const std::string root = temp_dir("unnamed_project");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  write_text_file(root + "/project.json", R"({"format": "lvl-project", "version": 100})");

  project::Project p{f.project_arena};
  ASSERT_TRUE(project::open_project(root.c_str(), p, f.ctx).ok);
  EXPECT_EQ(p.name, std::string_view{"unnamed_project"});
}

TEST(Manifest, RejectsBadFiles)
{
  Fixture f;
  const std::string root = temp_dir("manifest_bad");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  project::Project p{f.project_arena};

  struct Case
  {
    const char* name;
    const char* text;
  };
  const Case cases[] = {
    {"not_json.json", "{ this is not json"},
    {"not_an_object.json", "[1, 2, 3]"},
    {"wrong_format.json", R"({"format": "lvl-level", "version": 100})"},
    {"from_the_future.json", R"({"format": "lvl-project", "version": 99999})"},
    {"nameless_level.json", R"({"format": "lvl-project", "version": 100, "levels": [{"file": "levels/a.json"}]})"},
    {"duplicate_level.json",
     R"({"format": "lvl-project", "version": 100, "levels": [
          {"name": "a", "file": "levels/a.json"}, {"name": "a", "file": "levels/b.json"}]})"},
    {"level_is_not_an_object.json", R"({"format": "lvl-project", "version": 100, "levels": [7]})"},
    {"bad_size.json",
     R"({"format": "lvl-project", "version": 100, "levels": [
          {"name": "a", "file": "levels/a.json", "size": "big"}]})"},
    {"bad_tileset_list.json", R"({"format": "lvl-project", "version": 100, "tilesets": 7})"},
  };
  for (const Case& c : cases)
  {
    const std::string path = root + "/" + c.name;
    write_text_file(path, c.text);
    const Status s = project::load_project(path.c_str(), p, f.ctx);
    EXPECT_FALSE(s.ok) << c.name;
    EXPECT_FALSE(s.message.empty()) << c.name;
  }
  const Status missing = project::load_project((root + "/not_there.json").c_str(), p, f.ctx);
  EXPECT_FALSE(missing.ok);
  EXPECT_FALSE(project::open_project(temp_dir("no_manifest").c_str(), p, f.ctx).ok);
}

TEST(Project, RuleSetsAreFoundAddedAndInternedByName)
{
  Fixture f;
  project::Project p{f.project_arena};
  memory::Arena doc;
  memory::push(f.shared, doc, 4UL << 20, "doc");
  level::Level level{doc};

  // A project always holds one, as it always holds one season, so a level always has rules
  ASSERT_EQ(p.rules.size(), 1U);
  EXPECT_EQ(project::rules_of(p, level), 0U);

  const std::size_t cave = project::add_rules(p, "cave");
  EXPECT_EQ(cave, 1U);
  EXPECT_EQ(project::find_rules(p, "cave"), 1U);
  EXPECT_EQ(project::find_rules(p, "nowhere"), project::kNoRules);
  // Interning finds the set of that name rather than adding a second
  EXPECT_EQ(project::intern_rules(p, "cave"), 1U);
  EXPECT_EQ(project::intern_rules(p, "cliffs"), 2U);
  ASSERT_EQ(p.rules.size(), 3U);

  // A level names the set it uses; one naming nothing, or a set that has gone, takes the first
  level.rules = "cave";
  EXPECT_EQ(project::rules_of(p, level), 1U);
  level.rules = "never_existed";
  EXPECT_EQ(project::rules_of(p, level), 0U);
  level.rules.clear();
  EXPECT_EQ(project::rules_of(p, level), 0U);

  level.rules = "cliffs";
  project::remove_rules(p, 1);
  ASSERT_EQ(p.rules.size(), 2U);
  EXPECT_EQ(p.rules[1].wfc.name, std::string_view{"cliffs"});
  EXPECT_EQ(project::rules_of(p, level), 1U);
  // A project keeps at least one, so removing the last is a no-op
  project::remove_rules(p, 1);
  project::remove_rules(p, 0);
  EXPECT_EQ(p.rules.size(), 1U);
}

TEST(Manifest, ManyRuleSetsEachGetTheirOwnFile)
{
  Fixture f;
  const std::string root = temp_dir("manifest_rule_sets");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string manifest = root + "/project.json";

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  // A second set, so the levels of one project can differ by biome
  const std::size_t cave = project::add_rules(p, "cave");
  level::add_tile(p.rules[cave].wfc, level::TileRef{0, 0});
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);
  ASSERT_EQ(p.rule_files.size(), 2U);
  EXPECT_EQ(p.rule_files[0], std::string_view{"rules/forest.rules.json"});
  EXPECT_EQ(p.rule_files[1], std::string_view{"rules/cave.rules.json"});

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  const Status s = project::load_project(manifest.c_str(), read, f.ctx);
  ASSERT_TRUE(s.ok) << s.message.c_str();
  // Read in the order the manifest lists them, so each set keeps the index its levels name
  ASSERT_EQ(read.rules.size(), 2U);
  EXPECT_EQ(read.rules[0].wfc.name, std::string_view{"forest"});
  EXPECT_EQ(read.rules[1].wfc.name, std::string_view{"cave"});
  EXPECT_EQ(read.rules[0].wfc.tiles.size(), 1U);
  EXPECT_EQ(read.rules[1].wfc.tiles.size(), 1U);
}

TEST(Manifest, CarriesWhichCatalogEntriesAreRetired)
{
  Fixture f;
  const std::string root = temp_dir("manifest_retired");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string manifest = root + "/project.json";

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  memory::Arena doc;
  memory::push(f.shared, doc, 4UL << 20, "doc");
  level::Level level{doc};
  level::create_default(level, level::Settings{.extents = {1, 1}});
  // Already there (populate interned it), so this finds it rather than adding one
  const level::SemanticId grass = project::intern_semantic(p, "grass");
  ASSERT_EQ(grass, 1);
  project::remove_semantic(p, level, grass);
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  ASSERT_TRUE(project::load_project(manifest.c_str(), read, f.ctx).ok);
  // The id is still reserved, and still says why
  ASSERT_EQ(read.semantics.size(), p.semantics.size());
  EXPECT_EQ(read.semantics[grass].name, std::string_view{"grass"});
  EXPECT_TRUE(read.semantics[grass].retired);
  EXPECT_FALSE(read.semantics[grass + 1].retired);
  EXPECT_TRUE(project::needs_compaction(read));

  // The key is written only for the entry that has it, so a project with nothing retired writes
  // exactly what it always did
  memory::Scope scope{f.scratch};
  char* text = nullptr;
  std::size_t size = 0;
  ASSERT_TRUE(file::read(manifest.c_str(), f.scratch, text, size).ok);
  const std::string_view written{text, size};
  std::size_t occurrences = 0;
  for (std::size_t at = written.find("\"retired\""); at != std::string_view::npos;
       at = written.find("\"retired\"", at + 1))
  {
    ++occurrences;
  }
  EXPECT_EQ(occurrences, 1U);
}

TEST(Manifest, RefusesAProjectWhoseTilesetFileIsGone)
{
  Fixture f;
  const std::string root = temp_dir("manifest_lost_tileset");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  // A tileset's place in the list is the id the levels' cells hold, so a file that will not load
  // cannot be skipped: that would shift every tileset after it
  write_text_file(
    root + "/project.json", R"({"format": "lvl-project", "version": 100, "tilesets": ["tilesets/gone.tileset.json"]})");

  project::Project p{f.project_arena};
  const Status s = project::load_project((root + "/project.json").c_str(), p, f.ctx);
  EXPECT_FALSE(s.ok);
  EXPECT_FALSE(s.message.empty());
}

TEST(Manifest, APartKeepsTheFileItWasGiven)
{
  Fixture f;
  const std::string root = temp_dir("manifest_part_files");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  const std::string manifest = root + "/project.json";

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);
  ASSERT_EQ(p.tileset_files.size(), 1U);
  EXPECT_EQ(p.tileset_files[0], std::string_view{"tilesets/forest.tileset.json"});
  EXPECT_TRUE(file::exists((root + "/tilesets/forest.tileset.json").c_str()));
  EXPECT_TRUE(file::exists((root + "/rules/forest.rules.json").c_str()));

  // Renaming the tileset does not move its file: the project remembers where it put it, so a
  // rename never orphans the file the manifest already names
  p.tilesets[0].name.assign("woods");
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);
  EXPECT_EQ(p.tileset_files[0], std::string_view{"tilesets/forest.tileset.json"});
  EXPECT_FALSE(file::exists((root + "/tilesets/woods.tileset.json").c_str()));

  // A tileset added later gets a file of its own, from its own name
  project::add_tileset(p, "cliffs");
  ASSERT_TRUE(project::save_project(manifest.c_str(), p, f.ctx).ok);
  ASSERT_EQ(p.tileset_files.size(), 2U);
  EXPECT_EQ(p.tileset_files[1], std::string_view{"tilesets/cliffs.tileset.json"});
  EXPECT_TRUE(file::exists((root + "/tilesets/cliffs.tileset.json").c_str()));
}

TEST(Manifest, SavingElsewhereMovesTheProjectThere)
{
  Fixture f;
  const std::string root = temp_dir("manifest_from");
  const std::string elsewhere = temp_dir("manifest_to");
  ASSERT_TRUE(project::ensure_tree(root).ok);

  project::Project p{f.project_arena};
  populate(p, f.scratch, root);
  ASSERT_TRUE(project::save_project((root + "/project.json").c_str(), p, f.ctx).ok);
  // A project is where its manifest is, writing as well as reading, so its parts follow it and the
  // tree is created on the way
  ASSERT_TRUE(project::save_project((elsewhere + "/project.json").c_str(), p, f.ctx).ok);
  EXPECT_EQ(p.root, std::string_view{elsewhere});
  EXPECT_TRUE(file::exists((elsewhere + "/tilesets/forest.tileset.json").c_str()));
  EXPECT_TRUE(is_directory(elsewhere + "/levels"));

  memory::Arena other;
  memory::push(f.shared, other, 4UL << 20, "project-b");
  project::Project read{other};
  ASSERT_TRUE(project::open_project(elsewhere.c_str(), read, f.ctx).ok);
  ASSERT_EQ(read.tilesets.size(), 1U);
  EXPECT_EQ(read.tilesets[0].name, std::string_view{"forest"});
}

TEST(Manifest, RefusesToWriteABinaryManifest)
{
  Fixture f;
  const std::string root = temp_dir("manifest_binary");
  ASSERT_TRUE(project::ensure_tree(root).ok);
  project::Project p{f.project_arena};
  project::create_default(p, "shoreline", root);
  // The manifest is always JSON (see io.hpp); a .lvlb path is a mistake, not a format choice
  EXPECT_FALSE(project::save_project((root + "/project.lvlb").c_str(), p, f.ctx).ok);
}
