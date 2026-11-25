/**
 * @copyright 2026-present Brian Cairl
 *
 * @file file_dialog.cpp
 *
 * File open/save dialogs, on aiekick/ImGuiFileDialog: a places pane (fixed places, editable
 * bookmarks saved to the user's config, and devices), a clickable path bar that can also be edited
 * as text, search, sortable columns, keyboard exploration, file-type filters and overwrite
 * confirmation for saves.
 *
 * ImGuiFileDialog is a third-party library with its own (virtual, heap-allocating) internals; the
 * editor only calls into it from here.
 */

// C++ Standard Library
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

// Dear ImGui
#include <ImGuiFileDialog.h>
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/editor/config.hpp"
#include "lvl/editor/web.hpp"

namespace lvl::editor
{
namespace
{

constexpr const char* kKey = "lvl-file";
constexpr const char* kPlacesGroup = "Places";
constexpr const char* kPlacesFile = "file_dialog_places.txt";

constexpr const char* kProjectFilters = "Project manifest (project.json){project.json},.json";
constexpr const char* kLevelFilters = "Levels{.json,.lvlb,.bin},JSON{.json},Binary{.lvlb,.bin},All files{.*}";
constexpr const char* kDataFilters = "JSON{.json},Binary{.lvlb,.bin},All files{.*}";
constexpr const char* kImageFilters = "Images{.png,.jpg,.jpeg,.bmp,.tga,.gif,.psd,.hdr,.pic,.pnm},All files{.*}";
constexpr const char* kPngFilters = "PNG{.png}";
constexpr const char* kStyleFilters = "Style profiles{.json},All files{.*}";

ImGuiFileDialog& dialog() { return *ImGuiFileDialog::Instance(); }

/// What each action's dialog looks like
struct Setup
{
  const char* title = "Open";
  const char* filters = "All files{.*}";
  bool save = false;
  /**
   * @brief Which of the project's folders this kind of file belongs in
   *
   * Set for every action that reads or writes part of a project, which is what makes this table say
   * outright where each kind of file lives rather than leaving it to the dialog's last folder.
   * Left unset for a level, a project manifest and a style profile: none of them belongs to a
   * folder of the open project, so those open wherever the dialog was last used.
   */
  std::optional<project::Folder> folder;
  /// Suggested file name (saves)
  FixedString<160> name;
};

/**
 * @brief The current project folder: the folder the open level lives in, or the working directory
 *        while it has no path yet
 */
Path project_directory(const App& app)
{
  std::error_code ec;
  if (!app.path.empty())
  {
    const auto dir = std::filesystem::absolute(std::filesystem::path{app.path.c_str()}, ec).parent_path();
    if (!ec and !dir.empty())
    {
      return Path{dir.string()};
    }
  }
  if constexpr (web::kAvailable)
  {
    return Path{web::kWorkDirectory};
  }
  else if (const auto cwd = std::filesystem::current_path(ec); !ec)
  {
    return Path{cwd.string()};
  }
  return Path{"."};
}

/**
 * @brief Where the project's <code>folder</code> is, for a dialog to open in
 *
 * Inside the project tree once there is one, which is the point of the tree: an atlas opens in
 * `assets/atlases/`, an export in `artifacts/`, a tileset in `tilesets/`. With no project folder
 * yet it falls back to the folder the open level lives in, and then to the working directory --
 * the behaviour before projects, which is still what a bare level file gets.
 */
Path folder_directory(const App& app, project::Folder folder)
{
  if (!app.project_path.empty() and !app.project->root.empty())
  {
    Path out;
    if (project::subfolder(app.project->root.view(), folder, out).ok)
    {
      // Created on demand, since a project's tree is made when it is saved and a folder the user
      // deleted should not make the dialog open nowhere
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path{out.c_str()}, ec);
      if (!ec)
      {
        return out;
      }
    }
  }
  return project_directory(app);
}

Setup setup(const App& app, FileAction action)
{
  const project::Project& pr = *app.project;
  const level::Level& lv = *app.level;
  switch (action)
  {
  case FileAction::kNone:
    break;
  case FileAction::kNewProject: {
    // A project is where its manifest is, so putting project.json somewhere is what makes a project
    // there. Picking the file rather than the folder means no directory-picking mode is needed, and
    // it matches what save_project does with the path it is given.
    //
    // The suggested name carries a folder, because the tree is created beside the manifest: accept
    // it and the project lands in a folder of its own, named after the level. Accepting a bare
    // "project.json" would scatter levels/, tilesets/, assets/ and the rest directly into whatever
    // folder the dialog happened to be in, which is recoverable only by hand. The folder is still
    // only a suggestion -- it can be renamed or deleted in the name box like any other.
    Path suggested;
    const std::string_view name = lv.settings.name.empty() ? std::string_view{"project"} : lv.settings.name.view();
    if (!project::project_file(project::Folder::kRoot, name, "", suggested).ok)
    {
      suggested.assign("project");
    }
    return Setup{
      .title = "New project (choose where its folder goes)",
      .filters = kProjectFilters,
      .save = true,
      .name = format<160>(
        "%s/%.*s", suggested.c_str(), static_cast<int>(project::kManifestName.size()), project::kManifestName.data())};
  }
  case FileAction::kOpenProject:
    return Setup{.title = "Open project", .filters = kProjectFilters};
  case FileAction::kSaveProjectAs:
    return Setup{
      .title = "Save project as",
      .filters = kProjectFilters,
      .save = true,
      .folder = project::Folder::kRoot,
      .name = "project.json"};
  case FileAction::kOpenLevel:
    return Setup{.title = "Open level", .filters = kLevelFilters};
  case FileAction::kAddLevel:
    return Setup{.title = "Add a level to this project", .filters = kLevelFilters, .folder = project::Folder::kLevels};
  case FileAction::kSaveLevel:
    return Setup{
      .title = "Save level",
      .filters = kLevelFilters,
      .save = true,
      .name = app.path.empty() ? FixedString<160>{format<160>("%s.json", lv.settings.name.c_str())}
                               : FixedString<160>{std::filesystem::path{app.path.c_str()}.filename().string()}};
  case FileAction::kLoadTileSet:
    return Setup{.title = "Import tileset", .filters = kDataFilters, .folder = project::Folder::kTileSets};
  case FileAction::kSaveTileSet:
    return Setup{
      .title = "Export tileset",
      .filters = kDataFilters,
      .save = true,
      .folder = project::Folder::kTileSets,
      .name = format<160>(
        "%s.tileset.json", app.tileset < pr.tilesets.size() ? pr.tilesets[app.tileset].name.c_str() : "tileset")};
  case FileAction::kAtlasImage:
    return Setup{.title = "Choose atlas image", .filters = kImageFilters, .folder = project::Folder::kAtlases};
  case FileAction::kSeedImage:
    return Setup{.title = "Choose seed image", .filters = kImageFilters, .folder = project::Folder::kImages};
  case FileAction::kLoadWfc:
    return Setup{.title = "Import WFC rules", .filters = kDataFilters, .folder = project::Folder::kRules};
  case FileAction::kSaveWfc:
    return Setup{
      .title = "Export WFC rules",
      .filters = kDataFilters,
      .save = true,
      .folder = project::Folder::kRules,
      .name = "rules.wfc.json"};
  case FileAction::kLoadPaths:
    return Setup{.title = "Import path graphs", .filters = kDataFilters, .folder = project::Folder::kPaths};
  case FileAction::kSavePaths:
    return Setup{
      .title = "Export path graphs",
      .filters = kDataFilters,
      .save = true,
      .folder = project::Folder::kPaths,
      .name = "paths.json"};
  case FileAction::kExportPng:
    return Setup{
      .title = "Export map as PNG",
      .filters = kPngFilters,
      .save = true,
      .folder = project::Folder::kArtifacts,
      .name = format<160>("%s.png", lv.settings.name.c_str())};
  case FileAction::kExportMinimap:
    return Setup{
      .title = "Export mini-preview as PNG",
      .filters = kPngFilters,
      .save = true,
      .folder = project::Folder::kArtifacts,
      .name = format<160>("%s.preview.png", lv.settings.name.c_str())};
  case FileAction::kExportLayerImage:
    return Setup{
      .title = "Export layer as PNG",
      .filters = kPngFilters,
      .save = true,
      .folder = project::Folder::kArtifacts,
      // <level>.<layer>.png, so a project's artifacts folder says which level and layer each is
      .name = format<160>(
        "%s.%s.png",
        lv.settings.name.c_str(),
        app.layer < lv.layers.size() ? lv.layers[app.layer].name.c_str() : "layer")};
  case FileAction::kSaveStyle:
    return Setup{.title = "Save style profile", .filters = kStyleFilters, .save = true, .name = "style.json"};
  case FileAction::kLoadStyle:
    return Setup{.title = "Load style profile", .filters = kStyleFilters};
  }
  return Setup{};
}

/// The fixed places (refreshed on every open: the level's folder follows the open level)
void refresh_places(const App& app)
{
  dialog().RemovePlacesGroup(kPlacesGroup);
  dialog().AddPlacesGroup(kPlacesGroup, 0, false, true);
  auto* group = dialog().GetPlacesGroupPtr(kPlacesGroup);
  std::error_code ec;
  if constexpr (web::kAvailable)
  {
    group->AddPlace("Work (in browser)", web::kWorkDirectory, false);
  }
  else if (const char* home = std::getenv("HOME"); home != nullptr and home[0] != '\0')
  {
    group->AddPlace("Home", home, false);
  }
  if (const auto cwd = std::filesystem::current_path(ec); !ec)
  {
    group->AddPlace("Working directory", cwd.string(), false);
  }
  if (!app.path.empty())
  {
    const auto dir = std::filesystem::absolute(std::filesystem::path{app.path.c_str()}, ec).parent_path();
    if (!ec)
    {
      group->AddPlace("Level folder", dir.string(), false);
    }
  }
  if (const auto config = std::filesystem::path{config_path("").c_str()}.parent_path(); !config.empty())
  {
    group->AddPlace("lvl settings", config.string(), false);
  }
}

/// Loads the user's bookmarks (once) into ImGuiFileDialog's built-in, editable "Bookmarks" group
void load_places(App& app)
{
  if (app.dialog.places_loaded)
  {
    return;
  }
  app.dialog.places_loaded = true;
  const Path path = config_path(kPlacesFile);
  if (!file::exists(path.c_str()))
  {
    return;
  }
  memory::Scope scope{app.memory.frame};
  char* data = nullptr;
  std::size_t size = 0;
  if (const Status s = file::read(path.c_str(), app.memory.frame, data, size); !s.ok)
  {
    LVL_LOG_WARN() << "Ignoring file dialog bookmarks: " << s.message;
    return;
  }
  dialog().DeserializePlaces(std::string{data, size});
}

}  // namespace

