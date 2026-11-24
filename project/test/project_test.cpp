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
  p.tileset_files.push_back(Path{"tilesets/forest.tileset.json"});
  p.rule_files.push_back(Path{"rules/forest.rules.json"});

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
  ASSERT_EQ(read.tileset_files.size(), 1U);
  EXPECT_EQ(read.tileset_files[0], std::string_view{"tilesets/forest.tileset.json"});
  ASSERT_EQ(read.rule_files.size(), 1U);
  EXPECT_EQ(read.rule_files[0], std::string_view{"rules/forest.rules.json"});

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
