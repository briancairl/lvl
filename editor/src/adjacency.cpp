/**
 * @copyright 2026-present Brian Cairl
 *
 * @file adjacency.cpp
 *
 * The tile adjacency rules window, which has a tab per kind of rule a level holds:
 *
 * - WFC (level::WfcRules): the rule tiles, each tile's constraints, and which tiles may sit on each
 *   of its eight sides. A permission relation on pairs, per direction, which the WFC generator
 *   solves over a whole region.
 * - Auto-tiling (level::AutoTileRules): terrains, each keying its tiles on the pattern of like
 *   neighbors around a cell. That is what can prescribe an edge or an inner/outer corner, so it is
 *   what the Auto tool paints with.
 *
 * The rules are the level's own, read by the generators and the painting tools, so the window stands
 * on its own rather than living in the Generate panel.
 *
 * Neighbors are picked by clicking a tile, or by dragging the left button across several of them;
 * the pop-out drag-and-drop editor (neighbors.cpp) is the other way to edit the same rules.
 */

// C++ Standard Library
#include <algorithm>
#include <cstring>

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
  if (tile_uv(app, rule_set(app).wfc.tiles[b].tile, texture, uv0, uv1))
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
  WfcRules& rules = rule_set(app).wfc;
  ImGui::SeparatorText("From the palette layout");
  if (selection_size(app) < 2)
  {
    ImGui::TextDisabled("Ctrl/shift+click tiles in the palette to make the way they are laid out there a rule");
    if (selection_size(app) == 1)
    {
      ImGui::SameLine();
      deselect_all_button(app, "wfc_layout");
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
  if (deselect_all_button(app, "wfc_layout"))
  {
    return;
  }
}

/// Batch edit: the ticked constraints are applied to every selected rule tile
void batch_constraints_section(App& app)
{
  project::Project& pr = *app.project;
  WfcRules& rules = rule_set(app).wfc;
  WfcTileEdit& edit = app.wfc_batch;
  const auto tiles = selected_rule_tiles(app, app.memory.frame);
  ImGui::SeparatorText("Batch constraints");
  if (tiles.size() < 2)
  {
    ImGui::TextDisabled("Ctrl+click rule tiles above (or several palette tiles) to constrain them together");
    if (app.selection.count > 0)
    {
      ImGui::SameLine();
      deselect_all_button(app, "wfc_batch");
    }
    return;
  }
  ImGui::Text("%zu rule tiles selected", tiles.size());
  ImGui::SameLine();
  if (deselect_all_button(app, "wfc_batch"))
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
    "##batch_semantic", edit.set_semantic, [&] { catalog_combo("Semantic##batch", pr.semantics, edit.semantic); });
  if (ImGui::Button(format<64>("Apply to %zu rule tiles", tiles.size())))
  {
    const std::size_t edited = apply_edit(rules, edit, tiles.data(), tiles.size());
    LVL_LOG_INFO() << "Batch constraints applied to " << edited << " rule tiles";
    app.modified = true;
  }
}

