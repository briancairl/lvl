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

/// True while the file dialog is open
bool file_dialog_open();

/// Closes the file dialog without running its action
void close_file_dialog(App& app);

/// Writes the file dialog's bookmarks to the user's config
void save_file_dialog_places(App& app);

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
void minimap_panel(App& app);
void log_panel(App& app);
void file_dialog(App& app);
void style_panel(App& app);
/// The pop-out WFC neighbor editor: drag tiles into the eight sides around a rule tile
void wfc_neighbors_window(App& app);

/**
 * @brief Tooltip showing how rule tile <code>b</code> lines up <code>direction</code> of rule tile
 *        <code>a</code>, whether the rules allow it, and <code>hint</code> (if any)
 *
 * @return false if previews are off (App::wfc_preview), so the caller can show its own tooltip
 */
bool wfc_pair_tooltip(
  const App& app,
  level::WfcTileId a,
  level::WfcTileId b,
  level::Direction direction,
  const char* hint = nullptr);

/// Generators (as the Generate panel's buttons): start their background jobs with the current settings
void start_heightmap(App& app);
void start_watershed(App& app);
void start_wfc(App& app, std::size_t target_layer);

/// Rectangle of palette cells (of tileset <code>columns</code> wide) the selected tiles span
struct SelectionBounds
{
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  /// Columns of the palette the rectangle is in (so cell (x, y) is tile <code>y * columns + x</code>)
  int columns = 0;
};

/// Number of tiles selected in the open tileset's palette
inline std::size_t selection_size(const App& app)
{
  return (app.selection.tileset == app.tileset) ? app.selection.count : 0;
}

/// Tile grid a tileset's atlas slices into (0 x 0 if it has no image yet)
level::Extents palette_grid(const App& app, std::size_t tileset);

/// The selected tiles, in index order, in <code>arena</code>
memory::Vector<level::TileIndex> selected_tiles(const App& app, memory::Arena& arena);

/// The WFC rule tiles whose tile is selected in the palette, in <code>arena</code>
memory::Vector<level::WfcTileId> selected_rule_tiles(const App& app, memory::Arena& arena);

/// Palette rectangle the selected tiles span; false if nothing is selected
bool selection_bounds(const App& app, SelectionBounds& out);

/// Draws the selected tiles as the palette lays them out, in a child of at most <code>max_rows</code> rows
void selection_preview(const App& app, float thumb, int max_rows);

/// Fills <code>out</code> (an arena-backed grid) with the selected tiles as the palette lays them
/// out, empty where a cell of the rectangle is not selected; false if nothing is selected
bool palette_layout(const App& app, level::Grid<level::TileRef>& out);

/// A small delete button for the start of a row; returns true when clicked
bool delete_button(const char* tooltip);

/// Asks to delete something: a confirmation dialog follows, or with Shift held it is deleted at the
/// end of the frame (never while a panel is iterating over what's being deleted)
void request_delete(App& app, const PendingDelete& request);

/// Shows the confirmation dialog and applies confirmed deletes (once per frame, after the panels)
void process_deletes(App& app);

/// Texture and UV rectangle of a tile; false if it has no loaded texture
bool tile_uv(const App& app, level::TileRef tile, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1);

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

/**
 * @brief One row of a batch edit: the checkbox saying whether the field is applied, then the field
 *
 * The field is drawn disabled while the checkbox is off. Returns true when the checkbox changed.
 */
template <typename BodyT> bool batch_field(const char* id, bool& on, BodyT&& body)
{
  const bool toggled = ImGui::Checkbox(id, &on);
  ImGui::SameLine();
  ImGui::BeginDisabled(!on);
  body();
  ImGui::EndDisabled();
  return toggled;
}

/// Edits an image::Rgba with an ImGui color picker; returns true if changed
bool color_edit(const char* label, image::Rgba& color);

/// Text input over a FixedString; returns true if changed
template <std::size_t N> bool input_text(const char* label, FixedString<N>& s)
{
  return ImGui::InputText(label, s.data, N);
}

}  // namespace lvl::editor
