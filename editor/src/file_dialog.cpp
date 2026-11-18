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
  /// Suggested file name (saves)
  FixedString<160> name;
};

Setup setup(const App& app, FileAction action)
{
  const level::Level& lv = *app.level;
  switch (action)
  {
  case FileAction::kNone:
    break;
  case FileAction::kOpenLevel:
    return Setup{.title = "Open level", .filters = kLevelFilters};
  case FileAction::kSaveLevel:
    return Setup{
      .title = "Save level",
      .filters = kLevelFilters,
      .save = true,
      .name = app.path.empty() ? FixedString<160>{format<160>("%s.json", lv.settings.name.c_str())}
                               : FixedString<160>{std::filesystem::path{app.path.c_str()}.filename().string()}};
  case FileAction::kLoadTileSet:
    return Setup{.title = "Import tileset", .filters = kDataFilters};
  case FileAction::kSaveTileSet:
    return Setup{
      .title = "Export tileset",
      .filters = kDataFilters,
      .save = true,
      .name = format<160>(
        "%s.tileset.json", app.tileset < lv.tilesets.size() ? lv.tilesets[app.tileset].name.c_str() : "tileset")};
  case FileAction::kAtlasImage:
    return Setup{.title = "Choose atlas image", .filters = kImageFilters};
  case FileAction::kSeedImage:
    return Setup{.title = "Choose seed image", .filters = kImageFilters};
  case FileAction::kLoadWfc:
    return Setup{.title = "Import WFC rules", .filters = kDataFilters};
  case FileAction::kSaveWfc:
    return Setup{.title = "Export WFC rules", .filters = kDataFilters, .save = true, .name = "rules.wfc.json"};
  case FileAction::kLoadPaths:
    return Setup{.title = "Import path graphs", .filters = kDataFilters};
  case FileAction::kSavePaths:
    return Setup{.title = "Export path graphs", .filters = kDataFilters, .save = true, .name = "paths.json"};
  case FileAction::kExportPng:
    return Setup{
      .title = "Export map as PNG",
      .filters = kPngFilters,
      .save = true,
      .name = format<160>("%s.png", lv.settings.name.c_str())};
  case FileAction::kExportMinimap:
    return Setup{
      .title = "Export mini-preview as PNG",
      .filters = kPngFilters,
      .save = true,
      .name = format<160>("%s.preview.png", lv.settings.name.c_str())};
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
  IGFD::FileDialogConfig config;
  config.path = app.dialog.directory.c_str();
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
    app.dialog.directory.assign(dialog().GetCurrentPath());
    dialog().Close();
    save_file_dialog_places(app);
    file_action(app, action, path.c_str());
    return;
  }
  dialog().Close();
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
  const Path path = config_path(kPlacesFile);
  std::error_code ec;
  if (const auto dir = std::filesystem::path{path.c_str()}.parent_path(); !dir.empty())
  {
    std::filesystem::create_directories(dir, ec);
  }
  if (const Status s = file::write(path.c_str(), places.data(), places.size()); !s.ok)
  {
    LVL_LOG_WARN() << "Could not save file dialog bookmarks: " << s.message;
  }
}

}  // namespace lvl::editor
