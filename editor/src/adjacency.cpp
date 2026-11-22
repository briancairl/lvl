/**
 * @copyright 2026-present Brian Cairl
 *
 * @file adjacency.cpp
 *
 * The tile adjacency rules window: the rule tiles, each tile's constraints, and which tiles may sit
 * on each of its eight sides. The rules are a level's one set of tile adjacencies (level::WfcRules),
 * read by the WFC generator and by any other auto-tiling that wants to know what fits next to what,
 * so the window stands on its own rather than living in the Generate panel.
 *
 * Neighbors are picked by clicking a tile, or by dragging the left button across several of them;
 * the pop-out drag-and-drop editor (neighbors.cpp) is the other way to edit the same rules.
 */

// C++ Standard Library
#include <algorithm>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

/// A left-button drag over the neighbor thumbnails of one direction, setting every tile it touches
struct NeighborDrag
{
  bool active = false;
  /// Direction whose thumbnails the drag started in (a drag never crosses into another one)
  Direction direction = Direction::kNorth;
  /// What the tiles it touches are set to (the opposite of the tile it started on)
  bool value = false;
};

NeighborDrag g_drag;

/// Draws rule tile <code>b</code> as a square the mouse can press (so a drag does not move the
/// window), outlined when it is allowed; the item is the square
void neighbor_thumb(const App& app, WfcTileId b, float size, bool on)
{
  ImGui::InvisibleButton("##thumb", ImVec2{size, size});
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImTextureID texture{};
  ImVec2 uv0;
  ImVec2 uv1;
  if (tile_uv(app, app.level->wfc.tiles[b].tile, texture, uv0, uv1))
  {
    draw->AddImage(texture, lo, hi, uv0, uv1);
  }
  else
  {
    draw->AddRectFilled(lo, hi, IM_COL32(90, 90, 90, 255));
  }
  if (on)
  {
    draw->AddRect(lo, hi, IM_COL32(80, 230, 120, 255), 0, 0, 2.0F);
  }
  else
  {
    draw->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, 150));
  }
}