/// The rule set's name, its import/export, and what adds or removes rule tiles
/// The rule set's name and its file, which carries both kinds of rules
void rules_header(App& app)
{
  input_text("Rule set", rule_set(app).wfc.name);
  if (ImGui::SmallButton("Import..."))
  {
    open_file_dialog(app, FileAction::kLoadWfc);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Export..."))
  {
    open_file_dialog(app, FileAction::kSaveWfc);
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("One file holds both the WFC adjacencies and the auto-tiling terrains");
  }
}

bool toolbar(App& app)
{
  project::Project& pr = *app.project;
  WfcRules& rules = rule_set(app).wfc;
  bool changed = false;
  const std::size_t selected = selection_size(app);
  ImGui::BeginDisabled(selected == 0 and (app.paint.tile.empty() or app.paint.tile.tileset >= pr.tilesets.size()));
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
      const TileSet& ts = pr.tilesets[app.paint.tile.tileset];
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
  WfcRules& rules = rule_set(app).wfc;
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
  WfcRules& rules = rule_set(app).wfc;
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
// -------------------------------------------------------------------------------------------------
// Auto-tiling rules: terrains whose tiles are keyed on the pattern of like neighbors
// -------------------------------------------------------------------------------------------------

/// Side of a slot the terrain does *not* continue through, as a line; a missing corner as a notch
void draw_pattern(ImDrawList* draw, ImVec2 lo, ImVec2 hi, NeighborMask mask, Neighborhood neighborhood)
{
  constexpr ImU32 kEdge = IM_COL32(255, 170, 60, 255);
  constexpr float kThick = 2.5F;
  const float notch = (hi.x - lo.x) * 0.22F;
  if (!has(mask, Direction::kNorth))
  {
    draw->AddLine(ImVec2{lo.x, lo.y + 1}, ImVec2{hi.x, lo.y + 1}, kEdge, kThick);
  }
  if (!has(mask, Direction::kSouth))
  {
    draw->AddLine(ImVec2{lo.x, hi.y - 1}, ImVec2{hi.x, hi.y - 1}, kEdge, kThick);
  }
  if (!has(mask, Direction::kWest))
  {
    draw->AddLine(ImVec2{lo.x + 1, lo.y}, ImVec2{lo.x + 1, hi.y}, kEdge, kThick);
  }
  if (!has(mask, Direction::kEast))
  {
    draw->AddLine(ImVec2{hi.x - 1, lo.y}, ImVec2{hi.x - 1, hi.y}, kEdge, kThick);
  }
  if (neighborhood == Neighborhood::kSides)
  {
    return;
  }
  // A corner only shows where the terrain reaches both of its sides but not the corner itself
  for (const Direction corner : kDiagonals)
  {
    const auto [a, b] = corner_sides(corner);
    if (has(mask, corner) or !has(mask, a) or !has(mask, b))
    {
      continue;
    }
    const ImVec2 at{
      (corner == Direction::kNorthEast or corner == Direction::kSouthEast) ? hi.x - notch : lo.x,
      (corner == Direction::kSouthEast or corner == Direction::kSouthWest) ? hi.y - notch : lo.y};
    draw->AddRectFilled(at, ImVec2{at.x + notch, at.y + notch}, kEdge);
  }
}

/// One pattern's slot: the tile it holds, outlined where the terrain ends
bool pattern_slot(App& app, AutoTerrain& terrain, NeighborMask mask, float size)
{
  bool changed = false;
  ImGui::PushID(static_cast<int>(mask));
  const AutoVariant* variant = find_variant(terrain, mask);
  ImGui::InvisibleButton("##slot", ImVec2{size, size});
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImTextureID texture{};
  ImVec2 uv0;
  ImVec2 uv1;
  if (variant != nullptr and tile_uv(app, variant->tile, texture, uv0, uv1))
  {
    draw->AddImage(texture, lo, hi, uv0, uv1);
  }
  else
  {
    draw->AddRectFilled(lo, hi, (variant == nullptr) ? IM_COL32(40, 40, 40, 255) : IM_COL32(90, 90, 90, 255));
  }
  draw_pattern(draw, lo, hi, mask, terrain.neighborhood);
  // A pattern can hold several tiles, which a painted area scatters between
  const std::size_t pool = variant_count(terrain, mask);
  if (pool > 1)
  {
    draw->AddText(
      ImVec2{lo.x + 2, hi.y - ImGui::GetTextLineHeight()}, IM_COL32(255, 255, 255, 220), format<8>("x%zu", pool));
  }
  const bool can_set = !app.paint.tile.empty() and app.paint.tile.tileset < app.project->tilesets.size();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) and can_set)
  {
    if (ImGui::GetIO().KeyCtrl)
    {
      add_variant(terrain, mask, app.paint.tile);
    }
    else
    {
      set_variant(terrain, mask, app.paint.tile);
    }
    changed = true;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
  {
    set_variant(terrain, mask, kEmptyTile);
    changed = true;
  }
  if (ImGui::IsItemHovered())
  {
    // The directions the pattern says the terrain continues in, as one line
    FixedString<128> where;
    std::size_t at = 0;
    for (const Direction d : kNeighbors)
    {
      const std::string_view name = enum_name(d);
      if (!has(mask, d) or at + name.size() + 3 >= FixedString<128>::kMaxLength)
      {
        continue;
      }
      if (at > 0)
      {
        where.data[at++] = ',';
        where.data[at++] = ' ';
      }
      std::memcpy(where.data + at, name.data(), name.size());
      at += name.size();
    }
    where.data[at] = '\0';
    ImGui::SetTooltip(
      "Terrain continues: %s\n%s\nClick to use the palette tile, ctrl+click to add another to the "
      "pool, right-click to clear",
      where.empty() ? "nowhere (a lone cell)" : where.c_str(),
      (variant == nullptr) ? "No tile: the nearest pattern's tile is used"
                           : format<64>("%zu tile(s); a cell picks by position", pool));
  }
  ImGui::PopID();
  return changed;
}

/// The terrain list, and the one being edited
bool terrain_list(App& app)
{
  project::Project& pr = *app.project;
  AutoTileRules& rules = rule_set(app).auto_tiles;
  bool changed = false;
  if (ImGui::SmallButton("Add terrain"))
  {
    app.terrain = add_terrain(rules, *pr.arena, format<32>("terrain_%zu", rules.terrains.size() + 1));
    changed = true;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("A named set of tiles (water, a path, grass) the Auto tool paints and borders");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu terrain(s)", rules.terrains.size());
  if (rules.terrains.empty())
  {
    return changed;
  }
  app.terrain = std::min<TerrainId>(app.terrain, static_cast<TerrainId>(rules.terrains.size() - 1));
  ImGui::BeginChild("terrains", ImVec2{0, 4 * ImGui::GetTextLineHeightWithSpacing()}, ImGuiChildFlags_Borders);
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Delete this terrain"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kTerrain,
          .index = i,
          .what = format<128>("terrain '%s'", rules.terrains[i].name.c_str()),
          .detail = "The tiles it holds stay in their tilesets."});
    }
    ImGui::SameLine();
    if (ImGui::Selectable(
          format<96>("%s (%zu tile(s))", rules.terrains[i].name.c_str(), rules.terrains[i].variants.size()),
          i == app.terrain))
    {
      app.terrain = static_cast<TerrainId>(i);
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  return changed;
}

