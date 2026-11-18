/**
 * @copyright 2026-present Brian Cairl
 *
 * @file wfc_neighbors.cpp
 *
 * The pop-out WFC neighbor editor. The selected rule tile sits in the middle of a 3x3 grid; each of
 * the eight surrounding slots lists the tiles allowed on that side of it. Tiles are dragged from the
 * palette into a slot to allow them there, between slots to move them (Ctrl: copy), and back onto
 * the palette (or right-clicked) to forbid them again. Dropping a tile on the middle edits it next.
 */

// C++ Standard Library
#include <algorithm>
#include <cstdint>
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

constexpr const char* kPayloadType = "LVL_WFC_TILE";

/// What is being dragged: a rule tile, from the palette or from one of the slots
struct DragPayload
{
  WfcTileId tile = 0;
  /// -1 = the palette; otherwise the Direction of the slot it was dragged out of
  std::int8_t from = -1;
};

constexpr ImU32 kSelected = IM_COL32(255, 230, 60, 255);
constexpr ImU32 kAllowed = IM_COL32(80, 230, 120, 255);
constexpr ImU32 kTarget = IM_COL32(90, 170, 255, 200);

/// The 3x3 grid, row by row; the middle cell (-1) is the tile being edited
constexpr int kGrid[3][3] = {
  {static_cast<int>(Direction::kNorthWest),
   static_cast<int>(Direction::kNorth),
   static_cast<int>(Direction::kNorthEast)},
  {static_cast<int>(Direction::kWest), -1, static_cast<int>(Direction::kEast)},
  {static_cast<int>(Direction::kSouthWest),
   static_cast<int>(Direction::kSouth),
   static_cast<int>(Direction::kSouthEast)},
};

const char* label(Direction d)
{
  switch (d)
  {
  case Direction::kNorth:
    return "North";
  case Direction::kEast:
    return "East";
  case Direction::kSouth:
    return "South";
  case Direction::kWest:
    return "West";
  case Direction::kNorthEast:
    return "North-east";
  case Direction::kSouthEast:
    return "South-east";
  case Direction::kSouthWest:
    return "South-west";
  case Direction::kNorthWest:
    return "North-west";
  }
  return "";
}

const char* tile_name(const Level& lv, const WfcTile& t)
{
  return (t.tile.tileset < lv.tilesets.size() and t.tile.tile < lv.tilesets[t.tile.tileset].tiles.size())
    ? lv.tilesets[t.tile.tileset].tiles[t.tile.tile].name.c_str()
    : "";
}

/// True while a rule tile is being dragged
bool dragging_tile()
{
  const ImGuiPayload* p = ImGui::GetDragDropPayload();
  return p != nullptr and p->IsDataType(kPayloadType);
}

/// Accepts a rule tile dropped on the last item; true (with the payload) once dropped
bool accept_tile(DragPayload& out)
{
  bool dropped = false;
  if (ImGui::BeginDragDropTarget())
  {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayloadType); p != nullptr)
    {
      std::memcpy(&out, p->Data, sizeof(out));
      dropped = true;
    }
    ImGui::EndDragDropTarget();
  }
  return dropped;
}

/**
 * @brief One rule tile as a draggable square
 *
 * Returns true when clicked (pressed and released on it without dragging elsewhere).
 */
bool tile_cell(const App& app, WfcTileId id, float size, std::int8_t from, ImU32 border)
{
  const Level& lv = *app.level;
  const WfcTile& t = lv.wfc.tiles[id];
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2{size, size});
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImTextureID texture{};
  ImVec2 uv0;
  ImVec2 uv1;
  if (tile_uv(app, t.tile, texture, uv0, uv1))
  {
    draw->AddImage(texture, lo, hi, uv0, uv1);
  }
  else
  {
    draw->AddRectFilled(lo, hi, IM_COL32(90, 90, 90, 255));
  }
  if (border != 0)
  {
    draw->AddRect(lo, hi, border, 0, 0, 2.0F);
  }
  if (ImGui::IsItemHovered() and !dragging_tile())
  {
    draw->AddRectFilled(lo, hi, IM_COL32(255, 255, 255, 40));
    ImGui::SetTooltip(
      "#%u  %s (%u:%u)\n%s",
      id,
      tile_name(lv, t),
      t.tile.tileset,
      t.tile.tile,
      (from < 0) ? "Drag onto a side to allow it there; click to edit its neighbors"
                 : "Drag to another side to move it (Ctrl: copy); right-click or drag to the palette to remove");
  }
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
  {
    const DragPayload payload{.tile = id, .from = from};
    ImGui::SetDragDropPayload(kPayloadType, &payload, sizeof(payload));
    tile_image(app, t.tile, 40.0F);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("#%u  %s", id, tile_name(lv, t));
    ImGui::TextDisabled(
      (from < 0) ? "Drop on a side" : (ImGui::GetIO().KeyCtrl ? "Copy to another side" : "Move to another side"));
    ImGui::EndGroup();
    ImGui::EndDragDropSource();
  }
  ImGui::PopID();
  return clicked;
}

