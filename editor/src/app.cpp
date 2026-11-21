/**
 * @copyright 2026-present Brian Cairl
 *
 * @file app.cpp
 *
 * Editor lifecycle, main menu, shortcuts, background-job handling and dialogs.
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

// Dear ImGui
#include <imgui.h>
#include <imgui_internal.h>

// LVL
#include "internal.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/random.hpp"
#include "lvl/core/visitor.hpp"
#include "lvl/editor/app.hpp"
#include "lvl/editor/web.hpp"
#include "lvl/level/io.hpp"
#include "lvl/procgen/wfc.hpp"

namespace lvl::editor
{
namespace
{

/// Fractions of the total memory given to each arena
void reserve_memory(App& app)
{
  Memory& m = app.memory;
  const std::size_t total = app.config.total;
  memory::create(m.root, total, "editor");
  memory::push(m.root, m.documents[0], total / 100 * 18, "document-a");
  memory::push(m.root, m.documents[1], total / 100 * 18, "document-b");
  memory::push(m.root, m.assets[0], total / 100 * 7, "assets-a");
  memory::push(m.root, m.assets[1], total / 100 * 7, "assets-b");
  memory::push(m.root, m.undo, total / 100 * 8, "undo");
  memory::push(m.root, m.frame, total / 100 * 3, "frame");
  memory::push(m.root, m.json, total / 100 * 2, "json");
  memory::push(m.root, m.jobs, "jobs");
  LVL_LOG_INFO() << "Reserved " << memory::Bytes{total} << " for the editor";
}

/// Uploads freshly decoded atlases (now in asset arena <code>index</code>) and computes tile colors
void apply_atlases(App& app, AtlasImages& atlases, std::size_t index)
{
  app.assets.current = index;
  memory::Arena& arena = app.memory.assets[index];
  for (std::size_t i = 0; i < kMaxTileSets; ++i)
  {
    const bool present = i < atlases.count and i < app.level->tilesets.size();
    app.assets.images[i] = present ? atlases.images[i] : image::Image{};
    app.assets.status[i] = present ? atlases.status[i] : Status::okay();
    app.assets.tile_colors[i] = memory::make_vector<image::Rgba>(arena);
    if (present and !atlases.images[i].empty())
    {
      upload(app.assets.textures[i], atlases.images[i]);
      level::TileSet& ts = app.level->tilesets[i];
      // The atlas defines the slicing grid
      if (ts.image_width != atlases.images[i].width or ts.image_height != atlases.images[i].height)
      {
        ts.image_width = atlases.images[i].width;
        ts.image_height = atlases.images[i].height;
        level::reslice(ts);
      }
      render::average_tile_colors(ts, atlases.images[i], app.assets.tile_colors[i]);
    }
    else
    {
      release(app.assets.textures[i]);
    }
  }
  invalidate_view(app);
}

void update_title(App& app)
{
  set_title(format<600>("lvl - %s%s", app.path.empty() ? "untitled" : app.path.c_str(), app.modified ? " *" : ""));
}

/// Applies a finished job's results on the UI thread
void finish_job(App& app)
{
  std::visit(
    overloaded{
      [](std::monostate&) {},
      [&app](jobs::Background<SaveLevelJob>& bg) {
        auto& job = bg.job();
        if (job.result.ok)
        {
          app.path = job.path;
          app.modified = false;
          web::offer_download(job.path.c_str());
          // Quitting was waiting on this save
          app.exit = app.exit or app.exit_after_save;
        }
        app.exit_after_save = false;
        report(app, job.result);
      },
      [&app](jobs::Background<LoadLevelJob>& bg) {
        auto& job = bg.job();
        if (job.result.ok)
        {
          std::swap(app.level, app.spare);
          level::reset(*app.spare);
          apply_atlases(app, job.atlases, 1 - app.assets.current);
          app.path = job.path;
          app.modified = false;
          app.undo.clear();
          app.layer = std::min<std::size_t>(app.layer, app.level->layers.empty() ? 0 : app.level->layers.size() - 1);
          app.tileset = 0;
          app.entity = level::kNullEntity;
          app.view.fit = true;
          app.view.season = 0;
        }
        else
        {
          level::reset(*app.spare);
          report(app, job.result);
        }
      },
      [&app](jobs::Background<ReloadAtlasesJob>& bg) { apply_atlases(app, bg.job().atlases, 1 - app.assets.current); },
      [&app](jobs::Background<ExportJob>& bg) {
        if (bg.job().result.ok)
        {
          LVL_LOG_INFO() << "Exported '" << bg.job().path << "'";
          web::offer_download(bg.job().path.c_str());
        }
        report(app, bg.job().result);
      },
      [&app](jobs::Background<HeightmapJob>& bg) {
        auto& job = bg.job();
        const std::size_t layer = app.generators.height_layer;
        if (!job.result.ok or bg.progress().cancelled())
        {
          report(app, job.result);
          return;
        }
        if (
          layer >= app.level->layers.size() or app.level->layers[layer].kind != level::LayerKind::kHeight or
          level::extents(app.level->layers[layer]) != job.height.extents)
        {
          report(app, Status::error("the height layer changed while generating"));
          return;
        }
        app.undo.snapshot(*app.level, layer, "Generate heightmap");
        auto& plane = level::plane<level::ScalarGrid>(app.level->layers[layer]);
        std::copy(job.height.values.begin(), job.height.values.end(), plane.values.begin());
        level::fit_range(app.level->layers[layer]);
        app.modified = true;
        invalidate_layer(app, layer);
      },
      [&app](jobs::Background<WatershedJob>& bg) {
        auto& job = bg.job();
        if (bg.progress().cancelled())
        {
          return;
        }
        level::Level& lv = *app.level;
        const std::size_t height = app.generators.height_layer;
        const std::size_t water = level::find_layer(lv, level::LayerKind::kWater);
        const std::size_t semantic = level::find_layer(lv, level::LayerKind::kSemantic);
        app.undo.begin_group();
        app.undo.snapshot(lv, height, "Water-shed");
        app.undo.snapshot(lv, water, "Water-shed");
        if (job.semantics and semantic < lv.layers.size())
        {
          app.undo.snapshot(lv, semantic, "Water-shed");
          auto& s = level::plane<level::IdGrid>(lv.layers[semantic]);
          std::copy(job.semantic.values.begin(), job.semantic.values.end(), s.values.begin());
          invalidate_layer(app, semantic);
        }
        app.undo.end_group();
        auto& h = level::plane<level::ScalarGrid>(lv.layers[height]);
        std::copy(job.height.values.begin(), job.height.values.end(), h.values.begin());
        auto& w = level::plane<level::ScalarGrid>(lv.layers[water]);
        std::copy(job.water.values.begin(), job.water.values.end(), w.values.begin());
        level::fit_range(lv.layers[water]);
        lv.layers[water].range_min = 0.0F;
        invalidate_layer(app, height);
        invalidate_layer(app, water);
        app.modified = true;
        LVL_LOG_INFO_FMT(
          "Water-shed: %zu river, %zu lake, %zu tunnel cells",
          job.summary.river_cells,
          job.summary.lake_cells,
          job.summary.tunnel_cells);
      },
      [&app](jobs::Background<WfcJob>& bg) {
        auto& job = bg.job();
        if (!job.result.status.ok)
        {
          report(app, job.result.status);
          return;
        }
        if (job.layer >= app.level->layers.size() or app.level->layers[job.layer].kind != level::LayerKind::kTiles)
        {
          report(app, Status::error("the target layer changed while generating"));
          return;
        }
        app.undo.snapshot(*app.level, job.layer, "Wave function collapse");
        auto& tiles = level::plane<level::TileGrid>(app.level->layers[job.layer]);
        std::copy(job.tiles.values.begin(), job.tiles.values.end(), tiles.values.begin());
        invalidate_layer(app, job.layer);
        app.modified = true;
        LVL_LOG_INFO_FMT(
          "WFC: solved in %d attempt(s), %zu contradiction(s)", job.result.attempts, job.result.contradictions);
      },
    },
    app.job);
  app.job.emplace<std::monostate>();
  memory::clear(app.memory.jobs);
  update_title(app);
}

/// Polls the running job; shows its progress dialog
void job_progress(App& app)
{
  jobs::Progress* progress = std::visit(
    overloaded{
      [](std::monostate&) -> jobs::Progress* { return nullptr; },
      [](auto& bg) -> jobs::Progress* { return &bg.progress(); },
    },
    app.job);
  if (progress == nullptr)
  {
    return;
  }
  const bool done = std::visit(
    overloaded{
      [](std::monostate&) { return true; },
      [](auto& bg) { return bg.done(); },
    },
    app.job);
  if (done)
  {
    if (ImGui::IsPopupOpen("Working"))
    {
      ImGui::OpenPopup("Working");
      if (ImGui::BeginPopupModal("Working", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
      {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }
    finish_job(app);
    return;
  }
  ImGui::OpenPopup("Working");
  ImGui::SetNextWindowSize(ImVec2{420, 0});
  if (ImGui::BeginPopupModal("Working", nullptr, ImGuiWindowFlags_NoResize))
  {
    const auto stage = progress->stage();
    ImGui::TextUnformatted(stage.empty() ? "Working..." : stage.c_str());
    ImGui::ProgressBar(progress->fraction(), ImVec2{-1, 0});
    ImGui::TextDisabled("%zu / %zu", progress->completed(), progress->total());
    if (ImGui::Button(progress->cancelled() ? "Cancelling..." : "Cancel"))
    {
      progress->cancel();
    }
    ImGui::EndPopup();
  }
}

/// "Save changes before quitting?"
void exit_prompt(App& app)
{
  if (app.confirm_exit and !ImGui::IsPopupOpen("Unsaved changes"))
  {
    ImGui::OpenPopup("Unsaved changes");
  }
  if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    return;
  }
  ImGui::Text("'%s' has unsaved changes.", app.path.empty() ? app.level->settings.name.c_str() : app.path.c_str());
  ImGui::TextUnformatted("Save them before quitting?");
  ImGui::Spacing();
  if (ImGui::Button(app.path.empty() ? "Save as... and quit" : "Save and quit") or ImGui::IsKeyPressed(ImGuiKey_Enter))
  {
    ImGui::CloseCurrentPopup();
    save_and_exit(app);
  }
  ImGui::SameLine();
  if (ImGui::Button("Quit without saving"))
  {
    ImGui::CloseCurrentPopup();
    app.confirm_exit = false;
    app.exit = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") or ImGui::IsKeyPressed(ImGuiKey_Escape) or !app.confirm_exit)
  {
    ImGui::CloseCurrentPopup();
    app.confirm_exit = false;
  }
  ImGui::EndPopup();
}

void error_popup(App& app)
{
  if (app.show_error)
  {
    ImGui::OpenPopup("Error");
    app.show_error = false;
  }
  if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::PushTextWrapPos(520.0F);
    ImGui::TextUnformatted(app.error.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("OK") or ImGui::IsKeyPressed(ImGuiKey_Enter))
    {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void undo(App& app)
{
  if (app.undo.undo(*app.level))
  {
    invalidate_view(app);
    app.modified = true;
  }
}

void redo(App& app)
{
  if (app.undo.redo(*app.level))
  {
    invalidate_view(app);
    app.modified = true;
  }
}

void save(App& app)
{
  if (app.path.empty())
  {
    open_file_dialog(app, FileAction::kSaveLevel);
  }
  else
  {
    file_action(app, FileAction::kSaveLevel, app.path.c_str());
  }
}

void shortcuts(App& app)
{
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput or !std::holds_alternative<std::monostate>(app.job))
  {
    return;
  }
  const bool ctrl = io.KeyCtrl;
  if (ctrl and ImGui::IsKeyPressed(ImGuiKey_Z))
  {
    io.KeyShift ? redo(app) : undo(app);
  }
  else if (ctrl and ImGui::IsKeyPressed(ImGuiKey_Y))
  {
    redo(app);
  }
  else if (ctrl and ImGui::IsKeyPressed(ImGuiKey_S))
  {
    io.KeyShift ? open_file_dialog(app, FileAction::kSaveLevel) : save(app);
  }
  else if (ctrl and ImGui::IsKeyPressed(ImGuiKey_O))
  {
    open_file_dialog(app, FileAction::kOpenLevel);
  }
  if (ctrl or io.WantCaptureKeyboard)
  {
    return;
  }
  struct Hotkey
  {
    ImGuiKey key;
    Tool tool;
  };
  static constexpr Hotkey kHotkeys[] = {
    {ImGuiKey_H, Tool::kPan},
    {ImGuiKey_B, Tool::kPaint},
    {ImGuiKey_E, Tool::kErase},
    {ImGuiKey_G, Tool::kFill},
    {ImGuiKey_R, Tool::kRect},
    {ImGuiKey_I, Tool::kPick},
    {ImGuiKey_V, Tool::kSelect},
    {ImGuiKey_P, Tool::kPortal},
    {ImGuiKey_M, Tool::kMarker},
    {ImGuiKey_N, Tool::kPathNode},
    {ImGuiKey_K, Tool::kPathEdge},
  };
  for (const auto& h : kHotkeys)
  {
    if (ImGui::IsKeyPressed(h.key, false))
    {
      app.tool = h.tool;
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
  {
    app.brush.radius = std::max(0, app.brush.radius - 1);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
  {
    app.brush.radius = std::min(64, app.brush.radius + 1);
  }
}

void new_level_popup(App& app, bool open)
{
  static level::Settings settings;
  if (open)
  {
    settings = level::Settings{};
    ImGui::OpenPopup("New level");
  }
  if (ImGui::BeginPopupModal("New level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    input_text("Name", settings.name);
    ImGui::InputInt2("Size (cells)", &settings.extents.width);
    ImGui::InputInt2("Cell size (px)", &settings.cell_width);
    int seed = static_cast<int>(settings.seed);
    if (ImGui::InputInt("Seed", &seed))
    {
      settings.seed = static_cast<std::uint64_t>(std::max(seed, 0));
    }
    settings.extents.width = std::clamp(settings.extents.width, 1, 4096);
    settings.extents.height = std::clamp(settings.extents.height, 1, 4096);
    settings.cell_width = std::clamp(settings.cell_width, 1, 512);
    settings.cell_height = std::clamp(settings.cell_height, 1, 512);
    if (ImGui::Button("Create"))
    {
      new_level(app, settings);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void resize_popup(App& app, bool open)
{
  static level::Extents extents;
  if (open)
  {
    extents = app.level->settings.extents;
    ImGui::OpenPopup("Resize level");
  }
  if (ImGui::BeginPopupModal("Resize level", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::InputInt2("Size (cells)", &extents.width);
    extents.width = std::clamp(extents.width, 1, 4096);
    extents.height = std::clamp(extents.height, 1, 4096);
    ImGui::TextDisabled("Cells are kept from the top-left corner. Clears undo history.");
    if (ImGui::Button("Resize"))
    {
      level::resize(*app.level, extents);
      app.undo.clear();
      app.modified = true;
      invalidate_view(app);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void main_menu(App& app)
{
  bool open_new = false;
  bool open_resize = false;
  if (ImGui::BeginMainMenuBar())
  {
    if (ImGui::BeginMenu("File"))
    {
      open_new = ImGui::MenuItem("New...", "Ctrl+N");
      if (ImGui::MenuItem("New demo level"))
      {
        demo_level(app);
      }
      if (ImGui::MenuItem("Open...", "Ctrl+O"))
      {
        open_file_dialog(app, FileAction::kOpenLevel);
      }
      if (ImGui::MenuItem("Save", "Ctrl+S"))
      {
        save(app);
      }
      if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S"))
      {
        open_file_dialog(app, FileAction::kSaveLevel);
      }
      ImGui::Separator();
      if (ImGui::BeginMenu("Import"))
      {
        if (ImGui::MenuItem("Tileset..."))
        {
          open_file_dialog(app, FileAction::kLoadTileSet);
        }
        if (ImGui::MenuItem("WFC rules..."))
        {
          open_file_dialog(app, FileAction::kLoadWfc);
        }
        if (ImGui::MenuItem("Path graphs..."))
        {
          open_file_dialog(app, FileAction::kLoadPaths);
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Export"))
      {
        if (ImGui::MenuItem("Tileset...", nullptr, false, app.tileset < app.level->tilesets.size()))
        {
          open_file_dialog(app, FileAction::kSaveTileSet);
        }
        if (ImGui::MenuItem("WFC rules..."))
        {
          open_file_dialog(app, FileAction::kSaveWfc);
        }
        if (ImGui::MenuItem("Path graphs..."))
        {
          open_file_dialog(app, FileAction::kSavePaths);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Map as PNG..."))
        {
          open_file_dialog(app, FileAction::kExportPng);
        }
        if (ImGui::MenuItem("Mini-preview as PNG..."))
        {
          open_file_dialog(app, FileAction::kExportMinimap);
        }
        ImGui::EndMenu();
      }
      if constexpr (web::kAvailable)
      {
        ImGui::Separator();
        if (ImGui::MenuItem("Upload files..."))
        {
          web::request_upload(app.dialog.directory.c_str());
        }
      }
      else
      {
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
        {
          request_exit(app);
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
      if (ImGui::MenuItem(format<96>("Undo %s", app.undo.undo_label()), "Ctrl+Z", false, app.undo.can_undo()))
      {
        undo(app);
      }
      if (ImGui::MenuItem(format<96>("Redo %s", app.undo.redo_label()), "Ctrl+Y", false, app.undo.can_redo()))
      {
        redo(app);
      }
      ImGui::Separator();
      open_resize = ImGui::MenuItem("Resize level...");
      if (ImGui::MenuItem("Derive collision from tiles"))
      {
        std::size_t c = level::find_layer(*app.level, level::LayerKind::kCollision);
        if (c == app.level->layers.size())
        {
          c = level::add_layer(*app.level, level::LayerKind::kCollision, "collision");
        }
        app.undo.snapshot(*app.level, c, "Derive collision");
        level::derive_collision(*app.level, c, view_season(app));
        invalidate_layer(app, c);
        app.modified = true;
      }
      if (ImGui::MenuItem("Derive materials from tiles"))
      {
        std::size_t m = level::find_layer(*app.level, level::LayerKind::kMaterial);
        if (m == app.level->layers.size())
        {
          m = level::add_layer(*app.level, level::LayerKind::kMaterial, "materials");
        }
        app.undo.snapshot(*app.level, m, "Derive materials");
        level::derive_materials(*app.level, m, view_season(app));
        invalidate_layer(app, m);
        app.modified = true;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
      ImGui::MenuItem("Grid", nullptr, &app.view.grid);
      ImGui::MenuItem("Objects", nullptr, &app.view.objects);
      ImGui::MenuItem("Season tint", nullptr, &app.view.tint);
      if (ImGui::MenuItem("Fit map", "F"))
      {
        app.view.fit = true;
      }
      ImGui::Separator();
      ImGui::MenuItem("Layers", nullptr, &app.panels.layers);
      ImGui::MenuItem("Tilesets", nullptr, &app.panels.tilesets);
      ImGui::MenuItem("Tile metadata", nullptr, &app.panels.tile_meta);
      ImGui::MenuItem("Catalogs & seasons", nullptr, &app.panels.catalogs);
      ImGui::MenuItem("Generate", nullptr, &app.panels.generate);
      ImGui::MenuItem("WFC neighbors", nullptr, &app.panels.wfc_neighbors);
      ImGui::MenuItem("Objects & paths", nullptr, &app.panels.objects);
      ImGui::MenuItem("Mini-preview", nullptr, &app.panels.minimap);
      ImGui::MenuItem("Log", nullptr, &app.panels.log);
      ImGui::MenuItem("Style editor", nullptr, &app.panels.style);
      ImGui::MenuItem("ImGui demo", nullptr, &app.panels.demo);
      ImGui::EndMenu();
    }
    // Season selector, right of the menus
    ImGui::Separator();
    ImGui::SetNextItemWidth(140.0F);
    const auto& seasons = app.level->seasons;
    app.view.season = std::min(app.view.season, seasons.size() - 1);
    if (ImGui::BeginCombo("##season", seasons[app.view.season].name.c_str()))
    {
      for (std::size_t i = 0; i < seasons.size(); ++i)
      {
        if (ImGui::Selectable(seasons[i].name.c_str(), i == app.view.season))
        {
          app.view.season = i;
          invalidate_view(app);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TextDisabled(
      "%s | %dx%d | undo %zu | %s",
      app.level->settings.name.c_str(),
      app.level->settings.extents.width,
      app.level->settings.extents.height,
      app.undo.size(),
      memory::is_valid(app.memory.documents[0]) ? format<64>("doc %zu MiB", app.level->arena->utilized >> 20) : "");
    ImGui::EndMainMenuBar();
  }
  if (ImGui::GetIO().KeyCtrl and ImGui::IsKeyPressed(ImGuiKey_N) and !ImGui::GetIO().WantTextInput)
  {
    open_new = true;
  }
  new_level_popup(app, open_new);
  resize_popup(app, open_resize);
}

/// One scripted action per idle frame (see App::smoke); closes the editor when done
void smoke(App& app)
{
  if (!std::holds_alternative<std::monostate>(app.job) or app.frame_count < 5)
  {
    return;
  }
  if (app.show_error or ImGui::IsPopupOpen("Error"))
  {
    LVL_LOG_ERROR() << "Smoke test failed at step " << app.smoke_step;
    app.smoke = false;
    request_exit(app, true);
    return;
  }
  const int step = app.smoke_step++;
  LVL_LOG_INFO() << "Smoke step " << step;
  switch (step)
  {
  case 0:
    demo_level(app);
    break;
  case 1:
    start_heightmap(app);
    break;
  case 2:
    app.generators.watershed.erosion.droplets = 2000;
    app.generators.watershed.iterations = 2;
    start_watershed(app);
    break;
  case 3:
    start_wfc(app, 1);
    break;
  case 4:
    // Undo the WFC run, then redo it
    if (!app.undo.can_undo())
    {
      report(app, Status::error("nothing to undo after generating"));
      break;
    }
    undo(app);
    redo(app);
    break;
  case 5:
    file_action(app, FileAction::kSaveLevel, "smoke.json");
    break;
  case 6:
    file_action(app, FileAction::kSaveLevel, "smoke.lvlb");
    break;
  case 7:
    file_action(app, FileAction::kOpenLevel, "smoke.lvlb");
    break;
  case 8:
    file_action(app, FileAction::kExportPng, "smoke.png");
    break;
  case 9:
    file_action(app, FileAction::kExportMinimap, "smoke_preview.png");
    break;
  case 10:
    file_action(app, FileAction::kSaveTileSet, "smoke.tileset.json");
    break;
  case 11:
    file_action(app, FileAction::kSaveWfc, "smoke.wfc.json");
    break;
  case 12:
    file_action(app, FileAction::kSavePaths, "smoke.paths.lvlb");
    break;
  case 13:
    // Style profile round trip: save a modified light style, reset, load it back
    app.panels.style = true;
    {
      style::Profile light;
      style::apply(style::Preset::kLight, light);
      light.style.FrameRounding = 5.0F;
      style::activate(light);
    }
    file_action(app, FileAction::kSaveStyle, "smoke.style.json");
    style::activate(app.style_reference);
    style::apply(style::Preset::kDark, app.style_reference);
    style::activate(app.style_reference);
    file_action(app, FileAction::kLoadStyle, "smoke.style.json");
    if (ImGui::GetStyle().FrameRounding != 5.0F)
    {
      report(app, Status::error("style profile did not round-trip"));
    }
    break;
  case 15:
    // The file dialog (ImGuiFileDialog) opens and draws for a frame, then is closed
    open_file_dialog(app, FileAction::kOpenLevel);
    break;
  case 16:
    if (!file_dialog_open())
    {
      report(app, Status::error("the file dialog did not open"));
    }
    close_file_dialog(app);
    break;
  case 17:
    // The WFC neighbor editor draws for a frame; corner rules are derived from the sides
    app.panels.wfc_neighbors = true;
    app.level->wfc.diagonals = true;
    level::derive_diagonals(app.level->wfc, app.memory.frame);
    if (level::diagonal_count(app.level->wfc) == 0)
    {
      report(app, Status::error("no corner rules were derived"));
    }
    break;
  case 18:
    // WFC with 8-way adjacency
    start_wfc(app, 1);
    break;
  case 14: {
    // Deletes (confirmed, as with Shift+click): a material and a semantic type in use, a portal, a graph
    level::Level& lv = *app.level;
    const auto material = std::min<std::size_t>(level::intern_material(lv, "snow"), lv.materials.size() - 1);
    app.pending_delete =
      PendingDelete{.kind = DeleteKind::kMaterial, .index = material, .what = "material 'snow'", .confirmed = true};
    process_deletes(app);
    const auto semantic = level::intern_semantic(lv, "tunnel");
    app.pending_delete = PendingDelete{
      .kind = DeleteKind::kSemantic, .index = semantic, .what = "semantic type 'tunnel'", .confirmed = true};
    process_deletes(app);
    app.pending_delete = PendingDelete{
      .kind = DeleteKind::kObject,
      .entity = level::find_portal(lv.registry, "east_exit"),
      .what = "portal",
      .confirmed = true};
    process_deletes(app);
    if (!lv.graphs.empty())
    {
      app.pending_delete = PendingDelete{.kind = DeleteKind::kGraph, .index = 0, .what = "graph", .confirmed = true};
      process_deletes(app);
    }
    if (
      level::find_portal(lv.registry, "east_exit") != level::kNullEntity or !lv.graphs.empty() or
      !lv.registry.view<level::PathNode>().empty())
    {
      report(app, Status::error("deletes did not apply"));
    }
    for (const auto& m : lv.materials)
    {
      if (m.name == std::string_view{"snow"})
      {
        report(app, Status::error("material was not deleted"));
      }
    }
    break;
  }
  case 19: {
    // A batch metadata edit over a palette selection (which the tile panel then draws)
    level::TileSet& ts = app.level->tilesets[app.tileset];
    app.selection.retarget(app.tileset);
    app.selection.clear();
    for (std::size_t t = 0; t < std::min<std::size_t>(4, ts.tiles.size()); ++t)
    {
      app.selection.set(static_cast<level::TileIndex>(t), true);
    }
    app.batch = level::TileEdit{
      .set_collision = true,
      .collision = level::collision::kSolid,
      .set_property = true,
      .property_key = "batch",
      .property_value = "yes"};
    const auto tiles = selected_tiles(app, app.memory.frame);
    if (level::apply_edit(ts, app.batch, tiles.data(), tiles.size()) != app.selection.count)
    {
      report(app, Status::error("the batch edit did not reach every selected tile"));
    }
    for (const level::TileIndex t : tiles)
    {
      if (ts.tiles[t].collision != level::collision::kSolid or level::find_property(ts, t, "batch") == nullptr)
      {
        report(app, Status::error("the batch edit did not apply"));
      }
    }
    break;
  }
  case 20: {
    // WFC rules from the palette layout: the three tiles may only meet as the palette lays them out
    level::WfcRules& rules = app.level->wfc;
    const level::Extents grid = palette_grid(app, app.tileset);
    app.selection.retarget(app.tileset);
    app.selection.clear();
    for (int x = 0; x < std::min(3, grid.width); ++x)
    {
      app.selection.set(static_cast<level::TileIndex>(x), true);
    }
    app.generators.wfc_layout = level::LayoutMode::kExclusive;
    level::Grid<level::TileRef> layout{app.memory.frame};
    if (!palette_layout(app, layout))
    {
      report(app, Status::error("the palette selection has no layout"));
      break;
    }
    if (level::constrain_from_layout(rules, layout, app.generators.wfc_layout, app.memory.frame) == 0)
    {
      report(app, Status::error("no rules came from the palette layout"));
      break;
    }
    const auto tileset = static_cast<level::TileSetId>(app.tileset);
    const auto first = level::find_tile(rules, level::TileRef{tileset, 0});
    const auto second = level::find_tile(rules, level::TileRef{tileset, 1});
    if (!level::allowed(rules, first, second, level::Direction::kEast))
    {
      report(app, Status::error("the palette layout did not allow its own neighbors"));
    }
    // The same selection, as a batch edit of those rule tiles' constraints
    app.wfc_batch = level::WfcTileEdit{.set_weight = true, .weight = 2.5F};
    const auto rule_tiles = selected_rule_tiles(app, app.memory.frame);
    if (level::apply_edit(rules, app.wfc_batch, rule_tiles.data(), rule_tiles.size()) < 2)
    {
      report(app, Status::error("the batch constraints reached fewer than two rule tiles"));
    }
    for (const level::WfcTileId id : rule_tiles)
    {
      if (rules.tiles[id].weight != 2.5F)
      {
        report(app, Status::error("the batch constraints did not apply"));
      }
    }
    break;
  }
  case 21:
    // Quitting with unsaved changes asks first; Cancel keeps the editor open
    app.modified = true;
    request_exit(app);
    if (app.exit or !app.confirm_exit)
    {
      report(app, Status::error("quitting with unsaved changes did not ask first"));
    }
    break;
  case 22:
    // The prompt was drawn; choose Cancel
    if (!ImGui::IsPopupOpen("Unsaved changes") or app.exit)
    {
      report(app, Status::error("the unsaved-changes prompt is not showing"));
    }
    app.confirm_exit = false;
    break;
  default:
    // Finish through "Save and quit": the editor exits once the save succeeds
    LVL_LOG_INFO() << "Smoke test complete";
    app.smoke = false;
    app.modified = true;
    save_and_exit(app);
    break;
  }
}

/// The initial dock layout: viewport in the middle, panels around it
void dock_layout(ImGuiID dockspace)
{
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
  ImGuiID center = dockspace;
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22F, nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28F, nullptr, &center);
  ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22F, nullptr, &center);
  ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45F, nullptr, &left);
  ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5F, nullptr, &right);
  ImGui::DockBuilderDockWindow("Viewport", center);
  ImGui::DockBuilderDockWindow("Layers", left);
  ImGui::DockBuilderDockWindow("Mini-preview", left_bottom);
  ImGui::DockBuilderDockWindow("Objects & paths", left_bottom);
  ImGui::DockBuilderDockWindow("Tilesets", right);
  ImGui::DockBuilderDockWindow("Tile metadata", right_bottom);
  ImGui::DockBuilderDockWindow("Catalogs & seasons", right_bottom);
  ImGui::DockBuilderDockWindow("Generate", right_bottom);
  ImGui::DockBuilderDockWindow("Style", right_bottom);
  ImGui::DockBuilderDockWindow("Log", bottom);
  ImGui::DockBuilderFinish(dockspace);
}

}  // namespace

JobMemory& job_memory(App& app) { return app.job_memory; }

void start_job(App& app)
{
  std::visit(
    overloaded{
      [](std::monostate&) {},
      [](auto& bg) { bg.start(); },
    },
    app.job);
}

std::string_view view_season(const App& app)
{
  return (app.view.season < app.level->seasons.size()) ? app.level->seasons[app.view.season].name.view()
                                                       : std::string_view{};
}

void report(App& app, const Status& status)
{
  if (!status.ok)
  {
    LVL_LOG_ERROR() << status.message;
    app.error = status.message;
    app.show_error = true;
  }
}

void invalidate_view(App& app)
{
  app.overlays.all_dirty = true;
  app.minimap_dirty = true;
}

void invalidate_layer(App& app, std::size_t layer)
{
  if (layer < kMaxLayers)
  {
    app.overlays.dirty[layer] = true;
  }
  app.minimap_dirty = true;
}

void new_level(App& app, const level::Settings& settings)
{
  level::create_default(*app.level, settings);
  app.undo.clear();
  app.path.clear();
  app.modified = false;
  app.layer = 1;
  app.tileset = 0;
  app.entity = level::kNullEntity;
  app.view.fit = true;
  app.view.season = 0;
  AtlasImages none;
  apply_atlases(app, none, app.assets.current);
  update_title(app);
}

void reload_atlases(App& app)
{
  if (auto* job = prepare_job<ReloadAtlasesJob>(app); job != nullptr)
  {
    job->level = app.level;
    job->assets = &app.memory.assets[1 - app.assets.current];
    start_job(app);
  }
}

Status initialize(App& app, const MemoryConfig& config, const char* path)
{
  app.config = config;
  reserve_memory(app);
  json::install(app.memory.json);
  app.level = new (app.level_storage[0]) level::Level{app.memory.documents[0]};
  app.spare = new (app.level_storage[1]) level::Level{app.memory.documents[1]};
  app.undo.initialize(app.memory.undo);
  app.generators.heightmap.layer_count = 2;
  app.generators.heightmap.layers[1] = procgen::NoiseLayer{
    .kind = procgen::NoiseKind::kRidged,
    .blend = procgen::Blend::kAdd,
    .frequency = 0.04F,
    .amplitude = 0.35F,
    .octaves = 4};
  app.export_options.objects = false;
  // UI style: lvl's preset, overridden by the user's default profile if there is one
  style::apply(style::Preset::kLvl, app.style_reference);
  if (file::exists(style::default_path()))
  {
    if (const Status s = style::load_file(style::default_path(), app.style_reference); !s.ok)
    {
      LVL_LOG_WARN() << "Ignoring default style: " << s.message;
    }
    else
    {
      LVL_LOG_INFO() << "Loaded style '" << style::default_path() << "'";
    }
  }
  style::activate(app.style_reference);
  new_level(app, level::Settings{});
  if constexpr (web::kAvailable)
  {
    app.dialog.directory.assign(web::kWorkDirectory);
  }
  if (path != nullptr and path[0] != '\0')
  {
    file_action(app, FileAction::kOpenLevel, path);
  }
  return Status::okay();
}

void shutdown(App& app)
{
  // Finish (or cancel) any running job before tearing down memory it uses
  std::visit(
    overloaded{
      [](std::monostate&) {},
      [](auto& bg) {
        bg.progress().cancel();
        bg.join();
      },
    },
    app.job);
  app.job.emplace<std::monostate>();
  for (auto& t : app.assets.textures)
  {
    release(t);
  }
  for (auto& t : app.overlays.textures)
  {
    release(t);
  }
  release(app.minimap_texture);
  save_file_dialog_places(app);
  app.level->~Level();
  app.spare->~Level();
  json::uninstall();
  LVL_LOG_INFO() << app.memory.documents[0];
  LVL_LOG_INFO() << app.memory.jobs;
  memory::destroy(app.memory.root);
}

void frame(App& app)
{
  memory::clear(app.memory.frame);
  // The window's close button (and Alt+F4 etc.) go through the unsaved-changes check
  if (should_close())
  {
    cancel_close();
    request_exit(app);
  }
  web::set_unsaved_changes(app.modified);
  const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
  static bool layout_done = false;
  if (!layout_done)
  {
    layout_done = true;
    if (ImGui::DockBuilderGetNode(dockspace) == nullptr or ImGui::DockBuilderGetNode(dockspace)->IsLeafNode())
    {
      dock_layout(dockspace);
    }
  }
  main_menu(app);
  shortcuts(app);

  viewport_window(app);
  if (app.panels.layers)
  {
    layers_panel(app);
  }
  if (app.panels.tilesets)
  {
    tilesets_panel(app);
  }
  if (app.panels.tile_meta)
  {
    tile_meta_panel(app);
  }
  if (app.panels.catalogs)
  {
    catalogs_panel(app);
  }
  if (app.panels.generate)
  {
    generate_panel(app);
  }
  if (app.panels.wfc_neighbors)
  {
    wfc_neighbors_window(app);
  }
  if (app.panels.objects)
  {
    objects_panel(app);
  }
  if (app.panels.minimap)
  {
    minimap_panel(app);
  }
  if (app.panels.log)
  {
    log_panel(app);
  }
  if (app.panels.style)
  {
    style_panel(app);
  }
  if (app.panels.demo)
  {
    ImGui::ShowDemoWindow(&app.panels.demo);
  }
  file_dialog(app);
  exit_prompt(app);
  process_deletes(app);
  job_progress(app);
  error_popup(app);
  web::poll(app);
  if (app.smoke)
  {
    smoke(app);
  }
  ++app.frame_count;
  if (app.max_frames > 0 and app.frame_count >= app.max_frames)
  {
    request_exit(app, true);
  }
}

void request_exit(App& app, bool force)
{
  if (force or !app.modified)
  {
    app.exit = true;
    return;
  }
  app.confirm_exit = true;
}

void save_and_exit(App& app)
{
  app.confirm_exit = false;
  app.exit_after_save = true;
  if (app.path.empty())
  {
    // Exits when the dialog's save succeeds; cancelling the dialog cancels quitting
    open_file_dialog(app, FileAction::kSaveLevel);
  }
  else
  {
    file_action(app, FileAction::kSaveLevel, app.path.c_str());
  }
}

bool color_edit(const char* label, image::Rgba& color)
{
  float c[4] = {color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F};
  if (ImGui::ColorEdit4(label, c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar))
  {
    color = image::Rgba{
      static_cast<std::uint8_t>(c[0] * 255.0F + 0.5F),
      static_cast<std::uint8_t>(c[1] * 255.0F + 0.5F),
      static_cast<std::uint8_t>(c[2] * 255.0F + 0.5F),
      static_cast<std::uint8_t>(c[3] * 255.0F + 0.5F)};
    return true;
  }
  return false;
}

// -------------------------------------------------------------------------------------------------
// Files
// -------------------------------------------------------------------------------------------------

void file_action(App& app, FileAction action, const char* path)
{
  // Small files are handled synchronously; level load/save and exports run as jobs
  JobMemory& jm = job_memory(app);
  const auto small_io = [&](auto&& fn) {
    if (!std::holds_alternative<std::monostate>(app.job))
    {
      report(app, Status::error("another operation is still running"));
      return;
    }
    partition(app.memory.jobs, jm);
    auto ctx = jm.context(nullptr);
    report(app, fn(ctx));
    memory::clear(app.memory.jobs);
  };
  switch (action)
  {
  case FileAction::kNone:
    break;
  case FileAction::kOpenLevel:
    if (auto* job = prepare_job<LoadLevelJob>(app); job != nullptr)
    {
      job->level = app.spare;
      job->assets = &app.memory.assets[1 - app.assets.current];
      job->path.assign(path);
      start_job(app);
    }
    break;
  case FileAction::kSaveLevel:
    if (auto* job = prepare_job<SaveLevelJob>(app); job != nullptr)
    {
      job->level = app.level;
      job->path.assign(path);
      start_job(app);
    }
    break;
  case FileAction::kLoadTileSet:
    small_io([&](level::IoContext& ctx) {
      level::TileSetId id = level::kNoTileSet;
      Status s = level::load_tileset(path, *app.level, ctx, id);
      if (s.ok)
      {
        app.tileset = id;
        app.modified = true;
      }
      return s;
    });
    reload_atlases(app);
    break;
  case FileAction::kSaveTileSet:
    small_io([&](level::IoContext& ctx) {
      Status s = level::save_tileset(path, *app.level, static_cast<level::TileSetId>(app.tileset), ctx);
      if (s.ok)
      {
        web::offer_download(path);
      }
      return s;
    });
    break;
  case FileAction::kLoadWfc:
    small_io([&](level::IoContext& ctx) {
      app.modified = true;
      return level::load_wfc_rules(path, *app.level, ctx);
    });
    break;
  case FileAction::kSaveWfc:
    small_io([&](level::IoContext& ctx) {
      Status s = level::save_wfc_rules(path, *app.level, ctx);
      if (s.ok)
      {
        web::offer_download(path);
      }
      return s;
    });
    break;
  case FileAction::kLoadPaths:
    small_io([&](level::IoContext& ctx) {
      app.entity = level::kNullEntity;
      app.edge_start = level::kNullEntity;
      app.route_from = level::kNullEntity;
      app.modified = true;
      return level::load_paths(path, *app.level, ctx);
    });
    break;
  case FileAction::kSavePaths:
    small_io([&](level::IoContext& ctx) {
      Status s = level::save_paths(path, *app.level, ctx);
      if (s.ok)
      {
        web::offer_download(path);
      }
      return s;
    });
    break;
  case FileAction::kAtlasImage:
    if (app.tileset < app.level->tilesets.size())
    {
      app.level->tilesets[app.tileset].image.assign(path);
      app.modified = true;
      reload_atlases(app);
    }
    break;
  case FileAction::kSeedImage:
    app.generators.seed_image.assign(path);
    break;
  case FileAction::kExportPng:
  case FileAction::kExportMinimap:
    if (auto* job = prepare_job<ExportJob>(app); job != nullptr)
    {
      job->level = app.level;
      job->atlases = app.assets.atlases(app.level->tilesets.size());
      job->options = app.export_options;
      job->options.season = view_season(app);
      job->options.tint = app.view.tint;
      job->minimap = (action == FileAction::kExportMinimap);
      job->max_size = app.minimap_size;
      job->path.assign(path);
      start_job(app);
    }
    break;
  case FileAction::kSaveStyle:
    if (const Status s = style::save_file(path, style::current()); s.ok)
    {
      app.style_reference = style::current();
      LVL_LOG_INFO() << "Saved style '" << path << "'";
      web::offer_download(path);
    }
    else
    {
      report(app, s);
    }
    break;
  case FileAction::kLoadStyle:
    if (style::Profile profile = style::current(); true)
    {
      if (const Status s = style::load_file(path, profile); s.ok)
      {
        style::activate(profile);
        app.style_reference = profile;
        LVL_LOG_INFO() << "Loaded style '" << path << "'";
      }
      else
      {
        report(app, s);
      }
    }
    break;
  }
  update_title(app);
}


// -------------------------------------------------------------------------------------------------
// Demo level
// -------------------------------------------------------------------------------------------------

void demo_level(App& app)
{
  using namespace level;
  // A code-generated atlas: 8 columns x 3 rows of 16x16 tiles
  //   row 0: deep water, shallow water, sand, grass, meadow, forest, rock, snow
  //   row 1: the same in winter (ice, frozen shore, snowy sand/grass/meadow/forest/rock, snow)
  //   row 2: road, wall, door, bridge
  constexpr int kTile = 16;
  struct Base
  {
    image::Rgba color;
    int pattern;
  };
  static constexpr Base kSummer[8] = {
    {{30, 60, 140, 255}, 1},
    {{60, 120, 200, 255}, 1},
    {{220, 205, 150, 255}, 2},
    {{90, 160, 70, 255}, 2},
    {{110, 175, 80, 255}, 3},
    {{40, 105, 45, 255}, 4},
    {{125, 118, 110, 255}, 5},
    {{240, 240, 245, 255}, 2},
  };
  static constexpr Base kWinter[8] = {
    {{170, 200, 230, 255}, 5},
    {{190, 215, 235, 255}, 5},
    {{230, 228, 220, 255}, 2},
    {{225, 232, 238, 255}, 2},
    {{220, 230, 236, 255}, 3},
    {{150, 175, 165, 255}, 4},
    {{180, 182, 188, 255}, 5},
    {{250, 250, 255, 255}, 2},
  };
  static constexpr Base kExtra[4] = {
    {{150, 120, 85, 255}, 2},
    {{90, 85, 80, 255}, 6},
    {{120, 75, 40, 255}, 7},
    {{140, 100, 60, 255}, 6},
  };
  memory::Arena& scratch = app.memory.frame;
  memory::Scope scope{scratch};
  image::Image atlas = image::create(scratch, 8 * kTile, 3 * kTile, image::Rgba{0, 0, 0, 0});
  const auto paint = [&](int col, int row, const Base& b) {
    for (int y = 0; y < kTile; ++y)
    {
      for (int x = 0; x < kTile; ++x)
      {
        const auto h = random::hash(static_cast<std::uint64_t>(col * 31 + row * 7 + b.pattern), x, y);
        const int n = static_cast<int>(h % 21) - 10;
        int k = n;
        switch (b.pattern)
        {
        case 1:  // waves
          k += ((x + 2 * (y % 8)) % 16 < 2) ? 25 : 0;
          break;
        case 3:  // flowers
          k += (h % 23 == 0) ? 90 : 0;
          break;
        case 4:  // canopy
          k +=
            static_cast<int>(18.0F * std::sin(static_cast<float>(x) * 0.8F) * std::cos(static_cast<float>(y) * 0.7F));
          break;
        case 5:  // cracks
          k += ((x == y) or (x + y == 15)) ? -30 : 0;
          break;
        case 6:  // bricks
          k += ((y % 4 == 0) or ((x + ((y / 4) % 2) * 4) % 8 == 0)) ? -40 : 0;
          break;
        case 7:  // planks
          k += (x % 5 == 0) ? -35 : 0;
          break;
        default:
          break;
        }
        const auto c = [k](std::uint8_t v) {
          return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v) + k, 0, 255));
        };
        atlas.at(col * kTile + x, row * kTile + y) = image::Rgba{c(b.color.r), c(b.color.g), c(b.color.b), 255};
      }
    }
  };
  for (int i = 0; i < 8; ++i)
  {
    paint(i, 0, kSummer[i]);
    paint(i, 1, kWinter[i]);
  }
  for (int i = 0; i < 4; ++i)
  {
    paint(i, 2, kExtra[i]);
  }
  const char* atlas_path = "lvl_demo_atlas.png";
  memory::Arena encode;
  memory::push(scratch, encode, "encode");
  const Status saved = image::save_png(atlas_path, atlas, encode);
  memory::pop(scratch, encode);
  if (!saved.ok)
  {
    report(app, saved);
    return;
  }

  new_level(app, Settings{.name = "demo", .extents = {96, 64}, .cell_width = kTile, .cell_height = kTile, .seed = 7});
  Level& lv = *app.level;
  lv.seasons[0] = Season{.name = "summer", .duration_days = 90, .temperature = 22.0F, .precipitation = 1.5F};
  lv.seasons.push_back(Season{
    .name = "autumn", .duration_days = 60, .temperature = 10.0F, .precipitation = 4.0F, .tint = {255, 225, 190, 255}});
  lv.seasons.push_back(Season{
    .name = "winter",
    .duration_days = 90,
    .temperature = -6.0F,
    .precipitation = 2.5F,
    .humidity = 0.7F,
    .tint = {215, 230, 255, 255}});
  const char* semantic_names[] = {
    "water", "sand", "grass", "forest", "rock", "snow", "road", "river", "lake", "tunnel"};
  const image::Rgba semantic_colors[] = {
    {40, 90, 200, 255},
    {220, 200, 140, 255},
    {90, 170, 70, 255},
    {30, 100, 40, 255},
    {130, 120, 110, 255},
    {245, 245, 250, 255},
    {160, 120, 80, 255},
    {60, 140, 255, 255},
    {20, 70, 180, 255},
    {120, 60, 160, 255}};
  for (std::size_t i = 0; i < std::size(semantic_names); ++i)
  {
    lv.semantics[intern_semantic(lv, semantic_names[i])].color = semantic_colors[i];
  }
  const char* material_names[] = {"water", "sand", "grass", "dirt", "stone", "snow", "ice", "wood"};
  for (const char* m : material_names)
  {
    lv.materials[intern_material(lv, m)].color = render::id_color(intern_material(lv, m) * 7U);
  }
  lv.materials[intern_material(lv, "ice")].friction = 0.15F;
  lv.materials[intern_material(lv, "snow")].speed = 0.7F;

  const TileSetId ts_id = add_tileset(lv, "demo");
  TileSet& ts = lv.tilesets[ts_id];
  ts.image = atlas_path;
  ts.slice = SliceParams{.tile_width = kTile, .tile_height = kTile};
  ts.image_width = atlas.width;
  ts.image_height = atlas.height;
  reslice(ts);
  const char* tile_names[] = {"deep_water", "shallow_water", "sand", "grass", "meadow", "forest", "rock", "snow"};
  const char* tile_semantics[] = {"water", "water", "sand", "grass", "grass", "forest", "rock", "snow"};
  const char* tile_materials[] = {"water", "water", "sand", "grass", "grass", "dirt", "stone", "snow"};
  for (TileIndex t = 0; t < 8; ++t)
  {
    ts.tiles[t].name.assign(tile_names[t]);
    ts.tiles[t].semantic = intern_semantic(lv, tile_semantics[t]);
    ts.tiles[t].material = intern_material(lv, tile_materials[t]);
    ts.tiles[8 + t].name.assign(format<64>("%s_winter", tile_names[t]));
    ts.tiles[8 + t].semantic = ts.tiles[t].semantic;
    ts.tiles[8 + t].material = intern_material(lv, t < 2 ? "ice" : "snow");
    set_seasonal_variant(
      ts,
      SeasonalVariant{
        .tile = t,
        .season = "winter",
        .alternate = static_cast<TileIndex>(8 + t),
        .material = ts.tiles[8 + t].material});
  }
  ts.tiles[0].collision = collision::kSolid;
  ts.tiles[6].collision = collision::kSolid;
  ts.tiles[16].name.assign("road");
  ts.tiles[16].semantic = intern_semantic(lv, "road");
  ts.tiles[16].material = intern_material(lv, "dirt");
  ts.tiles[17].name.assign("wall");
  ts.tiles[17].collision = collision::kSolid | collision::kSightBlocker;
  ts.tiles[17].material = intern_material(lv, "stone");
  ts.tiles[18].name.assign("door");
  ts.tiles[18].collision = collision::kTrigger;
  ts.tiles[19].name.assign("bridge");
  ts.tiles[19].material = intern_material(lv, "wood");
  ts.properties.push_back(TileProperty{.tile = 18, .key = "sound", .value = "door_open"});
  for (TileIndex t = 20; t < ts.tiles.size(); ++t)
  {
    ts.tiles[t].hidden = true;
  }

  // WFC rules: elevation bands; neighbors only one band apart (meadow/grass/forest interchangeable)
  WfcRules& rules = lv.wfc;
  rules.name.assign("demo terrain");
  const float bands[8][2] = {
    {0.0F, 0.32F},
    {0.26F, 0.4F},
    {0.34F, 0.46F},
    {0.4F, 0.72F},
    {0.4F, 0.66F},
    {0.5F, 0.8F},
    {0.7F, 0.94F},
    {0.86F, 1.0F}};
  const float weights[8] = {4, 2, 2, 6, 2, 4, 3, 2};
  const int order[8] = {0, 1, 2, 3, 3, 3, 4, 5};
  for (TileIndex t = 0; t < 8; ++t)
  {
    const WfcTileId id = add_tile(rules, TileRef{ts_id, t});
    rules.tiles[id].weight = weights[t];
    rules.tiles[id].height_min = bands[t][0];
    rules.tiles[id].height_max = bands[t][1];
  }
  for (WfcTileId a = 0; a < 8; ++a)
  {
    for (WfcTileId b = 0; b < 8; ++b)
    {
      if (std::abs(order[a] - order[b]) <= 1)
      {
        for (const Direction d : kDirections)
        {
          set_allowed(rules, a, b, d, true);
        }
      }
    }
  }

  // Terrain: heightmap, then WFC over it (small enough to run right here)
  add_layer(lv, LayerKind::kWater, "water");
  add_layer(lv, LayerKind::kSemantic, "semantics");
  lv.layers.back().visible = false;
  auto& height = plane<ScalarGrid>(lv.layers[0]);
  app.generators.heightmap.falloff = 0.35F;
  procgen::generate_heightmap(
    app.generators.heightmap, lv.settings.seed, height, nullptr, jobs::hardware_concurrency());
  fit_range(lv.layers[0]);
  memory::Arena work;
  memory::push(scratch, work, "wfc");
  const procgen::WfcConstraints constraints{.height = &height, .height_lo = 0.0F, .height_hi = 1.0F};
  const auto result = procgen::solve(
    rules, constraints, procgen::WfcParams{.seed = lv.settings.seed}, plane<TileGrid>(lv.layers[1]), work);
  memory::pop(scratch, work);
  if (!result.status.ok)
  {
    LVL_LOG_WARN() << "Demo WFC: " << result.status.message;
  }
  // A road graph, a portal and a spawn marker
  const GraphId road = add_graph(lv, "main road", GraphKind::kRoad);
  const Entity a = create_path_node(lv.registry, road, {20, 30});
  const Entity b = create_path_node(lv.registry, road, {48, 26});
  const Entity c = create_path_node(lv.registry, road, {70, 34});
  create_path_edge(lv.registry, a, b);
  create_path_edge(lv.registry, b, c);
  create_portal(
    lv.registry,
    "east_exit",
    {94, 30},
    {2, 4},
    Portal{.target_level = "east.json", .target_portal = "west_entry", .exit = Direction::kWest});
  create_marker(lv.registry, "spawn", "player", {21.0F, 31.0F});
  app.layer = 1;
  app.generators.height_layer = 0;
  reload_atlases(app);
  LVL_LOG_INFO() << "Created demo level (atlas written to '" << atlas_path << "')";
}

}  // namespace lvl::editor