/// A terrain's tiles taken from the palette selection (a nine-slice, inner corners, or one tile)
bool terrain_layout_section(App& app, AutoTerrain& terrain)
{
  ImGui::SeparatorText("From the palette layout");
  ImGui::TextDisabled("Select the 3x3 block of edges and outer corners, the 2x2 block of inner corners beside\n"
                      "it, or both at once; tiles selected with them become alternative middles");
  if (selection_size(app) < 2)
  {
    if (selection_size(app) == 1)
    {
      deselect_all_button(app, "terrain_layout");
    }
    return false;
  }
  SelectionBounds bounds;
  selection_bounds(app, bounds);
  selection_preview(app, 24.0F, 4);
  const bool inner_only = (bounds.width == 2 and bounds.height == 2);
  const bool blob_needed = inner_only and terrain.neighborhood == Neighborhood::kSides;
  bool changed = false;
  ImGui::BeginDisabled(blob_needed);
  if (ImGui::Button(format<64>("Take these %zu tile(s)", selection_size(app))))
  {
    Grid<TileRef> layout{app.memory.frame};
    if (palette_layout(app, layout))
    {
      const std::size_t written = learn_from_layout(terrain, layout);
      LVL_LOG_INFO_FMT("Terrain '%s': %zu tile(s) from the palette", terrain.name.c_str(), written);
      changed = written > 0;
      if (written == 0)
      {
        report(app, Status::error("the selected tiles hold no 3x3 or 2x2 block to read a terrain from"));
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (blob_needed)
  {
    ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "inner corners need the blob neighborhood");
  }
  ImGui::SameLine();
  if (deselect_all_button(app, "terrain_layout"))
  {
    return changed;
  }
  return changed;
}

void auto_tile_body(App& app)
{
  bool changed = terrain_list(app);
  if (rule_set(app).auto_tiles.terrains.empty())
  {
    ImGui::TextDisabled("No terrains yet. Add one, then give it the tiles of its edges and corners -- the usual\n"
                        "way is to select the 3x3 block of a tileset's nine-slice and take it below.");
    app.modified |= changed;
    return;
  }
  AutoTerrain& terrain = rule_set(app).auto_tiles.terrains[app.terrain];
  ImGui::SeparatorText("Terrain");
  changed |= input_text("Name", terrain.name);
  changed |= enum_combo("Neighborhood", terrain.neighborhood);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("'sides' keys the tiles on the four sides (16 patterns, a nine-slice covers them);\n"
                      "'blob' also keys on the corners, which is what tells inner from outer corners (47)");
  }
  changed |= ImGui::SliderInt("Border", &terrain.expand, 0, 2);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("How far past what you paint the terrain reaches.\n"
                      "1: painting one cell lays its middle tile and borders the ring around it.\n"
                      "0: the cells you paint take the edge tiles themselves.");
  }
  changed |= terrain_layout_section(app, terrain);

  memory::Scope scope{app.memory.frame};
  const auto masks = patterns(terrain.neighborhood, app.memory.frame);
  std::size_t filled = 0;
  for (const NeighborMask mask : masks)
  {
    filled += (find_variant(terrain, mask) == nullptr) ? 0U : 1U;
  }
  ImGui::SeparatorText(format<96>("Tiles per pattern (%zu of %zu)", filled, masks.size()));
  ImGui::TextDisabled("The lines show where the terrain ends. Click a slot to use the palette tile.");
  constexpr float kSlot = 40.0F;
  ImGui::BeginChild("patterns", ImVec2{0, 0}, ImGuiChildFlags_Borders);
  const float width = ImGui::GetContentRegionAvail().x;
  const float step = kSlot + ImGui::GetStyle().ItemSpacing.x;
  const int columns = std::max(1, static_cast<int>(width / step));
  for (std::size_t i = 0; i < masks.size(); ++i)
  {
    if (i > 0 and static_cast<int>(i) % columns != 0)
    {
      ImGui::SameLine();
    }
    changed |= pattern_slot(app, terrain, masks[i], kSlot);
  }
  ImGui::EndChild();
  app.modified |= changed;
}

