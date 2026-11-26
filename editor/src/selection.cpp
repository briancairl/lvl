/**
 * @copyright 2026-present Brian Cairl
 *
 * @file selection.cpp
 *
 * Readers of the palette selection (App::selection), shared by the batch metadata edit in the tile
 * panel, the paint stamp and the WFC rules taken from the palette layout.
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "internal.hpp"

namespace lvl::editor
{

using namespace level;

Extents palette_grid(const App& app, std::size_t tileset)
{
  const project::Project& pr = *app.project;
  if (tileset >= pr.tilesets.size())
  {
    return Extents{0, 0};
  }
  const TileSet& ts = pr.tilesets[tileset];
  return slice_grid(ts.slice, ts.image_width, ts.image_height);
}

memory::Vector<TileIndex> selected_tiles(const App& app, memory::Arena& arena)
{
  auto out = memory::make_vector<TileIndex>(arena);
  const project::Project& pr = *app.project;
  if (app.selection.tileset >= pr.tilesets.size())
  {
    return out;
  }
  const std::size_t tiles = pr.tilesets[app.selection.tileset].tiles.size();
  out.reserve(app.selection.count);
  for (std::size_t i = 0; i < tiles; ++i)
  {
    if (app.selection.has(static_cast<TileIndex>(i)))
    {
      out.push_back(static_cast<TileIndex>(i));
    }
  }
  return out;
}

memory::Vector<WfcTileId> selected_rule_tiles(const App& app, memory::Arena& arena)
{
  auto out = memory::make_vector<WfcTileId>(arena);
  const WfcRules& rules = rule_set(app).wfc;
  for (std::size_t i = 0; i < rules.tiles.size(); ++i)
  {
    const TileRef tile = rules.tiles[i].tile;
    if (app.selection.has(tile.tileset, tile.tile))
    {
      out.push_back(static_cast<WfcTileId>(i));
    }
  }
  return out;
}

bool selection_bounds(const App& app, SelectionBounds& out)
{
  const Extents grid = palette_grid(app, app.selection.tileset);
  if (app.selection.count == 0 or grid.width <= 0)
  {
    return false;
  }
  const std::size_t tiles = app.project->tilesets[app.selection.tileset].tiles.size();
  int x0 = grid.width;
  int y0 = grid.height;
  int x1 = -1;
  int y1 = -1;
  for (std::size_t i = 0; i < tiles; ++i)
  {
    if (!app.selection.has(static_cast<TileIndex>(i)))
    {
      continue;
    }
    const int x = static_cast<int>(i) % grid.width;
    const int y = static_cast<int>(i) / grid.width;
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
  }
  if (x1 < 0)
  {
    return false;
  }
  out = SelectionBounds{.x = x0, .y = y0, .width = x1 - x0 + 1, .height = y1 - y0 + 1, .columns = grid.width};
  return true;
}

bool deselect_all_button(App& app)
{
  const bool empty = app.selection.count == 0;
  ImGui::BeginDisabled(empty);
  const bool clicked = ImGui::SmallButton("Deselect all");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
  {
    ImGui::SetTooltip(
      empty ? "No palette tiles are selected" : "Clears the palette selection (Ctrl+Shift+A), ending the batch edit");
  }
  if (clicked)
  {
    app.selection.clear();
  }
  return clicked;
}

void selection_preview(const App& app, float thumb, int max_rows)
{
  SelectionBounds bounds;
  if (!selection_bounds(app, bounds))
  {
    return;
  }
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float rows = static_cast<float>(std::min(max_rows, bounds.height));
  ImGui::BeginChild(
    "selection",
    ImVec2{0, rows * (thumb + spacing) + spacing},
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_HorizontalScrollbar);
  for (int y = 0; y < bounds.height; ++y)
  {
    for (int x = 0; x < bounds.width; ++x)
    {
      const auto tile = static_cast<TileIndex>((bounds.y + y) * bounds.columns + (bounds.x + x));
      if (x > 0)
      {
        ImGui::SameLine();
      }
      if (app.selection.has(tile))
      {
        tile_image(app, TileRef{static_cast<TileSetId>(app.selection.tileset), tile}, thumb);
      }
      else
      {
        ImGui::Dummy(ImVec2{thumb, thumb});
      }
    }
  }
  ImGui::EndChild();
}

const TileGroup* chosen_group(const App& app)
{
  const project::Project& pr = *app.project;
  if (!app.group.active() or app.group.tileset >= pr.tilesets.size())
  {
    return nullptr;
  }
  const TileSet& ts = pr.tilesets[app.group.tileset];
  if (app.group.group >= ts.groups.size())
  {
    return nullptr;
  }
  return &ts.groups[app.group.group];
}

void choose_group(App& app, GroupId group)
{
  app.group.tileset = app.tileset;
  app.group.group = group;
  if (group == kNoGroup)
  {
    return;
  }
  app.selection.clear();
  if (app.tool != Tool::kPaint and app.tool != Tool::kRect)
  {
    app.tool = Tool::kPaint;
  }
}

bool group_stamp(const App& app, Grid<TileRef>& out)
{
  const TileGroup* group = chosen_group(app);
  if (group == nullptr)
  {
    return false;
  }
  const TileSet& ts = app.project->tilesets[app.group.tileset];
  out.assign(group->size, kEmptyTile);
  for (int y = 0; y < group->size.height; ++y)
  {
    for (int x = 0; x < group->size.width; ++x)
    {
      // A hole keeps whatever is under it, which is how a tree's bounding box corners behave
      if (const TileIndex tile = group_cell(ts, *group, x, y); tile != kNoTileIndex)
      {
        out.at(x, y) = TileRef{static_cast<TileSetId>(app.group.tileset), tile};
      }
    }
  }
  return true;
}

Extents stamp_extents(const App& app)
{
  if (const TileGroup* group = chosen_group(app); group != nullptr)
  {
    return group->size;
  }
  SelectionBounds bounds;
  if (app.selection.count >= 2 and selection_bounds(app, bounds))
  {
    return Extents{bounds.width, bounds.height};
  }
  return Extents{std::max(1, app.paint.stamp_width), std::max(1, app.paint.stamp_height)};
}

bool paint_stamp(const App& app, Grid<TileRef>& out)
{
  // A chosen tile group is a thing the user named, so it is what a stamp means
  if (group_stamp(app, out))
  {
    return true;
  }
  // Two or more selected tiles are painted as the palette lays them out, holes and all
  if (app.selection.count >= 2 and palette_layout(app, out))
  {
    return true;
  }
  const project::Project& pr = *app.project;
  const TileRef base = app.paint.tile;
  if (base.empty() or base.tileset >= pr.tilesets.size())
  {
    return false;
  }
  const TileSet& ts = pr.tilesets[base.tileset];
  const int columns = std::max(1, palette_grid(app, base.tileset).width);
  const Extents e = Extents{std::max(1, app.paint.stamp_width), std::max(1, app.paint.stamp_height)};
  out.assign(e, kEmptyTile);
  for (int y = 0; y < e.height; ++y)
  {
    for (int x = 0; x < e.width; ++x)
    {
      const int index = static_cast<int>(base.tile) + y * columns + x;
      if (index < static_cast<int>(ts.tiles.size()))
      {
        out.at(x, y) = TileRef{base.tileset, static_cast<TileIndex>(index)};
      }
    }
  }
  return true;
}

bool palette_layout(const App& app, Grid<TileRef>& out)
{
  SelectionBounds bounds;
  if (!selection_bounds(app, bounds))
  {
    return false;
  }
  out.assign(Extents{bounds.width, bounds.height}, kEmptyTile);
  for (int y = 0; y < bounds.height; ++y)
  {
    for (int x = 0; x < bounds.width; ++x)
    {
      const int index = (bounds.y + y) * bounds.columns + (bounds.x + x);
      if (app.selection.has(static_cast<TileIndex>(index)))
      {
        out.at(x, y) = TileRef{static_cast<TileSetId>(app.selection.tileset), static_cast<TileIndex>(index)};
      }
    }
  }
  return true;
}

}  // namespace lvl::editor
