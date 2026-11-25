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
#include "lvl/procgen/autotile.hpp"
#include "lvl/procgen/wfc.hpp"
#include "lvl/project/io.hpp"
#include "lvl/project/paths.hpp"

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
  memory::push(m.root, m.documents[0], total / 100 * 15, "document-a");
  memory::push(m.root, m.documents[1], total / 100 * 15, "document-b");
  // The catalogs, seasons, tilesets and rule sets the levels share (paired like the documents)
  memory::push(m.root, m.projects[0], total / 100 * 3, "project-a");
  memory::push(m.root, m.projects[1], total / 100 * 3, "project-b");
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
    const bool present = i < atlases.count and i < app.project->tilesets.size();
    app.assets.images[i] = present ? atlases.images[i] : image::Image{};
    app.assets.status[i] = present ? atlases.status[i] : Status::okay();
    app.assets.tile_colors[i] = memory::make_vector<image::Rgba>(arena);
    if (present and !atlases.images[i].empty())
    {
      upload(app.assets.textures[i], atlases.images[i]);
      level::TileSet& ts = app.project->tilesets[i];
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
  // The project and the level are saved separately, so the star means "either has changes"
  const bool dirty = app.modified or app.project_modified;
  if (app.project_path.empty())
  {
    set_title(format<600>("lvl - %s%s", app.path.empty() ? "untitled" : app.path.c_str(), dirty ? " *" : ""));
    return;
  }
  set_title(format<600>(
    "lvl - %s/%s%s",
    app.project->name.c_str(),
    app.level->settings.name.empty() ? "untitled" : app.level->settings.name.c_str(),
    dirty ? " *" : ""));
}

/**
 * @brief Makes the open level a member of its project, and refreshes what the index says about it
 *
 * Called whenever a level is saved or loaded, because the index is what lets a portal in another
 * level name this one and have its portals offered without this level being loaded. A member is
 * found by name first, since that is how levels address each other; a level saved under a new name
 * keeps the file it had and takes the new name, rather than becoming a second member.
 */
Status add_member(App& app, std::string_view name, std::string_view file, level::Level& lv)
{
  project::Project& pr = *app.project;
  Path relative;
  if (!project::relativize(pr.root.view(), file, relative).ok)
  {
    relative.assign(file);
  }

  bool changed = false;
  std::size_t index = project::find_level(pr, name);
  if (index == project::kNoLevel)
  {
    if (const std::size_t by_file = project::find_level_by_file(pr, relative.view()); by_file != project::kNoLevel)
    {
      index = by_file;
      project::rename_level(pr, index, name);
      changed = true;
    }
  }
  if (index == project::kNoLevel)
  {
    index = project::add_level(pr, name, relative.view());
    if (index == project::kNoLevel)
    {
      return Status::error(
        "'%.*s' cannot be added: the project is full, or another level of that name is already a "
        "member",
        static_cast<int>(name.size()),
        name.data());
    }
    changed = true;
  }
  changed = changed or pr.levels[index].file != relative or pr.levels[index].missing;
  pr.levels[index].file = relative;
  pr.levels[index].missing = false;

  memory::Scope scope{app.memory.frame};
  auto portals = memory::make_vector<Name>(app.memory.frame);
  auto markers = memory::make_vector<Name>(app.memory.frame);
  for (auto [e, label, portal] : lv.registry.view<level::Label, level::Portal>().each())
  {
    portals.push_back(label.name);
  }
  for (auto [e, label, marker] : lv.registry.view<level::Label, level::Marker>().each())
  {
    markers.push_back(label.name);
  }

  // Whether the index now says anything different, compared before set_level_objects rebuilds the
  // name table. This runs on every load as well as every save, and loading a member re-reads what
  // the manifest already said: a project must not report unsaved changes for being opened.
  const project::LevelEntry& entry = pr.levels[index];
  const auto same_names = [](const Name* was, std::size_t was_count, const memory::Vector<Name>& now) {
    return was_count == now.size() and std::equal(now.begin(), now.end(), was);
  };
  changed = changed or entry.extents != lv.settings.extents or
    !same_names(project::portals_of(pr, entry), entry.portal_count, portals) or
    !same_names(project::markers_of(pr, entry), entry.marker_count, markers);

  const Status s = project::set_level_objects(
    pr,
    index,
    project::LevelObjects{
      .extents = lv.settings.extents,
      .portals = portals.data(),
      .portal_count = portals.size(),
      .markers = markers.data(),
      .marker_count = markers.size()},
    app.memory.frame);
  app.project_modified = app.project_modified or (s.ok and changed);
  return s;
}

/// Records the open level as a member, and refreshes what the index says about it
void refresh_level_entry(App& app, std::string_view file)
{
  const Status s = add_member(app, app.level->settings.name.view(), file, *app.level);
  if (!s.ok)
  {
    report(app, s);
  }
}

/**
 * @brief Opens member level <code>index</code> of the open project in place of the open one
 *
 * The file is a member, so it carries none of the shared sections: it is read into the open project
 * rather than the spare, and the project and its tilesets stay as they are. The caller is
 * responsible for the unsaved-changes prompt.
 *
 * <code>decode_atlases</code> is false for an ordinary switch, where the tilesets are the same
 * objects and their atlases are already decoded, and true when the project itself has just been
 * read in and the tilesets are new.
 */