// -------------------------------------------------------------------------------------------------
// Height-step rules: terrains whose tiles are keyed on the difference in level between cells
// -------------------------------------------------------------------------------------------------

/// The pattern a step type answers, drawn as the outline of the ground that stands above the rest
void draw_step(ImDrawList* draw, ImVec2 lo, ImVec2 hi, StepType type)
{
  constexpr ImU32 kEdge = IM_COL32(120, 200, 255, 255);
  constexpr float kThick = 2.5F;
  // Rather than keep a second copy of the table, probe it: the pattern to draw is the first set of
  // dropped neighbors that names this type
  StepCode code = 0;
  for (unsigned mask = 0; mask < 256U; ++mask)
  {
    StepCode candidate = 0;
    for (std::size_t n = 0; n < kStepNeighbors.size(); ++n)
    {
      candidate = step_code_with(candidate, n, ((mask >> n) & 1U) != 0U ? StepRelation::kBelow : StepRelation::kLevel);
    }
    if (step_type_of(candidate) == type)
    {
      code = candidate;
      break;
    }
  }
  const float notch = (hi.x - lo.x) * 0.22F;
  const auto drops = [code](std::size_t n) { return step_relation(code, n) == StepRelation::kBelow; };
  if (drops(1))
  {
    draw->AddLine(ImVec2{lo.x, lo.y + 1}, ImVec2{hi.x, lo.y + 1}, kEdge, kThick);
  }
  if (drops(6))
  {
    draw->AddLine(ImVec2{lo.x, hi.y - 1}, ImVec2{hi.x, hi.y - 1}, kEdge, kThick);
  }
  if (drops(3))
  {
    draw->AddLine(ImVec2{lo.x + 1, lo.y}, ImVec2{lo.x + 1, hi.y}, kEdge, kThick);
  }
  if (drops(4))
  {
    draw->AddLine(ImVec2{hi.x - 1, lo.y}, ImVec2{hi.x - 1, hi.y}, kEdge, kThick);
  }
  // A corner that drops on its own is the concave case, drawn as a notch
  static constexpr std::size_t kCorners[4] = {0, 2, 5, 7};
  static constexpr std::size_t kCornerSides[4][2] = {{1, 3}, {1, 4}, {6, 3}, {6, 4}};
  for (std::size_t i = 0; i < 4; ++i)
  {
    if (!drops(kCorners[i]) or drops(kCornerSides[i][0]) or drops(kCornerSides[i][1]))
    {
      continue;
    }
    const bool east = (kCorners[i] == 2 or kCorners[i] == 7);
    const bool south = (kCorners[i] == 5 or kCorners[i] == 7);
    const ImVec2 at{east ? hi.x - notch : lo.x, south ? hi.y - notch : lo.y};
    draw->AddRectFilled(at, ImVec2{at.x + notch, at.y + notch}, kEdge);
  }
}

