/**
 * @copyright 2026-present Brian Cairl
 *
 * @file internal.hpp
 *
 * Declarations shared by the editor's UI translation units.
 */
#pragma once

// C++ Standard Library
#include <string_view>

// Dear ImGui
#include <imgui.h>

// LVL
#include "lvl/editor/app.hpp"

namespace lvl::editor
{

/// True while a background job runs. Jobs read the document without locks, so panels that could
/// write to it (even unchanged values) must not run their bodies then.
inline bool busy(const App& app) { return !std::holds_alternative<std::monostate>(app.job); }

/// Ends a panel early while busy (keeps its window, so the dock layout doesn't change); returns true if so
inline bool skip_while_busy(const App& app)
{
  if (!busy(app))
  {
    return false;
  }
  ImGui::TextDisabled("Working...");
  ImGui::End();
  return true;
}

/// The job memory of the running job (valid while a job runs)
JobMemory& job_memory(App& app);

/// Starts a background job of type <code>JobT</code> (if none is running); returns it to fill in first
template <typename JobT> JobT* prepare_job(App& app)
{
  if (!std::holds_alternative<std::monostate>(app.job))
  {
    report(app, Status::error("another operation is still running"));
    return nullptr;
  }
  partition(app.memory.jobs, job_memory(app));
  auto& bg = app.job.template emplace<jobs::Background<JobT>>();
  bg.job().memory = &job_memory(app);
  return &bg.job();
}

/// Launches the job prepared by <code>prepare_job</code>
void start_job(App& app);

/// Opens the file dialog for <code>action</code>
void open_file_dialog(App& app, FileAction action);

/// Runs <code>action</code> on <code>path</code> (after the dialog closes, or from the command line)
void file_action(App& app, FileAction action, const char* path);

// Panels and windows (one ImGui window each)
void viewport_window(App& app);
void layers_panel(App& app);
void tilesets_panel(App& app);
void tile_meta_panel(App& app);
void catalogs_panel(App& app);
void objects_panel(App& app);
void generate_panel(App& app);
void wfc_editor_panel(App& app);
void minimap_panel(App& app);
void log_panel(App& app);
void file_dialog(App& app);

/// Generators (as the Generate panel's buttons): start their background jobs with the current settings
void start_heightmap(App& app);
void start_watershed(App& app);
void start_wfc(App& app, std::size_t target_layer);

/// Draws tile <code>tile</code> of tileset <code>ts</code> as an ImGui image of the given size
void tile_image(const App& app, level::TileRef tile, float size);

/// ImGui texture id of a texture
inline ImTextureID texture_id(const Texture& t) { return static_cast<ImTextureID>(t.id); }

/// Combo box over a catalog (semantics/materials); returns true if changed
template <typename CatalogT> bool catalog_combo(const char* label, const CatalogT& catalog, std::uint16_t& id)
{
  bool changed = false;
  const char* preview = (id < catalog.size()) ? catalog[id].name.c_str() : "?";
  if (ImGui::BeginCombo(label, preview))
  {
    for (std::size_t i = 0; i < catalog.size(); ++i)
    {
      ImGui::PushID(static_cast<int>(i));
      const auto& c = catalog[i].color;
      ImGui::ColorButton(
        "##c",
        ImVec4{c.r / 255.0F, c.g / 255.0F, c.b / 255.0F, c.a / 255.0F},
        ImGuiColorEditFlags_NoTooltip,
        ImVec2{14, 14});
      ImGui::SameLine();
      if (ImGui::Selectable(catalog[i].name.c_str(), i == id))
      {
        id = static_cast<std::uint16_t>(i);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

/// Edits an image::Rgba with an ImGui color picker; returns true if changed
bool color_edit(const char* label, image::Rgba& color);

/// Text input over a FixedString; returns true if changed
template <std::size_t N> bool input_text(const char* label, FixedString<N>& s)
{
  return ImGui::InputText(label, s.data, N);
}

}  // namespace lvl::editor