void switch_to_level(App& app, std::size_t index, bool decode_atlases = false)
{
  project::Project& pr = *app.project;
  if (index >= pr.levels.size())
  {
    return;
  }
  Path absolute;
  if (Status s = project::resolve(pr.root.view(), pr.levels[index].file.view(), absolute); !s.ok)
  {
    report(app, s);
    return;
  }
  if (auto* job = prepare_job<LoadLevelJob>(app); job != nullptr)
  {
    job->project = app.project;
    job->level = app.spare;
    job->keep_project = true;
    // The tilesets are the same objects, so their atlases are already decoded
    job->decode_atlases = decode_atlases;
    job->assets = &app.memory.assets[1 - app.assets.current];
    job->path = absolute;
    start_job(app);
  }
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
          refresh_level_entry(app, job.path.view());
          remember_recent_file(app, job.path.view());
          web::offer_download(job.path.c_str());
          // Quitting or switching was waiting on this save
          app.exit = app.exit or app.exit_after_save;
          if (app.switch_after_save and app.switch_to != project::kNoLevel)
          {
            const std::size_t index = app.switch_to;
            app.switch_to = project::kNoLevel;
            app.switch_after_save = false;
            switch_to_level(app, index);
            report(app, job.result);
            return;
          }
        }
        app.exit_after_save = false;
        app.switch_after_save = false;
        app.switch_to = project::kNoLevel;
        report(app, job.result);
      },
      [&app](jobs::Background<LoadLevelJob>& bg) {
        auto& job = bg.job();
        if (job.result.ok)
        {
          std::swap(app.level, app.spare);
          level::reset(*app.spare);
          if (!job.keep_project)
          {
            // Opening a level brings its project with it; switching within one, or opening a
            // project whose manifest is already in, does not
            std::swap(app.project, app.spare_project);
            project::reset(*app.spare_project);
          }
          if (job.decode_atlases)
          {
            apply_atlases(app, job.atlases, 1 - app.assets.current);
          }
          app.path = job.path;
          app.modified = false;
          refresh_level_entry(app, job.path.view());
          remember_recent_file(app, job.path.view());
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
          if (!job.keep_project)
          {
            project::reset(*app.spare_project);
          }
          // A level that could not be loaded does not belong in the recent list
          forget_recent_file(app, job.path.view());
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
      [&app](jobs::Background<RescanProjectJob>& bg) {
        auto& job = bg.job();
        project::Project& pr = *app.project;
        // Written here rather than in the job, because the project is the thing the panels read
        std::size_t updated = 0;
        for (std::size_t i = 0; i < job.member_count and i < pr.levels.size(); ++i)
        {
          const auto& member = job.members[i];
          if (!member.read)
          {
            pr.levels[i].missing = true;
            continue;
          }
          const Status s = project::set_level_objects(
            pr,
            i,
            project::LevelObjects{
              .extents = member.extents,
              .portals = job.names + member.portals_begin,
              .portal_count = member.portal_count,
              .markers = job.names + member.markers_begin,
              .marker_count = member.marker_count},
            app.memory.frame);
          if (!s.ok)
          {
            report(app, s);
            break;
          }
          pr.levels[i].missing = false;
          ++updated;
        }
        app.project_modified = app.project_modified or updated > 0;
        if (job.failed > 0)
        {
          report(
            app,
            Status::error(
              "%zu of %zu levels could not be read: %s", job.failed, job.member_count, job.result.message.c_str()));
        }
        else
        {
          LVL_LOG_INFO_FMT("Rescanned %zu level(s)", updated);
        }
      },
      [&app](jobs::Background<CompactCatalogsJob>& bg) {
        auto& job = bg.job();
        if (!job.result.ok)
        {
          // Nothing is dropped, so the ids on disk and the ids in the project still agree. The
          // members already rewritten hold the new ids, which is why this says where it stopped.
          report(
            app,
            Status::error(
              "compacting stopped at '%s' after %zu level(s): %s. The catalogs were left as they "
              "were; fix that file and run it again.",
              job.stopped_on.c_str(),
              job.rewritten,
              job.result.message.c_str()));
          return;
        }
        // The open level is the one the job skipped, because a job may not touch the document
        project::apply_compaction(*job.plan, *app.level);
        project::finish_compaction(*app.project, *job.plan);
        app.undo.clear();
        invalidate_view(app);
        app.modified = true;
        app.project_modified = true;
        LVL_LOG_INFO_FMT("Compacted the catalogs; %zu other level(s) rewritten", job.rewritten);
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

/**
 * @brief The start-up prompt: open one of the recent projects, or make a new one
 *
 * A project is what the editor is for, and it is also the thing that is tedious to reach through
 * the menus on a cold start -- the manifest is always called the same thing, so File > Open would
 * have the user navigating to a folder to pick a file whose name they already know. This asks the
 * one question that start-up has an answer to, and asks it only when the command line did not
 * already answer it.
 *
 * It is deliberately an escape rather than a gate: behind it is the blank level the editor has
 * always started with, and New project turns that into a project at any time.
 */
void welcome_prompt(App& app)
{
  if (!app.welcome)
  {
    return;
  }
  // Every way out of the prompt clears the flag first, so a job in flight means something has
  // already been chosen; the modal must not sit over the progress dialog
  if (busy(app) or file_dialog_open())
  {
    app.welcome = false;
    return;
  }
  load_recent_files(app);
  if (!ImGui::IsPopupOpen("Welcome to lvl"))
  {
    ImGui::OpenPopup("Welcome to lvl");
  }
  ImGui::SetNextWindowSize(ImVec2{480, 0});
  if (!ImGui::BeginPopupModal("Welcome to lvl", nullptr, ImGuiWindowFlags_NoResize))
  {
    return;
  }
  ImGui::TextWrapped("Open one of your projects, or start a new one.");
  ImGui::Spacing();
  // The recent list holds levels as well, and entries whose file has gone away. Neither belongs in
  // a start-up prompt: a level is not what you reopen, and a path that no longer resolves is worse
  // than a shorter list. Both are still reachable through File > Open recent, which says which of
  // its entries are missing rather than hiding them
  Path chosen;
  std::size_t listed = 0;
  ImGui::BeginChild("projects", ImVec2{0, 6 * ImGui::GetTextLineHeightWithSpacing()}, ImGuiChildFlags_Borders);
  for (std::size_t i = 0; i < app.recent.count; ++i)
  {
    const Path& path = app.recent.paths[i];
    if (project::file_name(path.view()) != project::kManifestName or !file::exists(path.c_str()))
    {
      continue;
    }
    ++listed;
    // Every manifest is called the same thing, so a project is named by its folder
    const std::string_view name = project::file_name(project::parent_path(path.view()));
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(format<Path::capacity()>("%.*s", static_cast<int>(name.size()), name.data())))
    {
      chosen = path;
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("%s", path.c_str());
    }
    ImGui::PopID();
  }
  if (listed == 0)
  {
    ImGui::TextDisabled("No projects yet.");
  }
  ImGui::EndChild();
  ImGui::Spacing();
  const bool make = ImGui::Button("New project...");
  ImGui::SameLine();
  // Offered beside the list because a project the editor has not seen before is in neither: a fresh
  // checkout of someone else's project has an empty recent list
  const bool open = ImGui::Button("Open project...");
  ImGui::SameLine();
  const bool skip = ImGui::Button("Not now") or ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (!chosen.empty() or make or open or skip)
  {
    app.welcome = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  // Acted on once the popup is closed out, so the file dialog does not open underneath it
  if (!chosen.empty())
  {
    // A project is opened as a project, which goes on to load its first member
    file_action(app, FileAction::kOpenProject, chosen.c_str());
  }
  else if (make)
  {
    open_file_dialog(app, FileAction::kNewProject);
  }
  else if (open)
  {
    open_file_dialog(app, FileAction::kOpenProject);
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
  const bool switching = app.switch_to != project::kNoLevel;
  ImGui::Text("'%s' has unsaved changes.", app.path.empty() ? app.level->settings.name.c_str() : app.path.c_str());
  ImGui::TextUnformatted(switching ? "Save them before opening the other level?" : "Save them before quitting?");
  ImGui::Spacing();
  const char* save_label = switching ? (app.path.empty() ? "Save as... and switch" : "Save and switch")
                                     : (app.path.empty() ? "Save as... and quit" : "Save and quit");
  if (ImGui::Button(save_label) or ImGui::IsKeyPressed(ImGuiKey_Enter))
  {
    ImGui::CloseCurrentPopup();
    if (switching)
    {
      save_and_switch(app);
    }
    else
    {
      save_and_exit(app);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(switching ? "Discard and switch" : "Quit without saving"))
  {
    ImGui::CloseCurrentPopup();
    app.confirm_exit = false;
    if (switching)
    {
      const std::size_t index = app.switch_to;
      app.switch_to = project::kNoLevel;
      app.modified = false;
      switch_to_level(app, index);
    }
    else
    {
      app.exit = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") or ImGui::IsKeyPressed(ImGuiKey_Escape) or !app.confirm_exit)
  {
    ImGui::CloseCurrentPopup();
    app.confirm_exit = false;
    app.switch_to = project::kNoLevel;
  }
  ImGui::EndPopup();
}

/// Stores the chosen image, by whatever route the user and the project left open
void apply_import(App& app, FileAction action, std::string_view stored)
{
  switch (action)
  {
  case FileAction::kAtlasImage:
    if (app.tileset < app.project->tilesets.size())
    {
      app.project->tilesets[app.tileset].image.assign(stored);
      // The atlas belongs to a tileset, which belongs to the project
      app.project_modified = true;
      reload_atlases(app);
    }
    break;
  case FileAction::kSeedImage:
    app.generators.seed_image.assign(stored);
    break;
  default:
    break;
  }
}

/**
 * @brief Takes an image the user chose, asking first if it is outside the project
 *
 * A file already inside the tree is simply relativized, which is what makes a project portable
 * without the user thinking about it. One outside raises the prompt below; with no project folder
 * yet there is nowhere to copy to, so the path is stored as it is.
 */
void request_import(App& app, FileAction action, const char* path)
{
  project::Project& pr = *app.project;
  if (app.project_path.empty() or pr.root.empty())
  {
    apply_import(app, action, std::string_view{path});
    return;
  }
  const project::Folder folder =
    (action == FileAction::kAtlasImage) ? project::Folder::kAtlases : project::Folder::kImages;
  project::CopyPlan plan;
  if (const Status s = project::plan_copy(pr.root.view(), folder, std::string_view{path}, app.memory.frame, plan);
      !s.ok)
  {
    report(app, s);
    return;
  }
  if (plan.inside)
  {
    // Already in the tree: nothing to ask, and the stored path is its relative one
    apply_import(app, action, plan.destination.view());
    return;
  }
  app.pending_import = PendingImport{.action = action, .plan = plan};
  app.pending_import.source.assign(path);
}

/// The copy-into-project prompt, drawn once a frame beside the other modals
void import_prompt(App& app)
{
  if (app.pending_import.action == FileAction::kNone)
  {
    return;
  }
  if (!ImGui::IsPopupOpen("Copy into the project?"))
  {
    ImGui::OpenPopup("Copy into the project?");
  }
  if (!ImGui::BeginPopupModal("Copy into the project?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    return;
  }
  const PendingImport& pending = app.pending_import;
  const std::string_view name = project::file_name(pending.source.view());
  ImGui::Text("'%.*s' is outside the project.", static_cast<int>(name.size()), name.data());
  ImGui::TextWrapped("Keeping everything inside the project folder is what makes it portable.");
  ImGui::Spacing();
  // plan_copy has already worked out what copying would do, so this says it rather than guessing
  if (pending.plan.identical)
  {
    ImGui::TextWrapped("The same bytes are already at %s, which will be used.", pending.plan.destination.c_str());
  }
  else if (pending.plan.renamed)
  {
    ImGui::TextWrapped(
      "A different file of that name is already there, so the copy will be %s.", pending.plan.destination.c_str());
  }
  else
  {
    ImGui::TextWrapped("It will be copied to %s.", pending.plan.destination.c_str());
  }
  ImGui::Spacing();

  const FileAction action = pending.action;
  if (ImGui::Button("Copy into the project") or ImGui::IsKeyPressed(ImGuiKey_Enter))
  {
    const Status s =
      project::apply_copy(app.project->root.view(), pending.plan, pending.source.view(), app.memory.frame);
    // The relative path is what the project file stores, so a moved folder still works
    const Path stored = pending.plan.destination;
    app.pending_import = PendingImport{};
    ImGui::CloseCurrentPopup();
    if (s.ok)
    {
      apply_import(app, action, stored.view());
    }
    else
    {
      report(app, s);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Link in place"))
  {
    // Deliberately allowed: a reference may point outside on purpose, and the Project panel lists
    // every one of them with a button to copy it in later
    const Path outside = pending.source;
    app.pending_import = PendingImport{};
    ImGui::CloseCurrentPopup();
    apply_import(app, action, outside.view());
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") or ImGui::IsKeyPressed(ImGuiKey_Escape))
  {
    app.pending_import = PendingImport{};
    ImGui::CloseCurrentPopup();
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
  // The manifest first, and synchronously: a member level is written in project scope, so its file
  // means nothing without the manifest that holds the catalogs, tilesets and rules its cells name.
  // Writing the manifest before the level job starts also keeps the two out of each other's way.
  if (!app.project_path.empty())
  {
    file_action(app, FileAction::kSaveProjectAs, app.project_path.c_str());
  }
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
  else if (ctrl and io.KeyShift and ImGui::IsKeyPressed(ImGuiKey_A))
  {
    app.selection.clear();
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
    {ImGuiKey_T, Tool::kAutoTile},
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
      recent_files_menu(app);
      if (ImGui::MenuItem("Save", "Ctrl+S"))
      {
        save(app);
      }
      if (ImGui::MenuItem("Save as...", "Ctrl+Shift+S"))
      {
        open_file_dialog(app, FileAction::kSaveLevel);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("New project..."))
      {
        open_file_dialog(app, FileAction::kNewProject);
      }
      if (ImGui::MenuItem("Open project..."))
      {
        open_file_dialog(app, FileAction::kOpenProject);
      }
      if (ImGui::MenuItem("Save project", nullptr, false, !app.project_path.empty()))
      {
        file_action(app, FileAction::kSaveProjectAs, app.project_path.c_str());
      }
      if (ImGui::MenuItem("Save project as..."))
      {
        open_file_dialog(app, FileAction::kSaveProjectAs);
      }
      if (ImGui::MenuItem(
            "Compact catalogs",
            nullptr,
            false,
            project::needs_compaction(*app.project) and !app.project->levels.empty()))
      {
        if (auto* job = prepare_job<CompactCatalogsJob>(app); job != nullptr)
        {
          job->project = app.project;
          job->skip = app.level->settings.name;
          job->scratch = app.spare;
          job->scratch_project = app.spare_project;
          start_job(app);
        }
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
        if (ImGui::MenuItem("Tileset...", nullptr, false, app.tileset < app.project->tilesets.size()))
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
        if (ImGui::MenuItem("Selected layer as PNG...", nullptr, false, app.layer < app.level->layers.size()))
        {
          open_file_dialog(app, FileAction::kExportLayerImage);
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
      if (ImGui::MenuItem(
            format<64>("Deselect all tiles (%zu)", app.selection.count),
            "Ctrl+Shift+A",
            false,
            app.selection.count > 0))
      {
        app.selection.clear();
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
        level::derive_collision(app.project->tilesets, *app.level, c, view_season(app));
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
        level::derive_materials(app.project->tilesets, *app.level, m, view_season(app));
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
      ImGui::MenuItem("Project", nullptr, &app.panels.project);
      ImGui::MenuItem("Generate", nullptr, &app.panels.generate);
      ImGui::MenuItem("Tile adjacency rules", nullptr, &app.panels.adjacency);
      ImGui::MenuItem("Tile neighbors", nullptr, &app.panels.neighbors);
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
    const auto& seasons = app.project->seasons;
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
    // Started with nothing named, so the start-up prompt should be up. It is a modal, but it stops
    // nothing: the scripted session answers it the way a user would who wants no project yet, by
    // going on to make a level. The prompt is drawn and dismissed through the same code a click
    // takes, which is the point of checking it here rather than skipping it in scripted runs
    if (!app.welcome)
    {
      report(app, Status::error("the start-up prompt did not come up with nothing on the command line"));
    }
    demo_level(app);
    if (app.welcome)
    {
      report(app, Status::error("the start-up prompt stayed up after a level was made"));
    }
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
    // The adjacency rules window and the neighbor editor draw for a frame; corner rules are
    // derived from the sides
    app.panels.adjacency = true;
    app.panels.neighbors = true;
    rule_set(app).wfc.diagonals = true;
    level::derive_diagonals(rule_set(app).wfc, app.memory.frame);
    if (level::diagonal_count(rule_set(app).wfc) == 0)
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
    project::Project& pr = *app.project;
    const auto material = std::min<std::size_t>(project::intern_material(pr, "snow"), pr.materials.size() - 1);
    app.pending_delete =
      PendingDelete{.kind = DeleteKind::kMaterial, .index = material, .what = "material 'snow'", .confirmed = true};
    process_deletes(app);
    const auto semantic = project::intern_semantic(pr, "tunnel");
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
    // Removal retires the entry rather than erasing it, so the name is still there and the id
    // after it has not moved. It does not survive the level reopen in step 22: a standalone file is
    // the whole source of the project it is read into, and retirement is the manifest's to carry.
    if (!pr.materials[material].retired or !pr.semantics[semantic].retired)
    {
      report(app, Status::error("deleting a catalog entry did not retire it"));
    }
    if (!project::needs_compaction(pr))
    {
      report(app, Status::error("the project does not report that it has something to compact"));
    }
    break;
  }
  case 19: {
    // A batch metadata edit over a palette selection (which the tile panel then draws)
    level::TileSet& ts = app.project->tilesets[app.tileset];
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
    level::WfcRules& rules = rule_set(app).wfc;
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
    // Deselect all (Edit > Deselect all tiles, Ctrl+Shift+A): the batch sections go back to their hints
    app.selection.clear();
    if (selection_size(app) != 0 or !selected_rule_tiles(app, app.memory.frame).empty())
    {
      report(app, Status::error("deselecting all left tiles selected"));
    }
    break;
  }
  case 21: {
    // A multi-tile paint stamp: the selected tiles keep their relative positions in the palette
    app.selection.retarget(app.tileset);
    app.selection.clear();
    const level::Extents palette = palette_grid(app, app.tileset);
    if (palette.width < 2 or palette.height < 2)
    {
      report(app, Status::error("the demo palette is too small for a multi-tile stamp"));
      break;
    }
    const auto columns = static_cast<level::TileIndex>(palette.width);
    for (const level::TileIndex tile : {level::TileIndex{0}, level::TileIndex{1}, columns})
    {
      app.selection.set(tile, true);
    }
    level::Grid<level::TileRef> stamp{app.memory.frame};
    const level::Extents footprint = stamp_extents(app);
    if (!paint_stamp(app, stamp) or stamp.extents != level::Extents{2, 2} or footprint != stamp.extents)
    {
      report(app, Status::error("the palette selection did not become a 2x2 paint stamp"));
      break;
    }
    const auto tileset = static_cast<level::TileSetId>(app.tileset);
    if (
      stamp.at(0, 0) != level::TileRef{tileset, 0} or stamp.at(1, 0) != level::TileRef{tileset, 1} or
      stamp.at(0, 1) != level::TileRef{tileset, columns} or !stamp.at(1, 1).empty())
    {
      report(app, Status::error("the paint stamp does not hold the selection's layout"));
    }
    app.selection.clear();
    break;
  }
  case 22: {
    // The recent levels: the saves and the load above left their paths in the list, newest first,
    // and the newest one opens again from it (as File > Open recent does)
    load_recent_files(app);
    if (app.recent.count < 2 or app.recent.paths[0] != std::string_view{"smoke.lvlb"})
    {
      report(app, Status::error("the recent levels did not record the smoke test's files"));
      break;
    }
    const Path newest = app.recent.paths[0];
    file_action(app, FileAction::kOpenLevel, newest.c_str());
    break;
  }
  case 23: {
    // Auto-tiled painting from a terrain: the adjacency rules window's auto-tiling tab, a terrain
    // taken from a 3x3 palette block, and one painted cell laying down the whole nine-slice
    level::Level& lv = *app.level;
    project::Project& pr = *app.project;
    app.tool = Tool::kAutoTile;
    app.panels.adjacency = true;
    app.rules_mode = RulesMode::kAutoTile;
    const std::size_t layer = 1;
    if (layer >= lv.layers.size() or lv.layers[layer].kind != level::LayerKind::kTiles)
    {
      report(app, Status::error("the demo level has no tile layer to auto-tile"));
      break;
    }
    app.layer = layer;
    const level::Extents palette = palette_grid(app, app.tileset);
    if (palette.width < 3 or palette.height < 3)
    {
      report(app, Status::error("the demo palette is too small for a nine-slice"));
      break;
    }
    // The whole thirteen-tile set in one selection: the 3x3 nine-slice at the palette's top-left
    // corner, and a 2x2 block of inner corners beside it
    if (palette.width < 6)
    {
      report(app, Status::error("the demo palette is too narrow for a nine-slice and inner corners"));
      break;
    }
    app.selection.retarget(app.tileset);
    app.selection.clear();
    for (int y = 0; y < 3; ++y)
    {
      for (int x = 0; x < 3; ++x)
      {
        app.selection.set(static_cast<level::TileIndex>(y * palette.width + x), true);
      }
    }
    for (int y = 0; y < 2; ++y)
    {
      for (int x = 4; x < 6; ++x)
      {
        app.selection.set(static_cast<level::TileIndex>(y * palette.width + x), true);
      }
    }
    level::Grid<level::TileRef> layout{app.memory.frame};
    if (!palette_layout(app, layout) or layout.extents != level::Extents{6, 3})
    {
      report(app, Status::error("the palette selection is not the two blocks"));
      break;
    }
    level::AutoTerrain& terrain =
      rule_set(app).auto_tiles.terrains[add_terrain(rule_set(app).auto_tiles, *pr.arena, "demo_path")];
    app.terrain = level::find_terrain(rule_set(app).auto_tiles, "demo_path");
    if (level::learn_from_layout(terrain, layout) != 13)
    {
      report(app, Status::error("the terrain did not take all thirteen tiles of the palette selection"));
      break;
    }
    // The inner corners came from the 2x2 block, not the nine-slice
    const auto inner =
      static_cast<level::NeighborMask>(level::kAllNeighbors & ~level::mask_of(level::Direction::kSouthEast));
    if (
      level::resolve(terrain, inner) !=
      level::TileRef{static_cast<level::TileSetId>(app.tileset), static_cast<level::TileIndex>(palette.width + 5)})
    {
      report(app, Status::error("the inner corner did not come from the 2x2 block"));
      break;
    }
    app.selection.clear();

    auto& grid = level::plane<level::TileGrid>(lv.layers[layer]);
    const auto tileset = static_cast<level::TileSetId>(app.tileset);
    const auto background =
      level::TileRef{tileset, static_cast<level::TileIndex>(pr.tilesets[app.tileset].tiles.size() - 1)};
    grid.fill(background);
    app.paint.tile = level::resolve(terrain, level::kAllNeighbors);
    if (level::terrain_of(rule_set(app).auto_tiles, app.paint.tile) != app.terrain)
    {
      report(app, Status::error("the terrain's middle tile does not belong to it"));
      break;
    }
    const level::Cell painted{grid.width() / 2, grid.height() / 2};
    // Through the same entry point the Auto tool's strokes use, so the dispatch is covered too
    app.tool = Tool::kAutoTile;
    auto_tile_stroke(app, layer, &painted, 1);
    const AutoTileStatus& result = app.auto_tile_status;
    if (!result.terrain or result.name != terrain.name)
    {
      report(app, Status::error("the Auto tool did not paint with the tile's terrain"));
      break;
    }
    if (result.trouble != 0 or result.cells != 9)
    {
      report(app, Status::error("painting one cell of the terrain did not tile the ring around it"));
      break;
    }
    for (int y = 0; y < 3; ++y)
    {
      for (int x = 0; x < 3; ++x)
      {
        // The ring is the nine-slice block (the inner corners sit where the terrain wraps, which
        // one painted cell never does)
        if (grid.at(painted.x - 1 + x, painted.y - 1 + y) != layout.at(x, y))
        {
          report(app, Status::error("the terrain's border does not match the palette block it came from"));
        }
      }
    }
    // A tile of no terrain leaves the Auto tool with only the WFC fallback
    app.paint.tile = background;
    if (auto_tile_terrain(app) != level::kNoTerrain)
    {
      report(app, Status::error("the background tile should belong to no terrain"));
    }
    break;
  }
  case 24: {
    // A masked fill: the selected tiles scattered over the cells a height band lets through
    level::Level& lv = *app.level;
    const std::size_t tiles_layer = level::find_layer(lv, level::LayerKind::kTiles);
    const std::size_t height_layer = level::find_layer(lv, level::LayerKind::kHeight);
    if (tiles_layer == lv.layers.size() or height_layer == lv.layers.size())
    {
      report(app, Status::error("the smoke level has no tile or height layer to fill"));
      break;
    }
    const auto& height = level::plane<level::ScalarGrid>(lv.layers[height_layer]);
    auto& grid = level::plane<level::TileGrid>(lv.layers[tiles_layer]);
    float lo = height.values[0];
    float hi = height.values[0];
    for (const float v : height.values)
    {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (!(hi > lo))
    {
      report(app, Status::error("the smoke level's heightmap is flat, so a band cannot be tested"));
      break;
    }
    // The lower half of the range, so some cells are inside the band and some are not
    Generators& g = app.generators;
    g.fill_use_band = true;
    g.fill_use_id = false;
    g.fill_only_empty = false;
    g.fill_scatter = true;
    g.fill_region = false;
    g.fill_band_layer = height_layer;
    g.fill_lo = lo;
    g.fill_hi = lo + (hi - lo) * 0.5F;

    // Three tiles of the palette, as a ctrl+click selection
    app.selection.retarget(app.tileset);
    app.selection.clear();
    for (const level::TileIndex tile : {level::TileIndex{0}, level::TileIndex{1}, level::TileIndex{2}})
    {
      app.selection.set(tile, true);
    }
    const auto tileset = static_cast<level::TileSetId>(app.tileset);
    memory::Scope fill_scope{app.memory.frame};
    auto before = memory::make_vector<level::TileRef>(app.memory.frame);
    before.assign(grid.values.begin(), grid.values.end());
    const level::TileRef chosen[3] = {
      level::TileRef{tileset, 0}, level::TileRef{tileset, 1}, level::TileRef{tileset, 2}};
    apply_fill(app, tiles_layer, chosen, nullptr, 3);
    const procgen::FillResult result = g.fill_result;
    if (result.matched == 0 or result.matched == grid.values.size())
    {
      report(app, Status::error("the height band matched every cell or none, so it masked nothing"));
      break;
    }
    // Every cell in the band holds one of the three, and every cell outside it is untouched
    std::size_t seen[3] = {0, 0, 0};
    for (std::size_t i = 0; i < grid.values.size(); ++i)
    {
      const bool inside = (height.values[i] >= g.fill_lo and height.values[i] <= g.fill_hi);
      if (!inside)
      {
        if (grid.values[i] != before[i])
        {
          report(app, Status::error("the fill wrote outside its height band"));
          break;
        }
        continue;
      }
      std::size_t which = 3;
      for (std::size_t t = 0; t < 3; ++t)
      {
        which = (grid.values[i] == chosen[t]) ? t : which;
      }
      if (which == 3)
      {
        report(app, Status::error("a cell inside the height band did not take one of the selected tiles"));
        break;
      }
      ++seen[which];
    }
    if (seen[0] == 0 or seen[1] == 0 or seen[2] == 0)
    {
      report(app, Status::error("the fill did not scatter all three of the selected tiles"));
      break;
    }
    // Running it again writes the same tiles, so the scatter stays put
    apply_fill(app, tiles_layer, chosen, nullptr, 3);
    if (g.fill_result.changed != 0)
    {
      report(app, Status::error("filling again moved the scattered tiles"));
    }
    // Weights change the shares: the first tile is asked for four times as much as the others
    const float weights[3] = {4.0F, 1.0F, 1.0F};
    apply_fill(app, tiles_layer, chosen, weights, 3);
    std::size_t weighted[3] = {0, 0, 0};
    for (std::size_t i = 0; i < grid.values.size(); ++i)
    {
      if (height.values[i] < g.fill_lo or height.values[i] > g.fill_hi)
      {
        continue;
      }
      for (std::size_t t = 0; t < 3; ++t)
      {
        weighted[t] += (grid.values[i] == chosen[t]) ? 1U : 0U;
      }
    }
    if (weighted[0] + weighted[1] + weighted[2] != result.matched)
    {
      report(app, Status::error("a weighted fill left cells of the band without one of the selected tiles"));
      break;
    }
    if (weighted[0] <= weighted[1] + weighted[2] or weighted[1] == 0 or weighted[2] == 0)
    {
      report(app, Status::error("the scatter weights did not change the shares"));
      break;
    }
    // A weight of zero keeps a tile out altogether
    const float without[3] = {1.0F, 0.0F, 1.0F};
    apply_fill(app, tiles_layer, chosen, without, 3);
    for (std::size_t i = 0; i < grid.values.size(); ++i)
    {
      const bool inside = (height.values[i] >= g.fill_lo and height.values[i] <= g.fill_hi);
      if (inside and grid.values[i] == chosen[1])
      {
        report(app, Status::error("a tile of weight zero was still scattered"));
        break;
      }
    }

    // With the scatter off, one tile covers the band instead
    g.fill_scatter = false;
    apply_fill(app, tiles_layer, chosen, nullptr, 1);
    for (std::size_t i = 0; i < grid.values.size(); ++i)
    {
      const bool inside = (height.values[i] >= g.fill_lo and height.values[i] <= g.fill_hi);
      if (inside and grid.values[i] != chosen[0])
      {
        report(app, Status::error("an unscattered fill did not use the first selected tile"));
        break;
      }
    }
    app.selection.clear();
    break;
  }
  case 25: {
    // Height steps: the heightmap cut into levels, tidied, and the cliffs between them drawn --
    // each terrace onto its own tile layer, since a cliff tile is transparent where the terrace
    // below shows through it
    level::Level& lv = *app.level;
    Generators& g = app.generators;
    const std::size_t height_layer = level::find_layer(lv, level::LayerKind::kHeight);
    const std::size_t collision_layer = level::find_layer(lv, level::LayerKind::kCollision);
    if (height_layer == lv.layers.size() or collision_layer == lv.layers.size())
    {
      report(app, Status::error("the smoke level is missing a layer the height steps need"));
      break;
    }
    const std::size_t level_layer = level::add_layer(lv, level::LayerKind::kLevel, "levels");

    // A step terrain per terrace, whose tiles are the step type itself, so the drawing can be read
    app.rules_mode = RulesMode::kSteps;
    rule_set(app).steps.terrains.clear();
    const auto tileset = static_cast<level::TileSetId>(app.tileset);
    static constexpr int kBands = 3;
    // One terrain covering every level: the ordinary case, where every terrace is drawn in the
    // same eighteen tiles and it is the levels, not the terrains, that make the layers
    {
      level::StepTerrain& terrain =
        rule_set(app).steps.terrains[level::add_step_terrain(rule_set(app).steps, "ground")];
      terrain.level_lo = 0;
      terrain.level_hi = 255;
      // Through the side-by-side block, so the layout the palette offers is covered too
      level::Grid<level::TileRef> block{app.memory.frame};
      block.assign(level::Extents{level::kSideBySideWidth, level::kSideBySideHeight}, level::kEmptyTile);
      for (int by = 0; by < level::kSideBySideHeight; ++by)
      {
        for (int bx = 0; bx < level::kSideBySideWidth; ++bx)
        {
          const std::size_t type = level::step_type_at(level::StepLayout::kSideBySide, bx, by);
          block.at(bx, by) = level::TileRef{tileset, static_cast<level::TileIndex>(type)};
        }
      }
      if (level::learn_step_layout(terrain, block) != level::kStepTypeCount)
      {
        report(app, Status::error("the side-by-side step block was not taken"));
      }
      for (std::size_t i = 0; i < level::kStepTypeCount; ++i)
      {
        if (terrain.tiles[i] != level::TileRef{tileset, static_cast<level::TileIndex>(i)})
        {
          report(app, Status::error("the side-by-side block put a tile in the wrong slot"));
          break;
        }
      }
    }
    app.step_terrain = 0;

    g.step_height_layer = height_layer;
    g.step_level_layer = level_layer;
    g.step_collision_layer = collision_layer;
    // A skew below 1, so the bands thin out towards the top as well
    g.terrace = procgen::TerraceParams{.levels = kBands, .skew = 0.75F};
    g.step_collision = procgen::StepCollision::kBelow;
    g.step_tile_after_terrace = true;
    g.step_tidy = true;
    g.tidy = procgen::TidyParams{};
    // Cutting the levels adds a layer per level and draws them, all from the one call
    apply_terrace(app);
    for (int band = 0; band < kBands; ++band)
    {
      if (step_layer_of(lv, band) >= lv.layers.size())
      {
        report(app, Status::error("a height level was left without a layer"));
      }
    }
    if (g.terrace_result.top != kBands - 1)
    {
      report(app, Status::error("cutting the heightmap did not produce the levels asked for"));
      break;
    }
    if (!(g.terrace_result.top_rate < g.terrace_result.rate))
    {
      report(app, Status::error("the skew did not make the upper bands thinner"));
      break;
    }
    if (!g.tidy_result.stable or g.tidy_result.changed == 0)
    {
      report(app, Status::error("tidying the levels did not settle, or found nothing to square off"));
      break;
    }
    if (g.step_result.missing != 0 or g.step_result.unexpressed != 0 or g.step_result.untermed != 0)
    {
      report(app, Status::error("drawing the steps left shapes it could not draw"));
      break;
    }

    const auto& levels = level::plane<level::IdGrid>(lv.layers[level_layer]);
    const auto& mask = level::plane<level::MaskGrid>(lv.layers[collision_layer]);
    // Each terrace's layer holds its own ground and nothing else, and the tile of every cell is
    // the one its own terrace's shape calls for
    std::size_t cliffs = 0;
    bool ok = true;
    for (int band = 0; band < kBands and ok; ++band)
    {
      const std::size_t at = step_layer_of(lv, band);
      const auto& grid = level::plane<level::TileGrid>(lv.layers[at]);
      for (int y = 0; y < levels.extents.height and ok; ++y)
      {
        for (int x = 0; x < levels.extents.width and ok; ++x)
        {
          const bool inside = static_cast<int>(levels.at(x, y)) >= band;
          if (!inside)
          {
            if (!grid.at(x, y).empty())
            {
              report(app, Status::error("a terrace's layer holds a tile below its own ground"));
              ok = false;
            }
            continue;
          }
          level::StepCode code = 0;
          for (std::size_t n = 0; n < level::kStepNeighbors.size(); ++n)
          {
            const level::Cell c = level::step(level::Cell{x, y}, level::kStepNeighbors[n]);
            const bool below = levels.extents.contains(c) and static_cast<int>(levels.at(c.x, c.y)) < band;
            code = level::step_code_with(code, n, below ? level::StepRelation::kBelow : level::StepRelation::kLevel);
          }
          const auto expected = static_cast<level::TileIndex>(level::step_type_of(code));
          if (grid.at(x, y) != level::TileRef{tileset, expected})
          {
            report(app, Status::error("a cell did not take the tile its terrace's shape calls for"));
            ok = false;
          }
        }
      }
    }
    if (!ok)
    {
      break;
    }
    // The collision mask stays one flat layer over the whole map, marking the foot of every step
    for (int y = 0; y < levels.extents.height; ++y)
    {
      for (int x = 0; x < levels.extents.width; ++x)
      {
        const int here = static_cast<int>(levels.at(x, y));
        bool above = false;
        for (const level::Direction d : level::kStepNeighbors)
        {
          const level::Cell c = level::step(level::Cell{x, y}, d);
          above = above or (levels.extents.contains(c) and static_cast<int>(levels.at(c.x, c.y)) > here);
        }
        if (((mask.at(x, y) & level::collision::kSolid) != 0) != above)
        {
          report(app, Status::error("the cells blocked are not the ones at the foot of a step"));
          ok = false;
          break;
        }
        cliffs += above ? 1U : 0U;
      }
    }
    if (!ok)
    {
      break;
    }
    if (cliffs == 0)
    {
      report(app, Status::error("the terraced heightmap has no steps at all"));
      break;
    }
    // Drawing again changes nothing
    apply_steps(app);
    if (g.step_result.changed != 0)
    {
      report(app, Status::error("drawing the steps again moved tiles"));
    }
    app.rules_mode = RulesMode::kWfc;
    break;
  }
  case 26: {
    // Portals name a level the project knows, and the index is what answers for one that is not
    // loaded. The saves above made the open level a member, so its own portals resolve from the
    // live registry and a name no member has does not.
    project::Project& pr = *app.project;
    level::Level& lv = *app.level;
    if (project::find_level(pr, lv.settings.name.view()) == project::kNoLevel)
    {
      report(app, Status::error("saving the level did not make it a member of its project"));
    }
    const level::Entity door = level::create_portal(lv.registry, "ring_road", {1, 1}, {1, 1}, level::Portal{});
    auto& portal = lv.registry.get<level::Portal>(door);
    // This level, and a portal of it that really is there
    portal.target_portal.assign("ring_road");
    if (portal_link_of(app, portal) != project::LinkState::kSelf)
    {
      report(app, Status::error("a portal pointing at this level's own portal did not resolve"));
    }
    // A level no member is named after
    portal.target_level.assign("nowhere_at_all");
    if (portal_link_of(app, portal) != project::LinkState::kUnknownLevel)
    {
      report(app, Status::error("a portal naming no member was not reported as dangling"));
    }
    if (*link_message(project::LinkState::kUnknownLevel, portal) == '\0')
    {
      report(app, Status::error("a dangling portal has nothing to say about what is missing"));
    }
    // And a member that has no portal of that name
    portal.target_level = lv.settings.name;
    portal.target_portal.assign("not_a_portal");
    if (portal_link_of(app, portal) != project::LinkState::kUnknownPortal)
    {
      report(app, Status::error("a portal naming a portal that is not there was not reported"));
    }
    app.pending_delete =
      PendingDelete{.kind = DeleteKind::kObject, .entity = door, .what = "portal", .confirmed = true};
    process_deletes(app);
    break;
  }
  case 27: {
    // Removing a catalog entry retires it: the id stays reserved because the project's other levels
    // are not loaded. Compacting is the deliberate act that renumbers every level at once, and the
    // only thing that moves a catalog id. This is the panel's own path, with one member level.
    //
    // The entry is retired here rather than relying on the ones step 14 removed: step 22 reopened a
    // level, and a standalone level file is the whole source of the project it is read into, so it
    // brings no retirement with it. Retirement is the manifest's to carry, and there is no manifest
    // until step 28.
    project::Project& pr = *app.project;
    level::Level& lv = *app.level;
    const auto doomed = project::intern_semantic(pr, "scaffold");
    project::remove_semantic(pr, lv, doomed);
    if (!project::needs_compaction(pr))
    {
      report(app, Status::error("removing a catalog entry did not retire it"));
    }
    std::size_t retired = 0;
    for (const auto& e : pr.semantics)
    {
      retired += e.retired ? 1U : 0U;
    }
    for (const auto& e : pr.materials)
    {
      retired += e.retired ? 1U : 0U;
    }
    const std::size_t before = pr.semantics.size() + pr.materials.size();
    project::CatalogPlan plan{app.memory.frame};
    project::plan_compaction(pr, plan);
    project::apply_compaction(plan, lv);
    project::finish_compaction(pr, plan);
    app.undo.clear();
    invalidate_view(app);
    if (pr.semantics.size() + pr.materials.size() + retired != before)
    {
      report(app, Status::error("compacting dropped the wrong number of catalog entries"));
    }
    if (project::needs_compaction(pr))
    {
      report(app, Status::error("compacting left a retired entry behind"));
    }
    // And the entry itself is gone, by name: the count above would be satisfied by dropping the
    // wrong one
    for (const auto& e : pr.semantics)
    {
      if (e.name == std::string_view{"scaffold"})
      {
        report(app, Status::error("the retired semantic type survived compacting"));
      }
    }
    break;
  }
  case 28:
    // A project on disk: the manifest goes where the user puts it, and that folder is the root
    file_action(app, FileAction::kNewProject, "smoke_project/project.json");
    break;
  case 29: {
    project::Project& pr = *app.project;
    if (app.project_path.empty() or pr.root.empty())
    {
      report(app, Status::error("creating a project did not record where it is"));
    }
    if (!project::is_project_root(pr.root.view()))
    {
      report(app, Status::error("creating a project did not write its manifest"));
    }
    // The folder tree is not checked here: file::exists is for regular files, as it says, and
    // //project:project_test checks ensure_tree properly with is_directory. The save below is the
    // evidence that matters anyway -- it writes into levels/, which only works if the tree is there.
    // Into the project's own levels/ folder, which is what makes it a member
    file_action(app, FileAction::kSaveLevel, "smoke_project/levels/demo.json");
    break;
  }
  case 30: {
    project::Project& pr = *app.project;
    level::Level& lv = *app.level;
    if (project::find_level(pr, lv.settings.name.view()) == project::kNoLevel)
    {
      report(app, Status::error("saving into a project did not make the level a member"));
    }
    // A member is written in project scope, so its file carries none of the shared sections
    memory::Scope scope{app.memory.frame};
    char* text = nullptr;
    std::size_t size = 0;
    if (file::read(app.path.c_str(), app.memory.frame, text, size).ok)
    {
      if (std::string_view{text, size}.find("\n  \"tilesets\"") != std::string_view::npos)
      {
        report(app, Status::error("a member level was saved standalone, not in project scope"));
      }
    }
    // A second member: the same level saved under another name, which is what the index follows
    lv.settings.name.assign("east");
    file_action(app, FileAction::kSaveLevel, "smoke_project/levels/east.json");
    break;
  }
  case 31: {
    project::Project& pr = *app.project;
    if (pr.levels.size() != 2)
    {
      report(app, Status::error("saving under a new name did not add a second member"));
    }
    // A portal of the open level pointing at the other member resolves from the index alone --
    // the other level is not loaded, and will not be
    const std::size_t other = (project::find_level(pr, "east") == 0) ? 1 : 0;
    const Name target = pr.levels[other].name;
    const level::Entity door =
      level::create_portal(app.level->registry, "west_gate", {0, 0}, {1, 1}, level::Portal{.target_level = target});
    auto& portal = app.level->registry.get<level::Portal>(door);
    if (pr.levels[other].portal_count == 0)
    {
      report(app, Status::error("the other member has no portal names in the index"));
    }
    portal.target_portal = project::portals_of(pr, pr.levels[other])[0];
    if (portal_link_of(app, portal) != project::LinkState::kResolved)
    {
      report(app, Status::error("a portal into a sibling level did not resolve from the index"));
    }
    app.pending_delete =
      PendingDelete{.kind = DeleteKind::kObject, .entity = door, .what = "portal", .confirmed = true};
    process_deletes(app);
    // Switch to that sibling. Nothing is unsaved, so it goes straight to the load.
    app.modified = false;
    app.smoke_check.tilesets = pr.tilesets.size();
    app.smoke_check.assets = app.assets.current;
    request_switch(app, other);
    break;
  }
  case 32: {
    project::Project& pr = *app.project;
    if (app.level->settings.name == std::string_view{"east"})
    {
      report(app, Status::error("switching level did not replace the open document"));
    }
    // A member carries none of the shared sections, so the project is untouched and its atlases
    // are not decoded again -- which is most of the point of the project owning the tilesets
    if (pr.tilesets.size() != app.smoke_check.tilesets)
    {
      report(app, Status::error("switching level replaced the project's tilesets"));
    }
    if (app.assets.current != app.smoke_check.assets)
    {
      report(app, Status::error("switching level decoded the atlases again"));
    }
    // Something to compact, now that the project has two members and the job can do it
    const auto doomed = project::intern_semantic(pr, "scaffold_again");
    project::remove_semantic(pr, *app.level, doomed);
    if (!project::needs_compaction(pr))
    {
      report(app, Status::error("removing a catalog entry did not leave anything to compact"));
    }
    app.smoke_check.semantics = pr.semantics.size();
    if (auto* job = prepare_job<CompactCatalogsJob>(app); job != nullptr)
    {
      job->project = app.project;
      job->skip = app.level->settings.name;
      job->scratch = app.spare;
      job->scratch_project = app.spare_project;
      start_job(app);
    }
    break;
  }
  case 33: {
    project::Project& pr = *app.project;
    if (project::needs_compaction(pr))
    {
      report(app, Status::error("the compaction job left a retired entry behind"));
    }
    if (pr.semantics.size() != app.smoke_check.semantics - 1)
    {
      report(app, Status::error("the compaction job dropped the wrong number of entries"));
    }
    // By name as well: the count alone would be satisfied by dropping the wrong entry
    for (const auto& e : pr.semantics)
    {
      if (e.name == std::string_view{"scaffold_again"})
      {
        report(app, Status::error("the entry the compaction job was meant to drop is still there"));
      }
    }
    // Rescan: read every member again and rebuild what the index says
    if (auto* job = prepare_job<RescanProjectJob>(app); job != nullptr)
    {
      job->project = app.project;
      job->scratch = app.spare;
      job->scratch_project = app.spare_project;
      start_job(app);
    }
    break;
  }
  case 34: {
    const project::Project& pr = *app.project;
    for (const project::LevelEntry& entry : pr.levels)
    {
      if (entry.missing)
      {
        report(app, Status::error(format<160>("the rescan could not read '%s'", entry.file.c_str())));
      }
      if (entry.extents.width <= 0)
      {
        report(app, Status::error(format<160>("the rescan left '%s' without a size", entry.name.c_str())));
      }
    }
    // Ctrl+S: the manifest first and synchronously, then the level, so what is on disk is what the
    // rescan and the compaction left
    save(app);
    break;
  }
  case 35:
    // And open it again: the manifest, its tilesets and rule sets, then its first member
    file_action(app, FileAction::kOpenProject, "smoke_project/project.json");
    break;
  case 36: {
    const project::Project& pr = *app.project;
    if (pr.levels.size() != 2)
    {
      report(app, Status::error("reopening the project did not bring both levels back"));
    }
    if (pr.tilesets.empty())
    {
      report(app, Status::error("reopening the project did not bring its tilesets back"));
    }
    if (app.project_modified)
    {
      report(app, Status::error("a project just opened already reports unsaved changes"));
    }
    // And it is in the recent list, so it can be reopened as a project. Not necessarily at the
    // top: the member level it went on to load was touched after it, and the list is ordered by
    // what was touched last.
    bool listed = false;
    for (std::size_t i = 0; i < app.recent.count; ++i)
    {
      listed = listed or app.recent.paths[i] == app.project_path;
    }
    if (!listed)
    {
      report(app, Status::error("opening a project did not record it in the recent list"));
    }
    // A layer as a PNG, one pixel per cell, into the project's own artifacts folder
    file_action(app, FileAction::kExportLayerImage, "smoke_project/artifacts/demo.layer.png");
    break;
  }
  case 37: {
    if (!file::exists("smoke_project/artifacts/demo.layer.png"))
    {
      report(app, Status::error("exporting a layer as a PNG wrote nothing"));
    }
    // An asset from outside the project asks before anything is stored, and says what copying
    // would do. The demo level wrote its atlas into the working directory, which is outside.
    project::Project& pr = *app.project;
    file_action(app, FileAction::kAtlasImage, "lvl_demo_atlas.png");
    if (app.pending_import.action != FileAction::kAtlasImage)
    {
      report(app, Status::error("choosing an atlas from outside the project did not ask first"));
    }
    const project::CopyPlan plan = app.pending_import.plan;
    const Path source = app.pending_import.source;
    if (plan.inside)
    {
      report(app, Status::error("a file outside the project was planned as already inside it"));
    }
    if (plan.destination != std::string_view{"assets/atlases/lvl_demo_atlas.png"})
    {
      report(app, Status::error(format<200>("the copy would land at '%s'", plan.destination.c_str())));
    }
    // What the prompt's "Copy into the project" button does
    app.pending_import = PendingImport{};
    if (const Status copied = project::apply_copy(pr.root.view(), plan, source.view(), app.memory.frame); !copied.ok)
    {
      report(app, copied);
    }
    Path absolute;
    if (project::resolve(pr.root.view(), plan.destination.view(), absolute).ok and !file::exists(absolute.c_str()))
    {
      report(app, Status::error("the atlas was not copied into the project"));
    }
    // And now that it is inside, choosing it asks nothing and is stored relative to the root --
    // which is what makes the folder portable
    file_action(app, FileAction::kAtlasImage, absolute.c_str());
    if (app.pending_import.action != FileAction::kNone)
    {
      report(app, Status::error("a file already inside the project asked anyway"));
    }
    if (pr.tilesets[app.tileset].image != plan.destination)
    {
      report(
        app,
        Status::error(
          format<200>("an atlas inside the project was stored as '%s'", pr.tilesets[app.tileset].image.c_str())));
    }
    break;
  }
  case 38:
    // The atlas just copied in is stored relative to the project root, and the reload that followed
    // had to resolve it against that root rather than against the working directory. An atlas that
    // will not load only draws red in the Tilesets panel, which is why this is checked here
    if (app.tileset < app.project->tilesets.size() and !app.assets.status[app.tileset].ok)
    {
      report(app, app.assets.status[app.tileset]);
    }
    // Each kind of file opens where it belongs in the project tree, which there now is one of.
    // The dialog has to draw a frame before it reports a folder, so this takes a step each.
    open_file_dialog(app, FileAction::kSaveLevel);
    break;
  case 39: {
    // The level's own file is in levels/ already, but the point is that the dialog says so rather
    // than offering wherever it was last used
    const Path directory = file_dialog_directory();
    if (project::file_name(directory.view()) != std::string_view{"levels"})
    {
      report(app, Status::error(format<200>("saving a level offered '%s'", directory.c_str())));
    }
    close_file_dialog(app);
    open_file_dialog(app, FileAction::kSaveTileSet);
    break;
  }
  case 40: {
    const Path directory = file_dialog_directory();
    if (project::file_name(directory.view()) != std::string_view{"tilesets"})
    {
      report(app, Status::error(format<200>("exporting a tileset offered '%s'", directory.c_str())));
    }
    close_file_dialog(app);
    break;
  }
  case 41:
    // Quitting with unsaved changes asks first; Cancel keeps the editor open
    app.modified = true;
    request_exit(app);
    if (app.exit or !app.confirm_exit)
    {
      report(app, Status::error("quitting with unsaved changes did not ask first"));
    }
    break;
  case 42:
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
  return (app.view.season < app.project->seasons.size()) ? app.project->seasons[app.view.season].name.view()
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
  // The new level shares nothing with the old one, so its project starts empty too
  project::reset(*app.project);
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
    job->project = app.project;
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
  app.project = new (app.project_storage[0]) project::Project{app.memory.projects[0]};
  app.spare_project = new (app.project_storage[1]) project::Project{app.memory.projects[1]};
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
    // One positional argument, three things it can be: a project folder, a project manifest, or a
    // level. A project is where its manifest is, so "is this a project?" is one existence check
    // either way, and opening one goes on to load its first member.
    Path manifest;
    if (
      project::is_project_root(std::string_view{path}) and
      project::join(std::string_view{path}, project::kManifestName, manifest).ok)
    {
      file_action(app, FileAction::kOpenProject, manifest.c_str());
    }
    else if (project::file_name(std::string_view{path}) == project::kManifestName)
    {
      file_action(app, FileAction::kOpenProject, path);
    }
    else
    {
      file_action(app, FileAction::kOpenLevel, path);
    }
  }
  else
  {
    // Nothing named on the command line, so ask what to work on (welcome_prompt)
    app.welcome = true;
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
  app.project->~Project();
  app.spare_project->~Project();
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
  // Either flag arms the browser's "leave site?" prompt, as either arms the editor's own
  web::set_unsaved_changes(app.modified or app.project_modified);
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
  if (app.panels.project)
  {
    project_panel(app);
  }
  if (app.panels.generate)
  {
    generate_panel(app);
  }
  if (app.panels.adjacency)
  {
    adjacency_window(app);
  }
  if (app.panels.neighbors)
  {
    neighbors_window(app);
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
  welcome_prompt(app);
  exit_prompt(app);
  import_prompt(app);
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

void copy_asset_into_project(App& app, FileAction action, std::string_view path)
{
  // request_import does the asking; this exists so the Project panel can reach it, since the panel
  // is a different translation unit and the prompt's own helpers are file-local
  const Path source{path};
  request_import(app, action, source.c_str());
}

void request_switch(App& app, std::size_t index)
{
  if (!app.modified)
  {
    switch_to_level(app, index);
    return;
  }
  // The same prompt quitting uses: the open document is about to be replaced either way
  app.switch_to = index;
  app.confirm_exit = true;
}

void request_exit(App& app, bool force)
{
  if (force or (!app.modified and !app.project_modified))
  {
    app.exit = true;
    return;
  }
  app.confirm_exit = true;
}

void save_and_switch(App& app)
{
  app.confirm_exit = false;
  app.switch_after_save = true;
  if (app.path.empty())
  {
    // Switches when the dialog's save succeeds; cancelling the dialog cancels switching
    open_file_dialog(app, FileAction::kSaveLevel);
  }
  else
  {
    file_action(app, FileAction::kSaveLevel, app.path.c_str());
  }
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
  case FileAction::kNewProject:
    // Synchronous: a manifest and the files it names are kilobytes, never on a hot path. What needs
    // a job is loading a level, and nothing is loaded here -- a new project has no members.
    small_io([&](level::IoContext& ctx) -> Status {
      const std::string_view folder = project::parent_path(std::string_view{path});
      const Path root{folder.empty() ? std::string_view{"."} : folder};
      LVL_RETURN_IF_ERROR(project::ensure_tree(root.view()));
      // The project the open level already draws on gets a home on disk; it is deliberately not
      // replaced by an empty one (which is what project::create_project does, for a caller that
      // wants that). Making a project out of the level you have open must not throw away its
      // tilesets, its catalogs or its rules: the level's cells name them.
      if (app.project->name.empty() or app.project->name == std::string_view{"untitled"})
      {
        app.project->name.assign(project::file_name(root.view()));
      }
      const Status s = project::save_project(path, *app.project, ctx);
      if (s.ok)
      {
        app.project_path.assign(path);
        app.project_modified = false;
        // A project is what you reopen, so it goes in the recent list beside the levels
        remember_recent_file(app, app.project_path.view());
        // The open level is not a member of it until it is saved into the tree
        app.modified = true;
      }
      return s;
    });
    break;
  case FileAction::kOpenProject:
    small_io([&](level::IoContext& ctx) {
      // Into the spare, so a manifest that will not load leaves the open project alone
      Status s = project::load_project(path, *app.spare_project, ctx);
      if (!s.ok)
      {
        project::reset(*app.spare_project);
        return s;
      }
      std::swap(app.project, app.spare_project);
      project::reset(*app.spare_project);
      app.project_path.assign(path);
      app.project_modified = false;
      app.tileset = 0;
      remember_recent_file(app, app.project_path.view());
      invalidate_view(app);
      // The project's own first member replaces whatever level is open, and brings the new
      // tilesets' atlases in with it -- one job, since only one runs at a time. A project with no
      // members leaves the open level where it is, and then the atlases need a job of their own.
      if (app.project->levels.empty())
      {
        reload_atlases(app);
      }
      else
      {
        switch_to_level(app, 0, true);
      }
      return s;
    });
    break;
  case FileAction::kSaveProjectAs:
    small_io([&](level::IoContext& ctx) {
      Status s = project::save_project(path, *app.project, ctx);
      if (s.ok)
      {
        app.project_path.assign(path);
        app.project_modified = false;
      }
      return s;
    });
    break;
  case FileAction::kAddLevel:
    small_io([&](level::IoContext& ctx) {
      // Read its name and objects without disturbing the open level, then record it as a member.
      // Its file stays where it is: copying levels into the tree is phase 5's business.
      level::reset(*app.spare);
      project::reset(*app.spare_project);
      Status s = level::load_level(path, *app.spare_project, *app.spare, ctx);
      if (!s.ok)
      {
        return s;
      }
      s = add_member(app, app.spare->settings.name.view(), path, *app.spare);
      level::reset(*app.spare);
      project::reset(*app.spare_project);
      return s;
    });
    break;
  case FileAction::kOpenLevel:
    if (auto* job = prepare_job<LoadLevelJob>(app); job != nullptr)
    {
      job->project = app.spare_project;
      job->level = app.spare;
      job->assets = &app.memory.assets[1 - app.assets.current];
      job->path.assign(path);
      start_job(app);
    }
    break;
  case FileAction::kSaveLevel:
    if (auto* job = prepare_job<SaveLevelJob>(app); job != nullptr)
    {
      job->project = app.project;
      job->level = app.level;
      job->path.assign(path);
      // A level inside a project is a member of it; one with no project folder stands alone
      job->scope = app.project_path.empty() ? level::LevelScope::kStandalone : level::LevelScope::kProject;
      start_job(app);
    }
    break;
  case FileAction::kLoadTileSet:
    small_io([&](level::IoContext& ctx) {
      level::TileSetId id = level::kNoTileSet;
      Status s = level::load_tileset(path, *app.project, ctx, id);
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
      Status s = level::save_tileset(path, *app.project, static_cast<level::TileSetId>(app.tileset), ctx);
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
      // The file names the set it holds, so it may be a new one; the open level switches to it,
      // since importing rules is how you say what this level should tile with
      std::size_t index = project::kNoRules;
      const Status s = level::load_wfc_rules(path, *app.project, ctx, index);
      if (s.ok)
      {
        app.level->rules = app.project->rules[index].wfc.name;
      }
      return s;
    });
    break;
  case FileAction::kSaveWfc:
    small_io([&](level::IoContext& ctx) {
      Status s = level::save_wfc_rules(path, *app.project, project::rules_of(*app.project, *app.level), ctx);
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
  case FileAction::kSeedImage:
    // Everything a project needs should live inside its folder, so a file from outside asks first
    // (see import_prompt). One already inside, or with no project to be inside of, is taken as is.
    request_import(app, action, path);
    break;
  case FileAction::kExportPng:
  case FileAction::kExportMinimap:
  case FileAction::kExportLayerImage:
    if (auto* job = prepare_job<ExportJob>(app); job != nullptr)
    {
      job->project = app.project;
      job->level = app.level;
      job->atlases = app.assets.atlases(app.project->tilesets.size());
      job->options = app.export_options;
      job->options.season = view_season(app);
      job->options.tint = app.view.tint;
      job->minimap = (action == FileAction::kExportMinimap);
      if (action == FileAction::kExportLayerImage)
      {
        job->layer = app.layer;
      }
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
  // --demo says what to work on, which is what the start-up prompt would otherwise be asking
  app.welcome = false;
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
  project::Project& pr = *app.project;
  pr.seasons[0] = Season{.name = "summer", .duration_days = 90, .temperature = 22.0F, .precipitation = 1.5F};
  pr.seasons.push_back(Season{
    .name = "autumn", .duration_days = 60, .temperature = 10.0F, .precipitation = 4.0F, .tint = {255, 225, 190, 255}});
  pr.seasons.push_back(Season{
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
    pr.semantics[project::intern_semantic(pr, semantic_names[i])].color = semantic_colors[i];
  }
  const char* material_names[] = {"water", "sand", "grass", "dirt", "stone", "snow", "ice", "wood"};
  for (const char* m : material_names)
  {
    pr.materials[project::intern_material(pr, m)].color = render::id_color(project::intern_material(pr, m) * 7U);
  }
  pr.materials[project::intern_material(pr, "ice")].friction = 0.15F;
  pr.materials[project::intern_material(pr, "snow")].speed = 0.7F;

  const TileSetId ts_id = project::add_tileset(pr, "demo");
  TileSet& ts = pr.tilesets[ts_id];
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
    ts.tiles[t].semantic = project::intern_semantic(pr, tile_semantics[t]);
    ts.tiles[t].material = project::intern_material(pr, tile_materials[t]);
    ts.tiles[8 + t].name.assign(format<64>("%s_winter", tile_names[t]));
    ts.tiles[8 + t].semantic = ts.tiles[t].semantic;
    ts.tiles[8 + t].material = project::intern_material(pr, t < 2 ? "ice" : "snow");
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
  ts.tiles[16].semantic = project::intern_semantic(pr, "road");
  ts.tiles[16].material = project::intern_material(pr, "dirt");
  ts.tiles[17].name.assign("wall");
  ts.tiles[17].collision = collision::kSolid | collision::kSightBlocker;
  ts.tiles[17].material = project::intern_material(pr, "stone");
  ts.tiles[18].name.assign("door");
  ts.tiles[18].collision = collision::kTrigger;
  ts.tiles[19].name.assign("bridge");
  ts.tiles[19].material = project::intern_material(pr, "wood");
  ts.properties.push_back(TileProperty{.tile = 18, .key = "sound", .value = "door_open"});
  for (TileIndex t = 20; t < ts.tiles.size(); ++t)
  {
    ts.tiles[t].hidden = true;
  }

  // WFC rules: elevation bands; neighbors only one band apart (meadow/grass/forest interchangeable)
  WfcRules& rules = rule_set(app).wfc;
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
    // A level is named, not pathed: this is a destination the project's index would resolve
    Portal{.target_level = "east", .target_portal = "west_entry", .exit = Direction::kWest});
  create_marker(lv.registry, "spawn", "player", {21.0F, 31.0F});
  app.layer = 1;
  app.generators.height_layer = 0;
  reload_atlases(app);
  LVL_LOG_INFO() << "Created demo level (atlas written to '" << atlas_path << "')";
}

}  // namespace lvl::editor
