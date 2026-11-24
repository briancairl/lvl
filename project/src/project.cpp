/**
 * @copyright 2026-present Brian Cairl
 *
 * @file project.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    tilesets{memory::ArenaAllocator<level::TileSet>{arena}},
    rules{memory::ArenaAllocator<RuleSet>{arena}},
    tileset_files{memory::ArenaAllocator<Path>{arena}},
    rule_files{memory::ArenaAllocator<Path>{arena}},
    levels{memory::ArenaAllocator<LevelEntry>{arena}},
    object_names{memory::ArenaAllocator<Name>{arena}}
{
  semantics.push_back(level::SemanticType{.name = "none", .color = {0, 0, 0, 0}});
  materials.push_back(level::Material{.name = "none", .color = {0, 0, 0, 0}});
  seasons.push_back(level::Season{.name = "default"});
  // At least one rule set, as there is at least one season, so a level always has one to resolve to
  rules.emplace_back(arena);
  rules.back().wfc.name = "rules";
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
  // A new project's one rule set is named after it, so its file is <project>.rules.json rather than
  // the bare "rules" a default-constructed Project carries
  project.rules[0].wfc.name = project.name;
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
      // A retired entry of that name is still the entry that name means, so its id comes back and
      // no duplicate is appended. It is deliberately *not* un-retired: interning is what reading a
      // file does, and a file should not quietly undo the project's decision to remove an entry.
      // The catalogs panel lists it as removed, with a button to bring it back.
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
      // As above: the id comes back, the retirement stays
      return static_cast<level::MaterialId>(i);
    }
  }
  project.materials.push_back(level::Material{.name = Name{name}});
  return static_cast<level::MaterialId>(project.materials.size() - 1);
}

void remove_season(Project& project, level::SeasonId id)
{
  LVL_ASSERT_GT(project.seasons.size(), 1UL) << "a project needs at least one season";
  LVL_ASSERT_LT(id, project.seasons.size());
  project.seasons.erase(project.seasons.begin() + static_cast<std::ptrdiff_t>(id));
}

level::SeasonId add_season(Project& project, std::string_view name)
{
  // SeasonId is a byte, and a cycle of 255 seasons is already past anything sensible
  LVL_ASSERT_LT(project.seasons.size(), 255UL) << "a project holds at most 255 seasons";
  project.seasons.push_back(level::Season{.name = Name{name}});
  return static_cast<level::SeasonId>(project.seasons.size() - 1);
}

std::size_t find_rules(const Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.rules.size(); ++i)
  {
    if (project.rules[i].wfc.name == name)
    {
      return i;
    }
  }
  return kNoRules;
}

std::size_t add_rules(Project& project, std::string_view name)
{
  project.rules.emplace_back(*project.arena);
  project.rules.back().wfc.name.assign(name);
  return project.rules.size() - 1;
}

std::size_t intern_rules(Project& project, std::string_view name)
{
  const std::size_t found = find_rules(project, name);
  return (found == kNoRules) ? add_rules(project, name) : found;
}

std::size_t rules_of(const Project& project, const level::Level& level)
{
  LVL_ASSERT_FALSE(project.rules.empty()) << "a project always holds at least one rule set";
  if (level.rules.empty())
  {
    return 0;
  }
  const std::size_t found = find_rules(project, level.rules.view());
  // A level naming a set the project does not have falls back rather than failing: the set may have
  // been removed, and the level is still editable with the project's first
  return (found == kNoRules) ? 0 : found;
}

void remove_rules(Project& project, std::size_t index)
{
  LVL_ASSERT_LT(index, project.rules.size());
  if (project.rules.size() == 1)
  {
    return;
  }
  project.rules.erase(project.rules.begin() + static_cast<std::ptrdiff_t>(index));
  // The file list is parallel to the sets, as the tilesets' is
  if (index < project.rule_files.size())
  {
    project.rule_files.erase(project.rule_files.begin() + static_cast<std::ptrdiff_t>(index));
  }
}

level::TileSetId add_tileset(Project& project, std::string_view name)
{
  LVL_ASSERT_LT(project.tilesets.size(), static_cast<std::size_t>(level::kNoTileSet)) << "too many tilesets";
  project.tilesets.emplace_back(*project.arena);
  project.tilesets.back().name.assign(name);
  return static_cast<level::TileSetId>(project.tilesets.size() - 1);
}

std::size_t find_tileset(const Project& project, std::string_view name)
{
  for (std::size_t i = 0; i < project.tilesets.size(); ++i)
  {
    if (project.tilesets[i].name == name)
    {
      return i;
    }
  }
  return project.tilesets.size();
}

Status remove_tileset(Project& project, level::Level& level, level::TileSetId id)
{
  LVL_ASSERT_LT(id, project.tilesets.size());
  if (project.levels.size() > 1)
  {
    return Status::error(
      "'%s' is used by %zu levels, and only the open one can be renumbered; removing a tileset from a "
      "project of several levels is not supported yet",
      project.tilesets[id].name.c_str(),
      project.levels.size());
  }
  project.tilesets.erase(project.tilesets.begin() + static_cast<std::ptrdiff_t>(id));
  // The file list is parallel to the tilesets, so it has to follow the removal or every entry after
  // this one would name the wrong file
  if (id < project.tileset_files.size())
  {
    project.tileset_files.erase(project.tileset_files.begin() + static_cast<std::ptrdiff_t>(id));
  }
  level::clear_tileset_refs(level, id);
  for (RuleSet& set : project.rules)
  {
    level::clear_tileset_refs(set.wfc, id);
    level::clear_tileset_refs(set.auto_tiles, id);
    level::clear_tileset_refs(set.steps, id);
  }
  return Status::okay();
}

void remove_semantic(Project& project, level::Level& level, level::SemanticId id)
{
  LVL_ASSERT_GT(id, level::kNoSemantic) << "the 'none' semantic type can't be removed";
  LVL_ASSERT_LT(id, project.semantics.size());
  // Retired, not erased: the ids after this one must not move, because the project's other levels
  // are not resident and their cells would keep an id that meant something else
  project.semantics[id].retired = true;
  // Everything the project itself holds is resident, so it is cleared outright
  for (auto& ts : project.tilesets)
  {
    for (auto& t : ts.tiles)
    {
      t.semantic = (t.semantic == id) ? level::kNoSemantic : t.semantic;
    }
  }
  for (RuleSet& set : project.rules)
  {
    for (auto& t : set.wfc.tiles)
    {
      t.semantic = (t.semantic == id) ? level::kNoSemantic : t.semantic;
    }
  }
  // So are the open level's cells. Its siblings' keep the id, which reads as "none" everywhere
  // because the entry is retired, until a compaction renumbers them all together.
  level::for_each_id_cell(
    level, level::LayerKind::kSemantic, [id](std::uint16_t& v) { v = (v == id) ? level::kNoSemantic : v; });
}

void remove_material(Project& project, level::Level& level, level::MaterialId id)
{
  LVL_ASSERT_GT(id, level::kNoMaterial) << "the 'none' material can't be removed";
  LVL_ASSERT_LT(id, project.materials.size());
  project.materials[id].retired = true;
  for (auto& ts : project.tilesets)
  {
    for (auto& t : ts.tiles)
    {
      t.material = (t.material == id) ? level::kNoMaterial : t.material;
    }
    for (auto& v : ts.seasonal)
    {
      v.material = (v.material == id) ? level::kNoMaterial : v.material;
    }
  }
  level::for_each_id_cell(
    level, level::LayerKind::kMaterial, [id](std::uint16_t& v) { v = (v == id) ? level::kNoMaterial : v; });
}

namespace
{

/// Fills <code>plan</code> for one catalog: the new id of each entry, or kRetiredEntry
template <typename T> std::size_t plan_one(const memory::Vector<T>& catalog, memory::Vector<std::uint16_t>& plan)
{
  plan.clear();
  plan.reserve(catalog.size());
  std::size_t retired = 0;
  for (const T& entry : catalog)
  {
    // "none" is never retired, so index 0 always maps to 0
    if (entry.retired)
    {
      plan.push_back(kRetiredEntry);
      ++retired;
    }
    else
    {
      plan.push_back(static_cast<std::uint16_t>(plan.size() - retired));
    }
  }
  return retired;
}

/// The new id of <code>v</code> under <code>plan</code>; 0 for a retired entry or an id off the end
std::uint16_t planned(const memory::Vector<std::uint16_t>& plan, std::uint16_t v)
{
  if (v >= plan.size() or plan[v] == kRetiredEntry)
  {
    return 0;
  }
  return plan[v];
}

/// Drops the entries <code>plan</code> retires, keeping the rest in order
template <typename T> void drop_retired(memory::Vector<T>& catalog, const memory::Vector<std::uint16_t>& plan)
{
  std::size_t out = 0;
  for (std::size_t i = 0; i < catalog.size(); ++i)
  {
    if (i < plan.size() and plan[i] == kRetiredEntry)
    {
      continue;
    }
    catalog[out] = catalog[i];
    ++out;
  }
  catalog.resize(out);
}

}  // namespace

bool needs_compaction(const Project& project)
{
  const auto any = [](const auto& catalog) {
    return std::any_of(catalog.begin(), catalog.end(), [](const auto& e) { return e.retired; });
  };
  return any(project.semantics) or any(project.materials);
}

void plan_compaction(const Project& project, CatalogPlan& out)
{
  out.retired_semantics = plan_one(project.semantics, out.semantics);
  out.retired_materials = plan_one(project.materials, out.materials);
}

void apply_compaction(const CatalogPlan& plan, level::Level& level)
{
  level::for_each_id_cell(
    level, level::LayerKind::kSemantic, [&plan](std::uint16_t& v) { v = planned(plan.semantics, v); });
  level::for_each_id_cell(
    level, level::LayerKind::kMaterial, [&plan](std::uint16_t& v) { v = planned(plan.materials, v); });
}

void finish_compaction(Project& project, const CatalogPlan& plan)
{
  // Everything the project holds, before the vectors the plan is indexed by are rewritten
  for (auto& ts : project.tilesets)
  {
    for (auto& t : ts.tiles)
    {
      t.semantic = planned(plan.semantics, t.semantic);
      t.material = planned(plan.materials, t.material);
    }
    for (auto& v : ts.seasonal)
    {
      v.material = planned(plan.materials, v.material);
    }
  }
  for (RuleSet& set : project.rules)
  {
    for (auto& t : set.wfc.tiles)
    {
      t.semantic = planned(plan.semantics, t.semantic);
    }
  }
  drop_retired(project.semantics, plan.semantics);
  drop_retired(project.materials, plan.materials);
}

std::size_t semantic_uses(const Project& project, const level::Level& level, level::SemanticId id)
{
  LVL_ASSERT_LT(id, project.semantics.size());
  std::size_t n = level::count_id_cells(level, level::LayerKind::kSemantic, [id](std::uint16_t v) { return v == id; });
  for (const auto& ts : project.tilesets)
  {
    n += static_cast<std::size_t>(
      std::count_if(ts.tiles.begin(), ts.tiles.end(), [id](const level::TileInfo& t) { return t.semantic == id; }));
  }
  for (const RuleSet& set : project.rules)
  {
    n += static_cast<std::size_t>(std::count_if(
      set.wfc.tiles.begin(), set.wfc.tiles.end(), [id](const level::WfcTile& t) { return t.semantic == id; }));
  }
  return n;
}

std::size_t material_uses(const Project& project, const level::Level& level, level::MaterialId id)
{
  LVL_ASSERT_LT(id, project.materials.size());
  std::size_t n = level::count_id_cells(level, level::LayerKind::kMaterial, [id](std::uint16_t v) { return v == id; });
  for (const auto& ts : project.tilesets)
  {
    n += static_cast<std::size_t>(
      std::count_if(ts.tiles.begin(), ts.tiles.end(), [id](const level::TileInfo& t) { return t.material == id; }));
    n += static_cast<std::size_t>(std::count_if(
      ts.seasonal.begin(), ts.seasonal.end(), [id](const level::SeasonalVariant& v) { return v.material == id; }));
  }
  return n;
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