/// Lays out square items of <code>size</code> in rows filling the available width
class Flow
{
public:
  explicit Flow(float size) :
      per_row_{std::max(
        1,
        static_cast<int>(
          (ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) /
          (size + ImGui::GetStyle().ItemSpacing.x)))}
  {}

  void next()
  {
    if (n_++ % per_row_ != 0)
    {
      ImGui::SameLine();
    }
  }

private:
  int per_row_;
  int n_ = 0;
};

/// Copies the allowed set of side <code>d</code> to <code>others</code>
template <std::size_t N> void copy_side(WfcRules& rules, WfcTileId a, Direction d, const Direction (&others)[N])
{
  const auto n = static_cast<WfcTileId>(rules.tiles.size());
  for (const Direction other : others)
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
}

/// One of the eight slots around the edited tile; returns true if the rules changed
bool slot(App& app, Direction d, ImVec2 size)
{
  WfcRules& rules = app.level->wfc;
  const WfcTileId a = app.wfc_tile;
  const auto n = static_cast<WfcTileId>(rules.tiles.size());
  const bool ignored = diagonal(d) and !rules.diagonals;
  bool changed = false;
  ImGui::PushID(static_cast<int>(d));

  std::size_t count = 0;
  for (WfcTileId b = 0; b < n; ++b)
  {
    count += allowed(rules, a, b, d) ? 1 : 0;
  }

  if (ignored)
  {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.45F);
  }
  ImGui::BeginChild("slot", size, ImGuiChildFlags_Borders);
  ImGui::TextUnformatted(label(d));
  ImGui::SameLine();
  ImGui::TextDisabled("%zu/%u", count, n);
  if (ignored)
  {
    ImGui::TextDisabled("ignored: corners off");
  }
  constexpr float kThumb = 28.0F;
  Flow flow{kThumb};
  for (WfcTileId b = 0; b < n; ++b)
  {
    if (!allowed(rules, a, b, d))
    {
      continue;
    }
    flow.next();
    tile_cell(app, b, kThumb, static_cast<std::int8_t>(d), 0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
      set_allowed(rules, a, b, d, false);
      changed = true;
    }
  }
  if (count == 0)
  {
    ImGui::TextDisabled("Nothing allowed.\nDrop tiles here.");
  }
  if (ImGui::BeginPopupContextWindow("menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
  {
    ImGui::TextDisabled("%s of #%u", label(d), a);
    if (ImGui::MenuItem("Allow every tile"))
    {
      for (WfcTileId b = 0; b < n; ++b)
      {
        set_allowed(rules, a, b, d, true);
      }
      changed = true;
    }
    if (ImGui::MenuItem("Clear"))
    {
      for (WfcTileId b = 0; b < n; ++b)
      {
        set_allowed(rules, a, b, d, false);
      }
      changed = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy to the four sides"))
    {
      copy_side(rules, a, d, kDirections);
      changed = true;
    }
    if (ImGui::MenuItem("Copy to the four corners"))
    {
      copy_side(rules, a, d, kDiagonals);
      changed = true;
    }
    if (ImGui::MenuItem("Copy to all eight"))
    {
      copy_side(rules, a, d, kNeighbors);
      changed = true;
    }
    ImGui::EndPopup();
  }
  ImGui::EndChild();
  if (ignored)
  {
    ImGui::PopStyleVar();
  }
  if (dragging_tile())
  {
    ImGui::GetWindowDrawList()->AddRect(
      ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), kTarget, ImGui::GetStyle().ChildRounding, 0, 1.0F);
  }
  if (DragPayload drop; accept_tile(drop) and drop.tile < n)
  {
    set_allowed(rules, a, drop.tile, d, true);
    if (drop.from >= 0 and drop.from != static_cast<int>(d) and !ImGui::GetIO().KeyCtrl)
    {
      set_allowed(rules, a, drop.tile, static_cast<Direction>(drop.from), false);
    }
    changed = true;
  }
  ImGui::PopID();
  return changed;
}

/// The middle of the grid: the edited tile; dropping another tile here edits that one instead
void center(App& app, ImVec2 size)
{
  const Level& lv = *app.level;
  const WfcTile& t = lv.wfc.tiles[app.wfc_tile];
  ImGui::BeginChild("center", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  const float text = ImGui::GetTextLineHeightWithSpacing();
  const float side = std::max(16.0F, std::min(size.x, size.y) - 2.0F * text - 2.0F * ImGui::GetStyle().WindowPadding.y);
  ImGui::SetCursorPosX((size.x - side) * 0.5F);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  tile_image(app, t.tile, side);
  ImGui::GetWindowDrawList()->AddRect(p, ImVec2{p.x + side, p.y + side}, kSelected, 0, 0, 2.0F);
  ImGui::Text("#%u  %s", app.wfc_tile, tile_name(lv, t));
  ImGui::TextDisabled("weight %g", static_cast<double>(t.weight));
  ImGui::EndChild();
  if (ImGui::IsItemHovered() and !dragging_tile())
  {
    ImGui::SetTooltip("The tile whose neighbors are edited. Drop a tile here to edit it instead.");
  }
  if (DragPayload drop; accept_tile(drop) and drop.tile < lv.wfc.tiles.size())
  {
    app.wfc_tile = drop.tile;
  }
}

/// The palette of every rule tile (drag sources); dropping a tile from a slot here removes it there
bool palette(App& app, float width)
{
  WfcRules& rules = app.level->wfc;
  const auto n = static_cast<WfcTileId>(rules.tiles.size());
  bool changed = false;
  ImGui::BeginChild("palette", ImVec2{width, 0}, ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Rule tiles");
  ImGui::TextDisabled("drag onto a side");
  constexpr float kThumb = 36.0F;
  Flow flow{kThumb};
  for (WfcTileId b = 0; b < n; ++b)
  {
    flow.next();
    if (tile_cell(app, b, kThumb, -1, (b == app.wfc_tile) ? kSelected : 0))
    {
      app.wfc_tile = b;
    }
  }
  ImGui::EndChild();
  if (DragPayload drop; accept_tile(drop) and drop.tile < n and drop.from >= 0)
  {
    set_allowed(rules, app.wfc_tile, drop.tile, static_cast<Direction>(drop.from), false);
    changed = true;
  }
  return changed;
}

}  // namespace

void wfc_neighbors_window(App& app)
{
  ImGui::SetNextWindowSize(ImVec2{820, 640}, ImGuiCond_FirstUseEver);
  const ImVec2 middle = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(middle, ImGuiCond_FirstUseEver, ImVec2{0.5F, 0.5F});
  // Floats over the viewport: opaque, so the map doesn't show through
  ImGui::SetNextWindowBgAlpha(1.0F);
  if (!ImGui::Begin("WFC neighbors", &app.panels.wfc_neighbors))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  Level& lv = *app.level;
  WfcRules& rules = lv.wfc;
  if (rules.tiles.empty())
  {
    ImGui::TextDisabled(
      "No rule tiles. Add tiles from the palette in Generate > WFC rules, or learn them from a layer.");
    ImGui::End();
    return;
  }
  app.wfc_tile = std::min<WfcTileId>(app.wfc_tile, static_cast<WfcTileId>(rules.tiles.size() - 1));
  bool changed = false;

  // Toolbar: step through tiles, corner rules
  if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
  {
    app.wfc_tile = (app.wfc_tile == 0) ? static_cast<WfcTileId>(rules.tiles.size() - 1) : app.wfc_tile - 1;
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##next", ImGuiDir_Right))
  {
    app.wfc_tile = static_cast<WfcTileId>((app.wfc_tile + 1) % rules.tiles.size());
  }
  ImGui::SameLine();
  ImGui::Text(
    "%s: tile %u of %zu", rules.name.empty() ? "WFC rules" : rules.name.c_str(), app.wfc_tile + 1, rules.tiles.size());
  ImGui::SameLine(0.0F, 2.0F * ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::Checkbox("Constrain corners", &rules.diagonals))
  {
    changed = true;
    if (rules.diagonals and diagonal_count(rules) == 0)
    {
      // Starting with no corner rules would forbid every corner pair: derive them from the sides
      derive_diagonals(rules, app.memory.frame);
      LVL_LOG_INFO() << "Derived " << diagonal_count(rules) << " corner rules from the side rules";
    }
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip(
      "Also constrain the four corner neighbors (8-way adjacency).\nOff: only the four sides are constrained "
      "and corner rules are kept but ignored.");
  }
  ImGui::SameLine();
  if (ImGui::Button("Derive corners"))
  {
    derive_diagonals(rules, app.memory.frame);
    changed = true;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip(
      "Allow every corner pair the side rules can produce:\nB may be south-east of A if some C fits east of A "
      "with B south of C, or south of A with B east of C.");
  }

  // Palette | 3x3 grid
  const float palette_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.22F, 120.0F, 260.0F);
  changed |= palette(app, palette_width);
  ImGui::SameLine();
  ImGui::BeginGroup();
  const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float hint = ImGui::GetTextLineHeightWithSpacing();
  const ImVec2 cell{
    std::max(60.0F, (avail.x - 2.0F * spacing.x) / 3.0F), std::max(60.0F, (avail.y - hint - 3.0F * spacing.y) / 3.0F)};
  for (const auto& row : kGrid)
  {
    for (int c = 0; c < 3; ++c)
    {
      if (c > 0)
      {
        ImGui::SameLine();
      }
      if (row[c] < 0)
      {
        center(app, cell);
      }
      else
      {
        changed |= slot(app, static_cast<Direction>(row[c]), cell);
      }
    }
  }
  ImGui::TextDisabled("Symmetric: B east of A also puts A west of B. Right-click a side for more.");
  ImGui::EndGroup();

  app.modified |= changed;
  ImGui::End();
}

}  // namespace lvl::editor