/// One step type's slot: the tile it holds, outlined where the ground drops away
bool step_slot(App& app, StepTerrain& terrain, StepType type, float size)
{
  bool changed = false;
  const auto index = static_cast<std::size_t>(type);
  ImGui::PushID(static_cast<int>(index));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const TileRef held = terrain.tiles[index];
  if (held.empty())
  {
    ImGui::Dummy(ImVec2{size, size});
    ImGui::GetWindowDrawList()->AddRectFilled(
      origin, ImVec2{origin.x + size, origin.y + size}, IM_COL32(28, 28, 32, 255));
  }
  else
  {
    tile_image(app, held, size);
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) and !app.paint.tile.empty())
  {
    terrain.tiles[index] = app.paint.tile;
    changed = true;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
  {
    terrain.tiles[index] = kEmptyTile;
    changed = true;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip(
      "%s\nClick to use the palette tile, right-click to clear.\nThe lines show where the ground drops away.%s",
      enum_name(type).data(),
      (type == StepType::kNone) ? "\nNo arrangement of levels asks for this one; gft's sheets keep the slot." : "");
  }
  ImGui::GetWindowDrawList()->AddRect(origin, ImVec2{origin.x + size, origin.y + size}, IM_COL32(90, 90, 100, 255));
  draw_step(ImGui::GetWindowDrawList(), origin, ImVec2{origin.x + size, origin.y + size}, type);
  ImGui::PopID();
  return changed;
}

bool step_terrain_list(App& app)
{
  StepRules& rules = rule_set(app).steps;
  bool changed = false;
  if (ImGui::SmallButton("Add terrain"))
  {
    const auto next = static_cast<int>(rules.terrains.size());
    app.step_terrain = add_step_terrain(rules, format<32>("level_%d", next));
    rules.terrains[app.step_terrain].level_lo = next;
    rules.terrains[app.step_terrain].level_hi = next;
    changed = true;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("A band of height levels and the eighteen tiles its ground and its cliffs are drawn from");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu terrain(s)", rules.terrains.size());
  if (rules.terrains.empty())
  {
    return changed;
  }
  app.step_terrain = std::min<StepTerrainId>(app.step_terrain, static_cast<StepTerrainId>(rules.terrains.size() - 1));
  ImGui::BeginChild("step_terrains", ImVec2{0, 4 * ImGui::GetTextLineHeightWithSpacing()}, ImGuiChildFlags_Borders);
  for (std::size_t i = 0; i < rules.terrains.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Delete this terrain"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kStepTerrain,
          .index = i,
          .what = format<128>("step terrain '%s'", rules.terrains[i].name.c_str()),
          .detail = "Cells at its levels will be left untiled until another terrain covers them."});
    }
    ImGui::SameLine();
    std::size_t filled = 0;
    for (const TileRef& t : rules.terrains[i].tiles)
    {
      filled += t.empty() ? 0U : 1U;
    }
    if (ImGui::Selectable(
          format<96>(
            "%s  levels %d..%d  (%zu/18)",
            rules.terrains[i].name.c_str(),
            rules.terrains[i].level_lo,
            rules.terrains[i].level_hi,
            filled),
          i == app.step_terrain))
    {
      app.step_terrain = static_cast<StepTerrainId>(i);
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  return changed;
}

