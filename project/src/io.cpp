/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io.cpp
 *
 * The manifest, which looks like this:
 *
 * <pre>
 * {
 *   "format": "lvl-project", "version": 100,
 *   "name": "shoreline",
 *   "semantics": [...], "materials": [...], "seasons": [...],
 *   "tilesets": ["tilesets/forest.tileset.json"],
 *   "rules": ["rules/forest.rules.json"],
 *   "levels": [
 *     {"name": "overworld", "file": "levels/overworld.json", "size": [256, 256],
 *      "portals": ["west_gate"], "markers": ["spawn"]}
 *   ]
 * }
 * </pre>
 *
 * As in the level files, catalog entries are written from index 1 (the reserved "none" is implied)
 * and are interned by name on load, so a manifest can be merged into a project that already has
 * entries of those names.
 *
 * The tilesets and rule sets are files of their own, named here and written and read through
 * <code>level::save_tileset</code>/<code>save_wfc_rules</code> in the standalone formats. They are
 * read in the order the manifest lists them, because each one appends and so takes the next id --
 * the ids the member levels' cells hold. A tileset file that will not load therefore fails the whole
 * project rather than being skipped: skipping it would shift every id after it.
 */

// C++ Standard Library
#include <cstddef>
#include <string_view>
#include <utility>

// LVL
#include "lvl/core/build.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/project/io.hpp"
#include "lvl/project/paths.hpp"

