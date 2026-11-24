/**
 * @copyright 2026-present Brian Cairl
 *
 * @file project.cpp
 */

// C++ Standard Library
#include <utility>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/project/project.hpp"

namespace lvl::project
{
namespace
{

/// Copies <code>count</code> names from <code>src</code> onto the end of the table
void append_names(memory::Vector<Name>& table, const Name* src, std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i)
  {
    table.push_back(src[i]);
  }
}

}  // namespace

Project::Project(memory::Arena& arena) :
    arena{&arena},
    semantics{memory::ArenaAllocator<level::SemanticType>{arena}},
    materials{memory::ArenaAllocator<level::Material>{arena}},
    seasons{memory::ArenaAllocator<level::Season>{arena}},
    tileset_files{memory::ArenaAllocator<Path>{arena}},
    rule_files{memory::ArenaAllocator<Path>{arena}},
    levels{memory::ArenaAllocator<LevelEntry>{arena}},
    object_names{memory::ArenaAllocator<Name>{arena}}
{
  semantics.push_back(level::SemanticType{.name = "none", .color = {0, 0, 0, 0}});
  materials.push_back(level::Material{.name = "none", .color = {0, 0, 0, 0}});
  seasons.push_back(level::Season{.name = "default"});
  // Reserved up front and never grown, so every rebuild reuses this one block (see kMaxObjectNames)
  object_names.reserve(kMaxObjectNames);
  levels.reserve(kMaxLevels);
}

void reset(Project& project)
{
  memory::Arena& arena = *project.arena;
  project.~Project();
  memory::clear(arena);
  new (&project) Project{arena};
}

void create_default(Project& project, std::string_view name, std::string_view root)
{
  reset(project);
  // An unnamed project is named after its folder, which is what the user sees on disk
  const std::string_view folder = file_name(root);
  project.name.assign(name.empty() ? (folder.empty() ? std::string_view{"untitled"} : folder) : name);
  project.root.assign(root);
}

std::size_t find_level(const Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.levels.size(); ++i)
  {
    if (project.levels[i].name == name)
    {
      return i;
    }
  }
  return kNoLevel;
}

std::size_t find_level_by_file(const Project& project, std::string_view file)
{
  for (std::size_t i = 0; i < project.levels.size(); ++i)
  {
    if (project.levels[i].file == file)
    {
      return i;
    }
  }
  return kNoLevel;
}

std::size_t find_level_by_file_name(const Project& project, std::string_view path)
{
  const std::string_view wanted = file_name(path);
  if (wanted.empty())
  {
    return kNoLevel;
  }
  for (std::size_t i = 0; i < project.levels.size(); ++i)
  {
    if (file_name(project.levels[i].file.view()) == wanted)
    {
      return i;
    }
  }
  return kNoLevel;
}

std::size_t add_level(Project& project, std::string_view name, std::string_view file)
{
  if (project.levels.size() >= kMaxLevels)
  {
    return kNoLevel;
  }
  if (name.empty() or find_level(project, name) != kNoLevel)
  {
    return kNoLevel;
  }
  LevelEntry entry;
  entry.name.assign(name);
  entry.file.assign(file);
  // A new member contributes no names yet, so its spans sit at the end of the table
  entry.portals_begin = static_cast<std::uint32_t>(project.object_names.size());
  entry.markers_begin = entry.portals_begin;
  project.levels.push_back(entry);
  return project.levels.size() - 1;
}

void remove_level(Project& project, std::size_t index, memory::Arena& scratch)
{
  LVL_ASSERT_LT(index, project.levels.size());
  project.levels.erase(project.levels.begin() + static_cast<std::ptrdiff_t>(index));
  // The removed member's names have to come out of the table, and every later span moves
  memory::Scope scope{scratch};
  memory::Vector<Name> rebuilt = memory::make_vector<Name>(scratch);
  rebuilt.reserve(project.object_names.size());
  for (LevelEntry& entry : project.levels)
  {
    const std::uint32_t portals = static_cast<std::uint32_t>(rebuilt.size());
    append_names(rebuilt, portals_of(project, entry), entry.portal_count);
    const std::uint32_t markers = static_cast<std::uint32_t>(rebuilt.size());
    append_names(rebuilt, markers_of(project, entry), entry.marker_count);
    entry.portals_begin = portals;
    entry.markers_begin = markers;
  }
  project.object_names.clear();
  append_names(project.object_names, rebuilt.data(), rebuilt.size());
}