/// Adjacencies taken from the palette: tiles sitting next to each other there may do so on the map
void palette_layout_section(App& app)
{
  WfcRules& rules = app.level->wfc;
  ImGui::SeparatorText("From the palette layout");
  if (selection_size(app) < 2)
  {
    ImGui::TextDisabled("Ctrl/shift+click tiles in the palette to make the way they are laid out there a rule");
    if (selection_size(app) == 1)
    {
      ImGui::SameLine();
      deselect_all_button(app);
    }
    return;
  }
  SelectionBounds bounds;
  selection_bounds(app, bounds);
  selection_preview(app, 24.0F, 4);
  LayoutMode& mode = app.generators.wfc_layout;
  if (ImGui::RadioButton("Allow what it shows", mode == LayoutMode::kAllow))
  {
    mode = LayoutMode::kAllow;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Only what it shows", mode == LayoutMode::kExclusive))
  {
    mode = LayoutMode::kExclusive;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip(
      "A hard constraint: on every side where the layout puts a neighbor, every other tile is forbidden");
  }
  if (ImGui::Button(format<64>("Apply the %d x %d layout", bounds.width, bounds.height)))
  {
    Grid<TileRef> layout{app.memory.frame};
    if (palette_layout(app, layout))
    {
      const std::size_t pairs = constrain_from_layout(rules, layout, mode, app.memory.frame);
      LVL_LOG_INFO_FMT(
        "Adjacency rules: %zu neighbor pair(s) from a %d x %d palette layout", pairs, bounds.width, bounds.height);
      app.modified = true;
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled(rules.diagonals ? "(sides and corners)" : "(sides only)");
  ImGui::SameLine();
  if (deselect_all_button(app))
  {
    return;
  }
}

/// Batch edit: the ticked constraints are applied to every selected rule tile
void batch_constraints_section(App& app)
{
  Level& lv = *app.level;
  WfcRules& rules = lv.wfc;
  WfcTileEdit& edit = app.wfc_batch;
  const auto tiles = selected_rule_tiles(app, app.memory.frame);
  ImGui::SeparatorText("Batch constraints");
  if (tiles.size() < 2)
  {
    ImGui::TextDisabled("Ctrl+click rule tiles above (or several palette tiles) to constrain them together");
    if (app.selection.count > 0)
    {
      ImGui::SameLine();
      deselect_all_button(app);
    }
    return;
  }
  ImGui::Text("%zu rule tiles selected", tiles.size());
  ImGui::SameLine();
  if (deselect_all_button(app))
  {
    return;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Take from this tile"))
  {
    const WfcTile& from = rules.tiles[app.wfc_tile];
    edit.weight = from.weight;
    edit.height_min = from.height_min;
    edit.height_max = from.height_max;
    edit.water = from.water;
    edit.semantic = from.semantic;
  }
  batch_field(
    "##batch_weight", edit.set_weight, [&] { ImGui::DragFloat("Weight##batch", &edit.weight, 0.05F, 0.0F, 1000.0F); });
  batch_field("##batch_band", edit.set_height, [&] {
    ImGui::DragFloatRange2("Height band##batch", &edit.height_min, &edit.height_max, 0.005F, 0.0F, 1.0F);
  });
  batch_field("##batch_water", edit.set_water, [&] { enum_combo("Water##batch", edit.water); });
  batch_field(
    "##batch_semantic", edit.set_semantic, [&] { catalog_combo("Semantic##batch", lv.semantics, edit.semantic); });
  if (ImGui::Button(format<64>("Apply to %zu rule tiles", tiles.size())))
  {
    const std::size_t edited = apply_edit(rules, edit, tiles.data(), tiles.size());
    LVL_LOG_INFO() << "Batch constraints applied to " << edited << " rule tiles";
    app.modified = true;
  }
}

/// The rule set's name, its import/export, and what adds or removes rule tiles
bool toolbar(App& app)
{
  Level& lv = *app.level;
  WfcRules& rules = lv.wfc;
  bool changed = false;
  input_text("Rule set", rules.name);
  if (ImGui::SmallButton("Import..."))
  {
    open_file_dialog(app, FileAction::kLoadWfc);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Export..."))
  {
    open_file_dialog(app, FileAction::kSaveWfc);
  }
  ImGui::SameLine();
  const std::size_t selected = selection_size(app);
  ImGui::BeginDisabled(selected == 0 and (app.paint.tile.empty() or app.paint.tile.tileset >= lv.tilesets.size()));
  if (ImGui::SmallButton("Add palette tile(s)"))
  {
    // Adds the palette selection, or the palette stamp when there is none
    if (selected > 0)
    {
      for (const TileIndex tile : selected_tiles(app, app.memory.frame))
      {
        app.wfc_tile = add_tile(rules, TileRef{static_cast<TileSetId>(app.tileset), tile});
      }
    }
    else
    {
      const TileSet& ts = lv.tilesets[app.paint.tile.tileset];
      const int cols = std::max(1, slice_grid(ts.slice, ts.image_width, ts.image_height).width);
      for (int dy = 0; dy < app.paint.stamp_height; ++dy)
      {
        for (int dx = 0; dx < app.paint.stamp_width; ++dx)
        {
          const int index = app.paint.tile.tile + dy * cols + dx;
          if (index < static_cast<int>(ts.tiles.size()))
          {
            app.wfc_tile = add_tile(rules, TileRef{app.paint.tile.tileset, static_cast<TileIndex>(index)});
          }
        }
      }
    }
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton("Neighbor editor..."))
  {
    app.panels.neighbors = true;
    ImGui::SetWindowFocus("Tile neighbors");
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Pop-out editor: drag tiles into the eight sides around a tile");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Clear rules"))
  {
    rules.tiles.clear();
    rules.adjacency.clear();
    changed = true;
  }
  return changed;
}

/// The strip of rule tiles: picks the tile whose rules are edited (ctrl+click: batch selection)
void tile_strip(App& app)
{
  WfcRules& rules = app.level->wfc;
  constexpr float kThumb = 32.0F;
  ImGui::BeginChild(
    "tiles",
    ImVec2{0, kThumb + 2 * ImGui::GetStyle().ScrollbarSize},
    ImGuiChildFlags_None,
    ImGuiWindowFlags_HorizontalScrollbar);
  for (std::size_t i = 0; i < rules.tiles.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    const TileRef ref = rules.tiles[i].tile;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    tile_image(app, ref, kThumb);
    if (ImGui::IsItemClicked())
    {
      if (ImGui::GetIO().KeyCtrl)
      {
        app.selection.retarget(ref.tileset);
        app.selection.set(ref.tile, !app.selection.has(ref.tile));
      }
      app.wfc_tile = static_cast<WfcTileId>(i);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 q{p.x + kThumb, p.y + kThumb};
    if (app.selection.has(ref.tileset, ref.tile))
    {
      draw->AddRectFilled(p, q, IM_COL32(255, 230, 60, 60));
    }
    if (i == app.wfc_tile)
    {
      draw->AddRect(p, q, IM_COL32(255, 230, 60, 255), 0, 0, 2.0F);
    }
    ImGui::SameLine();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

/**
 * @brief One direction's row of thumbnails: the tiles that may sit <code>d</code> of <code>a</code>
 *
 * A click toggles one tile; holding the left button and dragging across several sets all of them to
 * what the first one became, so a whole row can be allowed (or forbidden) in one sweep.
 */
bool neighbor_row(App& app, WfcTileId a, Direction d)
{
  WfcRules& rules = app.level->wfc;
  const auto n = static_cast<WfcTileId>(rules.tiles.size());
  bool changed = false;
  ImGui::PushID(static_cast<int>(d));
  ImGui::Text("%-10s", enum_name(d).data());
  ImGui::SameLine();
  if (ImGui::SmallButton("all"))
  {
    for (WfcTileId b = 0; b < n; ++b)
    {
      set_allowed(rules, a, b, d, true);
    }
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("none"))
  {
    for (WfcTileId b = 0; b < n; ++b)
    {
      set_allowed(rules, a, b, d, false);
    }
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(diagonal(d) ? "all corners" : "all sides"))
  {
    // Copy this direction's set to the other sides (or corners)
    for (const Direction other : diagonal(d) ? kDiagonals : kDirections)
    {
      if (other == d)
      {
        continue;
      }
      for (WfcTileId b = 0; b < n; ++b)
      {
        set_allowed(rules, a, b, other, allowed(rules, a, b, d));
      }
    }
    changed = true;
  }
  constexpr float kSmall = 24.0F;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int per_row = std::max(1, static_cast<int>(avail / (kSmall + ImGui::GetStyle().ItemSpacing.x)));
  for (WfcTileId b = 0; b < n; ++b)
  {
    ImGui::PushID(b);
    if (b % per_row != 0)
    {
      ImGui::SameLine();
    }
    const bool on = allowed(rules, a, b, d);
    neighbor_thumb(app, b, kSmall, on);
    if (ImGui::IsItemHovered() and !g_drag.active)
    {
      wfc_pair_tooltip(app, a, b, d, "Click to allow or forbid it there, or drag across several");
    }
    // Pressing a thumbnail starts a drag that sets every one it reaches to the opposite of this one
    if (ImGui::IsItemClicked())
    {
      g_drag = NeighborDrag{.active = true, .direction = d, .value = !on};
    }
    // While it lasts the pressed thumbnail keeps the active id, so hovering is tested by rectangle
    if (
      g_drag.active and g_drag.direction == d and on != g_drag.value and
      ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()))
    {
      set_allowed(rules, a, b, d, g_drag.value);
      changed = true;
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  return changed;
}

/// The window's body (the rules of the level's one adjacency set)
void adjacency_body(App& app)
{
  Level& lv = *app.level;
  WfcRules& rules = lv.wfc;
  bool changed = toolbar(app);
  palette_layout_section(app);
  if (rules.tiles.empty())
  {
    ImGui::TextDisabled("No rule tiles. Select tiles in the palette and add them, or learn from a tile layer "
                        "(Generate > WFC).");
    app.modified |= changed;
    return;
  }
  app.wfc_tile = std::min<WfcTileId>(app.wfc_tile, static_cast<WfcTileId>(rules.tiles.size() - 1));
  tile_strip(app);

  WfcTile& tile = rules.tiles[app.wfc_tile];
  ImGui::SeparatorText("Tile constraints");
  tile_image(app, tile.tile, 48.0F);
  ImGui::SameLine();
  ImGui::BeginGroup();
  const char* name =
    (tile.tile.tileset < lv.tilesets.size() and tile.tile.tile < lv.tilesets[tile.tile.tileset].tiles.size())
    ? lv.tilesets[tile.tile.tileset].tiles[tile.tile.tile].name.c_str()
    : "";
  ImGui::Text("#%u  tile %u:%u %s", app.wfc_tile, tile.tile.tileset, tile.tile.tile, name);
  changed |= ImGui::DragFloat("Weight", &tile.weight, 0.05F, 0.0F, 1000.0F);
  changed |= ImGui::DragFloatRange2("Height band", &tile.height_min, &tile.height_max, 0.005F, 0.0F, 1.0F);
  changed |= enum_combo("Water", tile.water);
  changed |= catalog_combo("Semantic", lv.semantics, tile.semantic);
  if (ImGui::SmallButton("Remove from rules"))
  {
    remove_tile(rules, app.wfc_tile);
    app.wfc_tile = 0;
    app.modified = true;
    ImGui::EndGroup();
    return;
  }
  ImGui::EndGroup();
  batch_constraints_section(app);

  // Adjacency: per direction, which tiles may sit on that side of the selected tile
  ImGui::SeparatorText("Allowed neighbors (click, or drag over several, to toggle)");
  const WfcTileId a = app.wfc_tile;
  ImGui::Checkbox("Preview pairs", &app.wfc_preview);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Hovering a neighbor shows the two tiles as they would sit next to each other");
  }
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Constrain corners (8-way)", &rules.diagonals);
  if (rules.diagonals and changed and diagonal_count(rules) == 0)
  {
    derive_diagonals(rules, app.memory.frame);
  }
  for (const Direction d : kNeighbors)
  {
    if (diagonal(d) and !rules.diagonals)
    {
      continue;
    }
    changed |= neighbor_row(app, a, d);
  }
  ImGui::TextDisabled("Rules are symmetric: allowing B east of A also allows A west of B.");
  app.modified |= changed;
}

}  // namespace

void adjacency_window(App& app)
{
  ImGui::SetNextWindowSize(ImVec2{640, 700}, ImGuiCond_FirstUseEver);
  // Floats over the viewport: opaque, so the map doesn't show through
  ImGui::SetNextWindowBgAlpha(1.0F);
  if (!ImGui::Begin("Tile adjacency rules", &app.panels.adjacency))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  // A drag over the neighbor thumbnails lasts until the button comes back up (wherever that is)
  if (g_drag.active and !ImGui::IsMouseDown(ImGuiMouseButton_Left))
  {
    g_drag.active = false;
  }
  adjacency_body(app);
  ImGui::End();
}

}  // namespace lvl::editor