namespace lvl::project
{
namespace
{

/// Runs <code>fn</code> with a JSON arena installed, as the level I/O does for its workers
template <typename Fn> decltype(auto) with_json(level::IoContext& ctx, Fn&& fn)
{
  LVL_ASSERT_NON_NULL(ctx.workers) << "an IoContext needs at least one worker";
  LVL_ASSERT_GT(ctx.worker_count, 0UL);
  if (json::is_installed())
  {
    return fn();
  }
  json::Installation installation{ctx.workers[0].json};
  return fn();
}

void write_format(Json& j)
{
  json::write(j, "format", Name{kProjectFormat});
  json::write(j, "version", kVersion);
}

bool check_format(json::Reader& r, const Json& j)
{
  Name format;
  int version = 0;
  r.get(j, "format", format);
  r.get(j, "version", version);
  if (!r.ok())
  {
    return false;
  }
  if (format != kProjectFormat)
  {
    return r.fail(
      "file holds '%s', expected '%.*s'",
      format.c_str(),
      static_cast<int>(kProjectFormat.size()),
      kProjectFormat.data());
  }
  if (version > kVersion)
  {
    return r.fail("project written by a newer editor (version %d)", version);
  }
  return true;
}

/// Writes <code>values</code> from index <code>first</code> as an array under <code>key</code>
template <typename T>
void save_list(Json& j, std::string_view key, const memory::Vector<T>& values, std::size_t first = 0)
{
  Json array = Json::array();
  for (std::size_t i = first; i < values.size(); ++i)
  {
    Json element;
    json::write(element, values[i]);
    array.push_back(std::move(element));
  }
  j[JsonString{key}] = std::move(array);
}

/// Reads catalog entries, interning them by name (a new entry takes its properties from the file)
template <typename T, typename InternFn>
bool load_catalog(json::Reader& r, const Json& j, std::string_view key, memory::Vector<T>& catalog, InternFn&& intern)
{
  const auto itr = j.find(key);
  if (itr == j.end())
  {
    return r.ok();
  }
  if (!r.array(*itr, key))
  {
    return false;
  }
  for (const auto& element : *itr)
  {
    T entry;
    if (!r.convert(element, entry, key))
    {
      return false;
    }
    const std::size_t before = catalog.size();
    const auto id = intern(entry.name.view());
    if (catalog.size() > before)
    {
      catalog[id] = entry;
    }
  }
  return r.ok();
}

/// Writes the root-relative file list under <code>key</code>
void save_files(Json& j, std::string_view key, const memory::Vector<Path>& files)
{
  Json array = Json::array();
  for (const Path& file : files)
  {
    Json element;
    json::write(element, file);
    array.push_back(std::move(element));
  }
  j[JsonString{key}] = std::move(array);
}

bool load_files(json::Reader& r, const Json& j, std::string_view key, memory::Vector<Path>& files)
{
  files.clear();
  const auto itr = j.find(key);
  if (itr == j.end())
  {
    return r.ok();
  }
  if (!r.array(*itr, key))
  {
    return false;
  }
  for (const auto& element : *itr)
  {
    Path file;
    if (!r.convert(element, file, key))
    {
      return false;
    }
    files.push_back(file);
  }
  return r.ok();
}

void save_names(Json& j, std::string_view key, const Name* names, std::size_t count)
{
  Json array = Json::array();
  for (std::size_t i = 0; i < count; ++i)
  {
    Json element;
    json::write(element, names[i]);
    array.push_back(std::move(element));
  }
  j[JsonString{key}] = std::move(array);
}

void save_levels(Json& j, const Project& project)
{
  Json array = Json::array();
  for (const LevelEntry& entry : project.levels)
  {
    Json element = Json::object();
    json::write(element, "name", entry.name);
    json::write(element, "file", entry.file);
    // A member that has never been saved has no size yet, and a grid of 0x0 is not a valid one, so
    // it is left out rather than written as something that would not read back
    if (entry.extents.width > 0 and entry.extents.height > 0)
    {
      json::write(element, "size", entry.extents);
    }
    save_names(element, "portals", portals_of(project, entry), entry.portal_count);
    save_names(element, "markers", markers_of(project, entry), entry.marker_count);
    array.push_back(std::move(element));
  }
  j[JsonString{"levels"}] = std::move(array);
}

/// Reads one member level's names straight onto the end of the table, and returns the span
bool load_names(
  json::Reader& r,
  const Json& j,
  std::string_view key,
  memory::Vector<Name>& table,
  std::uint32_t& begin,
  std::uint32_t& count)
{
  begin = static_cast<std::uint32_t>(table.size());
  count = 0;
  const auto itr = j.find(key);
  if (itr == j.end())
  {
    return r.ok();
  }
  if (!r.array(*itr, key))
  {
    return false;
  }
  for (const auto& element : *itr)
  {
    Name name;
    if (!r.convert(element, name, key))
    {
      return false;
    }
    if (table.size() >= kMaxObjectNames)
    {
      return r.fail("more than %zu portals and markers across the project's levels", kMaxObjectNames);
    }
    table.push_back(name);
    ++count;
  }
  return r.ok();
}

bool load_levels(json::Reader& r, const Json& j, Project& project)
{
  project.levels.clear();
  project.object_names.clear();
  const auto itr = j.find("levels");
  if (itr == j.end())
  {
    return r.ok();
  }
  if (!r.array(*itr, "levels"))
  {
    return false;
  }
  for (const auto& element : *itr)
  {
    if (!r.object(element, "levels"))
    {
      return false;
    }
    LevelEntry entry;
    r.get(element, "name", entry.name);
    r.get(element, "file", entry.file);
    r.get_optional(element, "size", entry.extents);
    if (!r.ok())
    {
      return false;
    }
    if (entry.name.empty())
    {
      return r.fail("a level has no name");
    }
    if (find_level(project, entry.name.view()) != kNoLevel)
    {
      // Portals address levels by name, so two members of one name would be ambiguous
      return r.fail("two levels are both named '%s'", entry.name.c_str());
    }
    if (project.levels.size() >= kMaxLevels)
    {
      return r.fail("a project holds at most %zu levels", kMaxLevels);
    }
    // The names go on the end of the shared table, in the order the members are read, so each
    // member's spans are contiguous and no rebuild is needed here
    if (!load_names(r, element, "portals", project.object_names, entry.portals_begin, entry.portal_count))
    {
      return false;
    }
    if (!load_names(r, element, "markers", project.object_names, entry.markers_begin, entry.marker_count))
    {
      return false;
    }
    project.levels.push_back(entry);
  }
  return r.ok();
}

}  // namespace

Status tileset_file(const Project& project, level::TileSetId id, Path& out)
{
  LVL_ASSERT_LT(id, project.tilesets.size());
  if (id < project.tileset_files.size() and !project.tileset_files[id].empty())
  {
    out = project.tileset_files[id];
    return Status::okay();
  }
  return project_file(Folder::kTileSets, project.tilesets[id].name.view(), kTileSetSuffix, out);
}

Status rule_set_file(const Project& project, std::size_t index, Path& out)
{
  LVL_ASSERT_LT(index, project.rules.size());
  if (index < project.rule_files.size() and !project.rule_files[index].empty())
  {
    out = project.rule_files[index];
    return Status::okay();
  }
  // A rule set's name is its WFC rules' name; one that was never named falls back to the project's
  const Name& named = project.rules[index].wfc.name;
  const std::string_view name = named.empty() ? project.name.view() : named.view();
  return project_file(Folder::kRules, name, kRuleSetSuffix, out);
}

Status save_project(const char* path, Project& project, level::IoContext& ctx)
{
  if (level::format_of(path) == level::FileFormat::kBinary)
  {
    return Status::error("a project manifest is always JSON; '%s' names a binary file", path);
  }
  // A project is where its manifest is, on the way out as on the way in: writing the manifest
  // somewhere else moves the project there, and its parts are written beside it
  const std::string_view folder = parent_path(std::string_view{path});
  project.root.assign(folder.empty() ? std::string_view{"."} : folder);
  LVL_RETURN_IF_ERROR(ensure_tree(project.root.view()));

  // Each part to its own file, and the paths chosen recorded, so the manifest names what was really
  // written and the next save reuses the same files
  project.tileset_files.resize(project.tilesets.size());
  for (std::size_t i = 0; i < project.tilesets.size(); ++i)
  {
    const auto id = static_cast<level::TileSetId>(i);
    Path file;
    LVL_RETURN_IF_ERROR(tileset_file(project, id, file));
    Path absolute;
    LVL_RETURN_IF_ERROR(resolve(project.root.view(), file.view(), absolute));
    LVL_RETURN_IF_ERROR(level::save_tileset(absolute.c_str(), project, id, ctx));
    project.tileset_files[i] = file;
  }
  project.rule_files.resize(project.rules.size());
  for (std::size_t i = 0; i < project.rules.size(); ++i)
  {
    Path file;
    LVL_RETURN_IF_ERROR(rule_set_file(project, i, file));
    Path absolute;
    LVL_RETURN_IF_ERROR(resolve(project.root.view(), file.view(), absolute));
    LVL_RETURN_IF_ERROR(level::save_wfc_rules(absolute.c_str(), project, i, ctx));
    project.rule_files[i] = file;
  }

  LVL_RETURN_IF_ERROR(with_json(ctx, [&] {
    return json::write_file(
      path,
      [&](Json& j) {
        j = Json::object();
        write_format(j);
        json::write(j, "name", project.name);
        save_list(j, "semantics", project.semantics, 1);
        save_list(j, "materials", project.materials, 1);
        save_list(j, "seasons", project.seasons);
        save_files(j, "tilesets", project.tileset_files);
        save_files(j, "rules", project.rule_files);
        save_levels(j, project);
      },
      2);
  }));
  LVL_LOG_INFO() << "Saved project '" << path << "'";
  return Status::okay();
}

Status load_project(const char* path, Project& project, level::IoContext& ctx)
{
  reset(project);
  // The project's location is where its manifest is, never what the manifest says
  const std::string_view folder = parent_path(std::string_view{path});
  project.root.assign(folder.empty() ? std::string_view{"."} : folder);
  // Named after its folder unless the manifest says otherwise
  const std::string_view derived = file_name(project.root.view());
  project.name.assign((derived.empty() or derived == ".") ? std::string_view{"untitled"} : derived);

  const Status status = with_json(ctx, [&] {
    return json::read_file(path, [&](const Json& j) {
      json::Reader r;
      if (!r.object(j, "project") or !check_format(r, j))
      {
        return r.status;
      }
      r.get_optional(j, "name", project.name);
      load_catalog(
        r, j, "semantics", project.semantics, [&project](std::string_view n) { return intern_semantic(project, n); });
      load_catalog(
        r, j, "materials", project.materials, [&project](std::string_view n) { return intern_material(project, n); });
      if (const auto seasons = j.find("seasons"); r.ok() and seasons != j.end() and r.array(*seasons, "seasons"))
      {
        // Seasons are an ordered cycle, not a set addressed by id, so a file replaces them
        project.seasons.clear();
        for (const auto& element : *seasons)
        {
          level::Season season;
          if (!r.convert(element, season, "seasons"))
          {
            break;
          }
          project.seasons.push_back(season);
        }
        if (project.seasons.empty())
        {
          project.seasons.push_back(level::Season{.name = "default"});
        }
      }
      if (r.ok())
      {
        load_files(r, j, "tilesets", project.tileset_files);
      }
      if (r.ok())
      {
        load_files(r, j, "rules", project.rule_files);
      }
      if (r.ok())
      {
        load_levels(r, j, project);
      }
      return r.status;
    });
  });
  if (!status.ok)
  {
    return status;
  }

  // The parts, in the order the manifest lists them: each tileset appends, so its position in the
  // list is the id the levels' cells hold. One that will not load fails the project.
  for (std::size_t i = 0; i < project.tileset_files.size(); ++i)
  {
    Path absolute;
    LVL_RETURN_IF_ERROR(resolve(project.root.view(), project.tileset_files[i].view(), absolute));
    level::TileSetId id = level::kNoTileSet;
    LVL_RETURN_IF_ERROR(level::load_tileset(absolute.c_str(), project, ctx, id));
    if (id != static_cast<level::TileSetId>(i))
    {
      return Status::error(
        "'%s' is the project's tileset %zu but read back as %u", absolute.c_str(), i, static_cast<unsigned>(id));
    }
  }
  // The rule sets, each in the file that holds it. A manifest that names none leaves the project
  // with the blank set it is created with, which is what a level naming nothing resolves to.
  if (!project.rule_files.empty())
  {
    // Each file names its set, and intern_rules decides where that set goes, so the list is rebuilt
    // from where the sets actually landed rather than assumed to come back in order. Copying it
    // first costs a few hundred bytes of the project arena, which was just reset.
    memory::Vector<Path> listed = memory::make_vector<Path>(*project.arena);
    listed.assign(project.rule_files.begin(), project.rule_files.end());
    project.rules.clear();
    project.rule_files.clear();
    project.rule_files.resize(listed.size());
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
      Path absolute;
      LVL_RETURN_IF_ERROR(resolve(project.root.view(), listed[i].view(), absolute));
      std::size_t index = kNoRules;
      LVL_RETURN_IF_ERROR(level::load_wfc_rules(absolute.c_str(), project, ctx, index));
      LVL_ASSERT_LT(index, project.rules.size());
      project.rule_files[index] = listed[i];
    }
    if (project.rules.size() != listed.size())
    {
      // Two of the files hold a set of the same name, so one has replaced the other and the
      // manifest's list no longer says where each set lives
      return Status::error(
        "'%s' names %zu rule files but they hold only %zu sets; two of them have the same name",
        path,
        listed.size(),
        project.rules.size());
    }
  }

  check_level_files(project);
  return Status::okay();
}

Status open_project(const char* root, Project& project, level::IoContext& ctx)
{
  Path manifest;
  LVL_RETURN_IF_ERROR(join(std::string_view{root}, kManifestName, manifest));
  if (!file::exists(manifest.c_str()))
  {
    return Status::error(
      "'%s' is not a project folder (no %.*s)", root, static_cast<int>(kManifestName.size()), kManifestName.data());
  }
  return load_project(manifest.c_str(), project, ctx);
}

Status create_project(const char* root, std::string_view name, Project& project, level::IoContext& ctx)
{
  LVL_RETURN_IF_ERROR(ensure_tree(std::string_view{root}));
  create_default(project, name, std::string_view{root});
  Path manifest;
  LVL_RETURN_IF_ERROR(join(std::string_view{root}, kManifestName, manifest));
  return save_project(manifest.c_str(), project, ctx);
}

}  // namespace lvl::project
