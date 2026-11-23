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

/// Reads the recent levels from the user's config (once; later calls do nothing)
void load_recent_files(App& app);

/// Puts <code>path</code> at the front of the recent levels and writes the list to the user's config
void remember_recent_file(App& app, std::string_view path);

/// Drops <code>path</code> from the recent levels (a load of it failed), and writes the list
void forget_recent_file(App& app, std::string_view path);

/// Empties the recent levels, and writes the list
void clear_recent_files(App& app);

/// The File > Open recent submenu (opens the level it is given, or clears the list)
void recent_files_menu(App& app);

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
/// The tile adjacency rules window: the rule tiles, their constraints and their allowed neighbors
void adjacency_window(App& app);
/// The pop-out neighbor editor: drag tiles into the eight sides around a rule tile
void neighbors_window(App& app);

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

/**
 * @brief Fills the cells of a tile layer that the Generate panel's mask lets through
 *
 * The mask (a band of a scalar layer, a semantic ground type, a region, whether to spare cells that
 * already hold a tile) comes from <code>App::generators</code>, and the scatter from the level's
 * seed. Takes an undo snapshot and records what it did in <code>Generators::fill_result</code>.
 */
/// Generate > Height steps: cuts the heightmap into whole levels (and redraws the tiles, if asked)
void apply_terrace(App& app);

/// Index of the tile layer a step terrain draws into (the one named after it), or layers.size()
std::size_t step_layer_of(const level::Level& level, std::string_view terrain);

/// Adds a tile layer for every step terrain that has none, lowest terrace first
void add_step_layers(App& app);

/**
 * @brief Generate > Height steps: draws the cliffs between the levels, and blocks them if asked
 *
 * One tile layer per terrain, found by <code>step_layer_of</code>: a cliff tile is transparent
 * where the terrace below shows through it, so they are composited rather than flattened. The
 * collision mask stays a single flat layer, since it comes from the levels and not from any one
 * terrace.
 */
void apply_steps(App& app);

void apply_fill(App& app, std::size_t layer, const level::TileRef* tiles, const float* weights, std::size_t count);

/// A "Deselect all" button (disabled with nothing selected); empties the palette selection when clicked
bool deselect_all_button(App& app);

/// Fills <code>out</code> (an arena-backed grid) with the selected tiles as the palette lays them
/// out, empty where a cell of the rectangle is not selected; false if nothing is selected
bool palette_layout(const App& app, level::Grid<level::TileRef>& out);

/**
 * @brief The tiles one application of the paint tool writes, laid out as the palette has them
 *
 * With two or more tiles selected in the palette it is the selection's layout (so a multi-tile
 * object keeps its shape; cells the selection skips are left alone), otherwise the paint stamp: one
 * tile, or the rectangle a palette drag picked. False if there is no tile to paint with.
 */
bool paint_stamp(const App& app, level::Grid<level::TileRef>& out);

/// Footprint of <code>paint_stamp</code>, without building it (for the cursor's hover preview)
level::Extents stamp_extents(const App& app);

/// Auto-tiling terrain the tile being painted belongs to, or <code>level::kNoTerrain</code>
level::TerrainId auto_tile_terrain(const App& app);

/**
 * @brief Tiles the cells one stroke of the Auto tool painted, from whichever rules apply
 *
 * The tile being painted decides which: the auto-tiling terrain it belongs to
 * (<code>auto_tile_terrain</code>), which prescribes edges and inner/outer corners, or else the WFC
 * adjacency rules, scored against each cell's neighbors, as the fallback. Records what the stroke
 * did in <code>App::auto_tile_status</code>; <code>layer</code> must be a tile layer.
 */
void auto_tile_stroke(App& app, std::size_t layer, const level::Cell* painted, std::size_t count);

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

/// Combo box over a serialized enum's names; returns true if changed
template <typename EnumT> bool enum_combo(const char* label, EnumT& value)
{
  bool changed = false;
  if (ImGui::BeginCombo(label, enum_name(value).data()))
  {
    for (std::size_t i = 0; i < EnumNames<EnumT>::kNames.size(); ++i)
    {
      if (ImGui::Selectable(EnumNames<EnumT>::kNames[i].data(), static_cast<std::size_t>(value) == i))
      {
        value = static_cast<EnumT>(i);
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

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
