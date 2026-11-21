/**
 * @copyright 2026-present Brian Cairl
 *
 * @file selection.cpp
 *
 * Readers of the palette selection (App::selection), shared by the batch metadata edit in the tile
 * panel and the WFC rules taken from the palette layout.
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
  const Level& lv = *app.level;
  if (tileset >= lv.tilesets.size())
  {
    return Extents{0, 0};
  }
  const TileSet& ts = lv.tilesets[tileset];
  return slice_grid(ts.slice, ts.image_width, ts.image_height);
}

memory::Vector<TileIndex> selected_tiles(const App& app, memory::Arena& arena)
{
  auto out = memory::make_vector<TileIndex>(arena);
  const Level& lv = *app.level;
  if (app.selection.tileset >= lv.tilesets.size())
  {
    return out;
  }
  const std::size_t tiles = lv.tilesets[app.selection.tileset].tiles.size();
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
  const WfcRules& rules = app.level->wfc;
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
  const std::size_t tiles = app.level->tilesets[app.selection.tileset].tiles.size();
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