void open_file_dialog(App& app, FileAction action)
{
  load_places(app);
  refresh_places(app);
  const Setup s = setup(app, action);
  app.dialog.action = action;
  // Each kind of file opens where it belongs in the project tree (see Setup::folder); a level, a
  // manifest or a style profile belongs to no folder of it, and opens where the dialog was last used
  const Path directory = s.folder.has_value() ? folder_directory(app, *s.folder) : app.dialog.directory;
  IGFD::FileDialogConfig config;
  config.path = directory.c_str();
  config.fileName = s.name.c_str();
  config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering |
    ImGuiFileDialogFlags_ShowDevicesButton | ImGuiFileDialogFlags_NaturalSorting |
    (s.save ? ImGuiFileDialogFlags_ConfirmOverwrite : ImGuiFileDialogFlags_None);
  dialog().OpenDialog(kKey, s.title, s.filters, config);
}

void file_dialog(App& app)
{
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const ImVec2 min_size{std::min(760.0F, display.x * 0.9F), std::min(460.0F, display.y * 0.8F)};
  if (!dialog().Display(kKey, ImGuiWindowFlags_NoCollapse, min_size, ImVec2{display.x * 0.95F, display.y * 0.95F}))
  {
    return;
  }
  const FileAction action = app.dialog.action;
  app.dialog.action = FileAction::kNone;
  if (dialog().IsOk())
  {
    const std::string path = dialog().GetFilePathName(IGFD_ResultMode_AddIfNoFileExt);
    // An import or export elsewhere does not move the folder the other dialogs remember
    if (!setup(app, action).folder.has_value())
    {
      app.dialog.directory.assign(dialog().GetCurrentPath());
    }
    dialog().Close();
    save_file_dialog_places(app);
    file_action(app, action, path.c_str());
    return;
  }
  dialog().Close();
  // Cancelling the save dialog opened by "Save as... and quit" cancels quitting
  app.exit_after_save = false;
}

bool file_dialog_open() { return dialog().IsOpened(kKey); }

void close_file_dialog(App& app)
{
  dialog().Close();
  app.dialog.action = FileAction::kNone;
}

void save_file_dialog_places(App& app)
{
  if (!app.dialog.places_loaded)
  {
    return;
  }
  // Only the editable groups (bookmarks): the fixed places are rebuilt every time
  const std::string places = dialog().SerializePlaces(false);
  if (const Status s = write_config(kPlacesFile, places.data(), places.size()); !s.ok)
  {
    LVL_LOG_WARN() << "Could not save file dialog bookmarks: " << s.message;
  }
}

}  // namespace lvl::editor