bool rename_level(Project& project, std::size_t index, std::string_view name)
{
  LVL_ASSERT_LT(index, project.levels.size());
  if (name.empty())
  {
    return false;
  }
  const std::size_t existing = find_level(project, name);
  if (existing != kNoLevel and existing != index)
  {
    return false;
  }
  project.levels[index].name.assign(name);
  return true;
}

const Name* portals_of(const Project& project, const LevelEntry& entry)
{
  return project.object_names.data() + entry.portals_begin;
}

const Name* markers_of(const Project& project, const LevelEntry& entry)
{
  return project.object_names.data() + entry.markers_begin;
}

Status set_level_objects(Project& project, std::size_t index, const LevelObjects& objects, memory::Arena& scratch)
{
  LVL_ASSERT_LT(index, project.levels.size());
  memory::Scope scope{scratch};
  memory::Vector<Name> rebuilt = memory::make_vector<Name>(scratch);

  // The new table, member by member, taking the updated member's names from the caller
  for (std::size_t i = 0; i < project.levels.size(); ++i)
  {
    LevelEntry& entry = project.levels[i];
    const Name* portals = (i == index) ? objects.portals : portals_of(project, entry);
    const std::size_t portal_count = (i == index) ? objects.portal_count : entry.portal_count;
    const Name* markers = (i == index) ? objects.markers : markers_of(project, entry);
    const std::size_t marker_count = (i == index) ? objects.marker_count : entry.marker_count;

    const std::uint32_t portals_begin = static_cast<std::uint32_t>(rebuilt.size());
    append_names(rebuilt, portals, portal_count);
    const std::uint32_t markers_begin = static_cast<std::uint32_t>(rebuilt.size());
    append_names(rebuilt, markers, marker_count);

    if (rebuilt.size() > kMaxObjectNames)
    {
      // Refused rather than asserted: how many portals a level holds is the user's business
      return Status::error("the project's levels hold more than %zu portals and markers between them", kMaxObjectNames);
    }
    entry.portals_begin = portals_begin;
    entry.portal_count = static_cast<std::uint32_t>(portal_count);
    entry.markers_begin = markers_begin;
    entry.marker_count = static_cast<std::uint32_t>(marker_count);
  }
  project.levels[index].extents = objects.extents;
  project.object_names.clear();
  append_names(project.object_names, rebuilt.data(), rebuilt.size());
  return Status::okay();
}

void check_level_files(Project& project)
{
  for (LevelEntry& entry : project.levels)
  {
    Path absolute;
    entry.missing = !resolve(project.root.view(), entry.file.view(), absolute).ok or !file::exists(absolute.c_str());
  }
}

LinkState portal_link(const Project& project, std::string_view level, std::string_view portal)
{
  if (level.empty())
  {
    return LinkState::kSelf;
  }
  const std::size_t index = find_level(project, level);
  if (index == kNoLevel)
  {
    return LinkState::kUnknownLevel;
  }
  // An unnamed destination portal means "wherever the level puts arrivals", so it always resolves
  if (portal.empty())
  {
    return LinkState::kResolved;
  }
  const LevelEntry& entry = project.levels[index];
  const Name* names = portals_of(project, entry);
  for (std::uint32_t i = 0; i < entry.portal_count; ++i)
  {
    if (names[i] == portal)
    {
      return LinkState::kResolved;
    }
  }
  return LinkState::kUnknownPortal;
}

level::SemanticId intern_semantic(Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.semantics.size(); ++i)
  {
    if (project.semantics[i].name == name)
    {
      return static_cast<level::SemanticId>(i);
    }
  }
  project.semantics.push_back(level::SemanticType{.name = Name{name}});
  return static_cast<level::SemanticId>(project.semantics.size() - 1);
}

level::MaterialId intern_material(Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.materials.size(); ++i)
  {
    if (project.materials[i].name == name)
    {
      return static_cast<level::MaterialId>(i);
    }
  }
  project.materials.push_back(level::Material{.name = Name{name}});
  return static_cast<level::MaterialId>(project.materials.size() - 1);
}

level::SeasonId find_season(const Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.seasons.size(); ++i)
  {
    if (project.seasons[i].name == name)
    {
      return static_cast<level::SeasonId>(i);
    }
  }
  return static_cast<level::SeasonId>(project.seasons.size());
}

}  // namespace lvl::project
