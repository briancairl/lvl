/**
 * @copyright 2026-present Brian Cairl
 *
 * @file recent.cpp
 *
 * The recently opened levels (File > Open recent): a fixed, newest-first list of paths, kept in the
 * user's config as one path per line.
 */

// C++ Standard Library
#include <algorithm>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/editor/config.hpp"
#include "lvl/project/paths.hpp"

namespace lvl::editor
{
namespace
{

constexpr const char* kRecentFile = "recent_files.txt";

/// Where <code>path</code> sits in the list, or <code>count</code> if it is not in it
std::size_t find_recent(const RecentFiles& recent, std::string_view path)
{
  for (std::size_t i = 0; i < recent.count; ++i)
  {
    if (recent.paths[i] == path)
    {
      return i;
    }
  }
  return recent.count;
}

/// The file name in <code>path</code> (what the menu shows)
std::string_view file_name(std::string_view path)
{
  const std::size_t slash = path.find_last_of("/\\");
  return (slash == std::string_view::npos) ? path : path.substr(slash + 1);
}

/// Writes the list to the user's config
void save_recent_files(App& app)
{
  memory::Scope scope{app.memory.frame};
  auto text = memory::make_vector<char>(app.memory.frame);
  text.reserve(app.recent.count * (Path::capacity() + 1));
  for (std::size_t i = 0; i < app.recent.count; ++i)
  {
    const std::string_view path = app.recent.paths[i].view();
    text.insert(text.end(), path.begin(), path.end());
    text.push_back('\n');
  }
  if (const Status s = write_config(kRecentFile, text.data(), text.size()); !s.ok)
  {
    LVL_LOG_WARN() << "Could not save the recent levels: " << s.message;
  }
}

}  // namespace

void load_recent_files(App& app)
{
  if (app.recent.loaded)
  {
    return;
  }
  app.recent.loaded = true;
  const Path path = config_path(kRecentFile);
  if (!file::exists(path.c_str()))
  {
    return;
  }
  memory::Scope scope{app.memory.frame};
  char* data = nullptr;
  std::size_t size = 0;
  if (const Status s = file::read(path.c_str(), app.memory.frame, data, size); !s.ok)
  {
    LVL_LOG_WARN() << "Ignoring the recent levels: " << s.message;
    return;
  }
  const std::string_view text{data, size};
  std::size_t at = 0;
  while (at < text.size() and app.recent.count < RecentFiles::kCapacity)
  {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = text.substr(at, end - at);
    at = end + 1;
    // Blank, over-long and repeated lines are dropped rather than refusing the whole file
    if (line.empty() or line.size() >= Path::capacity() or find_recent(app.recent, line) < app.recent.count)
    {
      continue;
    }
    app.recent.paths[app.recent.count++].assign(line);
  }
}

void remember_recent_file(App& app, std::string_view path)
{
  if (path.empty() or path.size() >= Path::capacity())
  {
    return;
  }
  load_recent_files(app);
  RecentFiles& recent = app.recent;
  // A path already listed moves back to the front; otherwise the oldest entry falls off a full list
  const std::size_t found = find_recent(recent, path);
  std::size_t from = found;
  if (found == recent.count)
  {
    from = std::min(recent.count, RecentFiles::kCapacity - 1);
    recent.count = std::min(recent.count + 1, RecentFiles::kCapacity);
  }
  for (std::size_t i = from; i > 0; --i)
  {
    recent.paths[i] = recent.paths[i - 1];
  }
  recent.paths[0].assign(path);
  save_recent_files(app);
}

void forget_recent_file(App& app, std::string_view path)
{
  const std::size_t found = find_recent(app.recent, path);
  if (found == app.recent.count)
  {
    return;
  }
  for (std::size_t i = found + 1; i < app.recent.count; ++i)
  {
    app.recent.paths[i - 1] = app.recent.paths[i];
  }
  app.recent.paths[--app.recent.count] = Path{};
  save_recent_files(app);
}

void clear_recent_files(App& app)
{
  app.recent.loaded = true;
  app.recent.count = 0;
  std::fill(std::begin(app.recent.paths), std::end(app.recent.paths), Path{});
  save_recent_files(app);
}

void recent_files_menu(App& app)
{
  load_recent_files(app);
  if (!ImGui::BeginMenu("Open recent", app.recent.count > 0))
  {
    return;
  }
  // Choosing one replaces the document, as "Open..." does. A file that has gone away is disabled
  // and says so in its tooltip; it leaves the list once a load of it fails
  Path chosen;
  for (std::size_t i = 0; i < app.recent.count; ++i)
  {
    const Path& path = app.recent.paths[i];
    // A project manifest is listed by its folder, since "project.json" is the same for all of them
    const bool is_project = file_name(path.view()) == project::kManifestName;
    const std::string_view name = is_project ? file_name(project::parent_path(path.view())) : file_name(path.view());
    const bool present = file::exists(path.c_str());
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::MenuItem(
          format<Path::capacity()>(is_project ? "%.*s (project)" : "%.*s", static_cast<int>(name.size()), name.data()),
          nullptr,
          false,
          present))
    {
      chosen = path;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
      ImGui::SetTooltip(present ? "%s" : "%s (missing)", path.c_str());
    }
    ImGui::PopID();
  }
  ImGui::Separator();
  const bool clear = ImGui::MenuItem("Clear the list");
  ImGui::EndMenu();
  // Acted on after the menu is closed out, as either one changes the list the loop reads
  if (clear)
  {
    clear_recent_files(app);
  }
  else if (!chosen.empty())
  {
    // A project is reopened as a project, which goes on to load its first member
    const bool is_project = file_name(chosen.view()) == project::kManifestName;
    file_action(app, is_project ? FileAction::kOpenProject : FileAction::kOpenLevel, chosen.c_str());
  }
}

}  // namespace lvl::editor