void steps_body(App& app)
{
  bool changed = step_terrain_list(app);
  ImGui::TextDisabled("Ground cut into whole height levels, with a cliff drawn wherever two levels meet.\n"
                      "Fill a map with them in Generate > Height steps.");
  if (rule_set(app).steps.terrains.empty())
  {
    ImGui::TextDisabled("No terrains yet. Add one, then select a tileset's 3-wide, 6-tall step block and take it.");
    app.modified |= changed;
    return;
  }
  StepTerrain& terrain = rule_set(app).steps.terrains[app.step_terrain];
  ImGui::SeparatorText("Terrain");
  changed |= input_text("Name", terrain.name);
  changed |= ImGui::DragIntRange2("Levels", &terrain.level_lo, &terrain.level_hi, 0.1F, 0, 255);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("The height levels this terrain covers. A cell takes the first terrain whose band holds its "
                      "level, so bands should not overlap.");
  }

  // The 3x6 block, read the way gft's sheets hold it
  memory::Scope scope{app.memory.frame};
  Grid<TileRef> layout{app.memory.frame};
  StepLayout shape = StepLayout::kStacked;
  const bool has_block =
    palette_layout(app, layout) and step_layout_of(layout.extents.width, layout.extents.height, shape);
  ImGui::BeginDisabled(!has_block);
  if (ImGui::Button(
        has_block and shape == StepLayout::kSideBySide ? "Take the palette block (step-ups left, step-downs right)"
                                                       : "Take the palette block"))
  {
    learn_step_layout(terrain, layout);
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (!has_block)
  {
    ImGui::TextDisabled("select 18 tiles: 6 across and 3 down, or 3 across and 6 down");
  }
  else
  {
    ImGui::TextDisabled(
      "%s", (shape == StepLayout::kSideBySide) ? "6 across, 3 down" : "3 across, 6 down (gft's own order)");
  }

  // The slots are drawn in whichever shape the palette selection is, so they sit as the tileset does
  const StepLayout drawn = has_block ? shape : StepLayout::kSideBySide;
  const int block_w = (drawn == StepLayout::kSideBySide) ? kSideBySideWidth : kStackedWidth;
  const int block_h = (drawn == StepLayout::kSideBySide) ? kSideBySideHeight : kStackedHeight;

  ImGui::SeparatorText("Tiles");
  constexpr float kSlot = 40.0F;
  ImGui::BeginChild("step_types", ImVec2{0, 0}, ImGuiChildFlags_Borders);
  for (int y = 0; y < block_h; ++y)
  {
    for (int x = 0; x < block_w; ++x)
    {
      if (x > 0)
      {
        ImGui::SameLine();
      }
      changed |= step_slot(app, terrain, static_cast<StepType>(step_type_at(drawn, x, y)), kSlot);
    }
  }
  ImGui::EndChild();
  app.modified |= changed;
}

void adjacency_body(App& app)
{
  project::Project& pr = *app.project;
  WfcRules& rules = rule_set(app).wfc;
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
    (tile.tile.tileset < pr.tilesets.size() and tile.tile.tile < pr.tilesets[tile.tile.tileset].tiles.size())
    ? pr.tilesets[tile.tile.tileset].tiles[tile.tile.tile].name.c_str()
    : "";
  ImGui::Text("#%u  tile %u:%u %s", app.wfc_tile, tile.tile.tileset, tile.tile.tile, name);
  changed |= ImGui::DragFloat("Weight", &tile.weight, 0.05F, 0.0F, 1000.0F);
  changed |= ImGui::DragFloatRange2("Height band", &tile.height_min, &tile.height_max, 0.005F, 0.0F, 1.0F);
  changed |= enum_combo("Water", tile.water);
  changed |= catalog_combo("Semantic", pr.semantics, tile.semantic);
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
  rules_header(app);
  // The two kinds of rules answer different questions: WFC adjacency says what *may* sit next to
  // what (a permission relation, per direction), while a terrain says which tile goes where its
  // like neighbors form a given pattern, which is what prescribes edges and inner/outer corners
  if (ImGui::BeginTabBar("rules"))
  {
    // App::rules_mode is the source of truth: when something else changes it the tab follows (which
    // takes a frame, as ImGui applies the request when the tab is submitted), and otherwise the
    // tab the user clicks sets it
    static RulesMode shown = RulesMode::kWfc;
    const bool follow = (shown != app.rules_mode);
    const auto flags = [&app, follow](RulesMode mode) {
      return (follow and app.rules_mode == mode) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabItem("WFC", nullptr, flags(RulesMode::kWfc)))
    {
      shown = RulesMode::kWfc;
      adjacency_body(app);
      ImGui::EndTabItem();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("What may sit next to what, per direction: the constraints the WFC generator solves");
    }
    if (ImGui::BeginTabItem("Auto-tiling", nullptr, flags(RulesMode::kAutoTile)))
    {
      shown = RulesMode::kAutoTile;
      auto_tile_body(app);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Height steps", nullptr, flags(RulesMode::kSteps)))
    {
      shown = RulesMode::kSteps;
      ImGui::BeginChild("steps");
      steps_body(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Terrains: which tile to use for each pattern of like neighbors, which is what the Auto "
                        "tool paints with");
    }
    ImGui::EndTabBar();
    if (!follow)
    {
      app.rules_mode = shown;
    }
  }
  ImGui::End();
}

}  // namespace lvl::editor
