/**
 * @copyright 2026-present Brian Cairl
 *
 * @file viewport.cpp
 *
 * The map canvas: navigation (pan with middle mouse / space+drag / pan tool / arrow keys; zoom with
 * the wheel around the cursor), drawing (tiles from atlases when zoomed in, per-layer overlay
 * textures for other kinds and when zoomed out), objects, and the editing tools (including
 * auto-tiled painting, which re-fills the band around what was painted from the adjacency rules).
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/procgen/autotile.hpp"
#include "lvl/render/preview.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

/// Per-frame viewport geometry
struct Canvas
{
  ImVec2 origin;
  ImVec2 size;
  float scale;
  ImDrawList* draw;

  [[nodiscard]] ImVec2 to_screen(const View& v, float cx, float cy) const
  {
    return ImVec2{origin.x + (cx - v.x) * scale, origin.y + (cy - v.y) * scale};
  }

  [[nodiscard]] ImVec2 to_cells(const View& v, ImVec2 p) const
  {
    return ImVec2{v.x + (p.x - origin.x) / scale, v.y + (p.y - origin.y) / scale};
  }
};

ImU32 to_u32(image::Rgba c, float alpha = 1.0F)
{
  return IM_COL32(c.r, c.g, c.b, static_cast<int>(static_cast<float>(c.a) * alpha));
}

/// Stroke state across frames
struct Stroke
{
  bool active = false;
  Cell last = {-1, -1};
  /// Rect / select tool drag start
  Cell anchor = {0, 0};
  bool dragging_rect = false;
  /// Select tool: dragging an object
  bool moving = false;
  ImVec2 grab_offset = {0, 0};
};

Stroke stroke;

image::Rgba tint_of(const App& app)
{
  if (app.view.tint and app.view.season < app.project->seasons.size())
  {
    return app.project->seasons[app.view.season].tint;
  }
  return image::Rgba{255, 255, 255, 255};
}

ImU32 modulate(image::Rgba tint, float alpha)
{
  return IM_COL32(tint.r, tint.g, tint.b, static_cast<int>(255.0F * alpha));
}

/// Rebuilds dirty overlay textures (one texel per cell)
void update_overlays(App& app)
{
  const project::Project& pr = *app.project;
  const Level& lv = *app.level;
  const Extents e = lv.settings.extents;
  const auto atlases = app.assets.atlases(pr.tilesets.size());
  for (std::size_t i = 0; i < lv.layers.size() and i < kMaxLayers; ++i)
  {
    if (!app.overlays.all_dirty and !app.overlays.dirty[i])
    {
      continue;
    }
    memory::Scope scope{app.memory.frame};
    image::Image img = image::create(app.memory.frame, e.width, e.height);
    render::colorize(*app.project, lv.layers[i], img, &atlases, view_season(app));
    upload(app.overlays.textures[i], img);
    app.overlays.dirty[i] = false;
  }
  app.overlays.all_dirty = false;
}

/// Applies the brush to one cell of the current layer
void apply_cell(App& app, Layer& layer, Cell c, bool erase, float weight)
{
  const Extents e = extents(layer);
  if (!e.contains(c))
  {
    return;
  }
  switch (layer.kind)
  {
  case LayerKind::kTiles: {
    auto& g = plane<TileGrid>(layer);
    g.at(c) = erase ? kEmptyTile : app.paint.tile;
    break;
  }
  case LayerKind::kSemantic:
    plane<IdGrid>(layer).at(c) = erase ? 0 : app.paint.semantic;
    break;
  case LayerKind::kMaterial:
    plane<IdGrid>(layer).at(c) = erase ? 0 : app.paint.material;
    break;
  case LayerKind::kCollision:
    plane<MaskGrid>(layer).at(c) = erase ? 0 : app.paint.collision;
    break;
  case LayerKind::kHeight:
  case LayerKind::kWater: {
    auto& g = plane<ScalarGrid>(layer);
    float& v = g.at(c);
    const float s = app.brush.strength * weight;
    if (erase)
    {
      v = 0.0F;
      break;
    }
    switch (app.brush.scalar_mode)
    {
    case ScalarMode::kSet:
      v = app.brush.value;
      break;
    case ScalarMode::kRaise:
      v += s;
      break;
    case ScalarMode::kLower:
      v -= s;
      break;
    case ScalarMode::kSmooth: {
      float sum = 0.0F;
      int n = 0;
      for (int dy = -1; dy <= 1; ++dy)
      {
        for (int dx = -1; dx <= 1; ++dx)
        {
          if (e.contains(c.x + dx, c.y + dy))
          {
            sum += g.at(c.x + dx, c.y + dy);
            ++n;
          }
        }
      }
      v += (sum / static_cast<float>(n) - v) * std::min(1.0F, s * 10.0F);
      break;
    }
    case ScalarMode::kFlatten:
      v += (app.brush.value - v) * std::min(1.0F, s * 10.0F);
      break;
    }
    break;
  }
  }
}

/// Applies the brush: a tile stamp with its top-left corner at the cell, or the radius brush
/// (<code>touched</code>, if given, collects the cells written, for auto-tiling)
void apply_brush(
  App& app,
  Cell center,
  bool erase,
  const Grid<TileRef>* stamp = nullptr,
  memory::Vector<Cell>* touched = nullptr)
{
  Layer& layer = app.level->layers[app.layer];
  if (layer.kind == LayerKind::kTiles and !erase and stamp != nullptr and stamp->size() > 1)
  {
    // Stamp the tiles as the palette lays them out, keeping their relative positions; a cell the
    // stamp leaves empty (a hole in the palette selection) keeps what is under it
    auto& g = plane<TileGrid>(layer);
    for (int dy = 0; dy < stamp->height(); ++dy)
    {
      for (int dx = 0; dx < stamp->width(); ++dx)
      {
        const TileRef tile = stamp->at(dx, dy);
        const Cell c{center.x + dx, center.y + dy};
        if (!tile.empty() and g.extents.contains(c))
        {
          g.at(c) = tile;
          if (touched != nullptr)
          {
            touched->push_back(c);
          }
        }
      }
    }
    return;
  }
  const int r = app.brush.radius;
  for (int dy = -r; dy <= r; ++dy)
  {
    for (int dx = -r; dx <= r; ++dx)
    {
      const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
      if (d > static_cast<float>(r) + 0.5F)
      {
        continue;
      }
      // Scalar brushes fall off towards the rim
      const float weight = (r == 0) ? 1.0F : std::clamp(1.0F - d / (static_cast<float>(r) + 1.0F), 0.0F, 1.0F);
      const Cell c{center.x + dx, center.y + dy};
      apply_cell(app, layer, c, erase, weight);
      if (touched != nullptr and extents(layer).contains(c))
      {
        touched->push_back(c);
      }
    }
  }
}

/// Applies the brush along the line from a to b (so fast strokes leave no gaps)
void apply_line(
  App& app,
  Cell a,
  Cell b,
  bool erase,
  const Grid<TileRef>* stamp = nullptr,
  memory::Vector<Cell>* touched = nullptr)
{
  const int steps = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
  for (int i = (a == b) ? 0 : 1; i <= steps; ++i)
  {
    const float t = (steps == 0) ? 0.0F : static_cast<float>(i) / static_cast<float>(steps);
    apply_brush(
      app,
      Cell{static_cast<int>(std::lround(a.x + (b.x - a.x) * t)), static_cast<int>(std::lround(a.y + (b.y - a.y) * t))},
      erase,
      stamp,
      touched);
  }
}

/// Generic cell equality for flood fill
bool same_value(const Layer& layer, std::size_t a, std::size_t b)
{
  return std::visit([a, b](const auto& g) { return g.values[a] == g.values[b]; }, layer.data);
}

void flood_fill(App& app, Cell start)
{
  Layer& layer = app.level->layers[app.layer];
  const Extents e = extents(layer);
  if (!e.contains(start))
  {
    return;
  }
  memory::Scope scope{app.memory.frame};
  auto visited = memory::make_vector<std::uint8_t>(app.memory.frame, e.count(), std::uint8_t{0});
  auto stack = memory::make_vector<std::uint32_t>(app.memory.frame);
  auto region = memory::make_vector<std::uint32_t>(app.memory.frame);
  const std::size_t seed = e.index(start);
  stack.push_back(static_cast<std::uint32_t>(seed));
  visited[seed] = 1;
  while (!stack.empty())
  {
    const std::uint32_t i = stack.back();
    stack.pop_back();
    region.push_back(i);
    const Cell c = e.cell(i);
    for (const Direction d : kDirections)
    {
      const Cell n = step(c, d);
      if (e.contains(n) and visited[e.index(n)] == 0 and same_value(layer, seed, e.index(n)))
      {
        visited[e.index(n)] = 1;
        stack.push_back(static_cast<std::uint32_t>(e.index(n)));
      }
    }
  }
  const int saved_radius = app.brush.radius;
  const ScalarMode saved_mode = app.brush.scalar_mode;
  app.brush.radius = 0;
  if (is_scalar(layer.kind))
  {
    app.brush.scalar_mode = ScalarMode::kSet;
  }
  for (const std::uint32_t i : region)
  {
    apply_cell(app, layer, e.cell(i), false, 1.0F);
  }
  app.brush.radius = saved_radius;
  app.brush.scalar_mode = saved_mode;
}

void fill_rect(App& app, Cell a, Cell b, bool erase)
{
  Layer& layer = app.level->layers[app.layer];
  const ScalarMode saved_mode = app.brush.scalar_mode;
  if (is_scalar(layer.kind))
  {
    app.brush.scalar_mode = ScalarMode::kSet;
  }
  for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
  {
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
    {
      apply_cell(app, layer, Cell{x, y}, erase, 1.0F);
    }
  }
  app.brush.scalar_mode = saved_mode;
}

void eyedrop(App& app, Cell c)
{
  const Layer& layer = app.level->layers[app.layer];
  if (!extents(layer).contains(c))
  {
    return;
  }
  switch (layer.kind)
  {
  case LayerKind::kTiles: {
    const TileRef t = plane<TileGrid>(layer).at(c);
    if (!t.empty())
    {
      app.paint.tile = t;
      app.paint.stamp_width = 1;
      app.paint.stamp_height = 1;
      app.tileset = t.tileset;
      app.tile = t.tile;
      // A multi-tile selection would otherwise keep being what painting writes
      app.selection.clear();
      app.selection.retarget(t.tileset);
    }
    break;
  }
  case LayerKind::kSemantic:
    app.paint.semantic = plane<IdGrid>(layer).at(c);
    break;
  case LayerKind::kMaterial:
    app.paint.material = plane<IdGrid>(layer).at(c);
    break;
  case LayerKind::kCollision:
    app.paint.collision = plane<MaskGrid>(layer).at(c);
    break;
  case LayerKind::kHeight:
  case LayerKind::kWater:
    app.brush.value = plane<ScalarGrid>(layer).at(c);
    break;
  }
}

/// Value under the cursor, for the status line
const char* describe(const App& app, Cell c)
{
  const Layer& layer = app.level->layers[app.layer];
  if (!extents(layer).contains(c))
  {
    return "";
  }
  switch (layer.kind)
  {
  case LayerKind::kTiles: {
    const TileRef t = plane<TileGrid>(layer).at(c);
    if (t.empty())
    {
      return "empty";
    }
    const char* name =
      (t.tileset < app.project->tilesets.size() and t.tile < app.project->tilesets[t.tileset].tiles.size())
      ? app.project->tilesets[t.tileset].tiles[t.tile].name.c_str()
      : "";
    return format<160>("tile %u:%u %s", t.tileset, t.tile, name);
  }
  case LayerKind::kSemantic: {
    const auto id = plane<IdGrid>(layer).at(c);
    return format<160>("%s", id < app.project->semantics.size() ? app.project->semantics[id].name.c_str() : "?");
  }
  case LayerKind::kMaterial: {
    const auto id = plane<IdGrid>(layer).at(c);
    return format<160>("%s", id < app.project->materials.size() ? app.project->materials[id].name.c_str() : "?");
  }
  case LayerKind::kCollision:
    return format<160>("mask 0x%02x", plane<MaskGrid>(layer).at(c));
  case LayerKind::kHeight:
  case LayerKind::kWater:
    return format<160>("%.4f", static_cast<double>(plane<ScalarGrid>(layer).at(c)));
  }
  return "";
}

void draw_layers(App& app, const Canvas& cv)
{
  const project::Project& pr = *app.project;
  const Level& lv = *app.level;
  const View& v = app.view;
  const Extents e = lv.settings.extents;
  const image::Rgba tint = tint_of(app);
  const ImVec2 map_min = cv.to_screen(v, 0, 0);
  const ImVec2 map_max = cv.to_screen(v, static_cast<float>(e.width), static_cast<float>(e.height));
  cv.draw->AddRectFilled(map_min, map_max, IM_COL32(24, 24, 28, 255));

  // Visible cell range
  const int x0 = std::max(0, static_cast<int>(std::floor(v.x)));
  const int y0 = std::max(0, static_cast<int>(std::floor(v.y)));
  const int x1 = std::min(e.width, static_cast<int>(std::ceil(v.x + cv.size.x / cv.scale)) + 1);
  const int y1 = std::min(e.height, static_cast<int>(std::ceil(v.y + cv.size.y / cv.scale)) + 1);
  const bool detailed =
    cv.scale >= 6.0F and static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0) <= 60000;

  // Seasonal tile lookups for this frame
  memory::Vector<TileIndex>* seasonal =
    memory::allocate<memory::Vector<TileIndex>>(app.memory.frame, pr.tilesets.size());
  for (std::size_t t = 0; t < pr.tilesets.size(); ++t)
  {
    new (&seasonal[t]) memory::Vector<TileIndex>{memory::ArenaAllocator<TileIndex>{app.memory.frame}};
    resolve_season(pr.tilesets[t], view_season(app), seasonal[t]);
  }

  for (std::size_t i = 0; i < lv.layers.size() and i < kMaxLayers; ++i)
  {
    const Layer& layer = lv.layers[i];
    if (!layer.visible)
    {
      continue;
    }
    if (layer.kind == LayerKind::kTiles and detailed)
    {
      const auto& g = plane<TileGrid>(layer);
      const ImU32 col = modulate(tint, layer.opacity);
      for (int y = y0; y < y1; ++y)
      {
        for (int x = x0; x < x1; ++x)
        {
          const TileRef t = g.at(x, y);
          if (t.empty() or t.tileset >= pr.tilesets.size())
          {
            continue;
          }
          const Texture& tex = app.assets.textures[t.tileset];
          const ImVec2 p0 = cv.to_screen(v, static_cast<float>(x), static_cast<float>(y));
          const ImVec2 p1{p0.x + cv.scale, p0.y + cv.scale};
          const TileIndex drawn = (t.tile < seasonal[t.tileset].size()) ? seasonal[t.tileset][t.tile] : t.tile;
          if (!tex.valid())
          {
            cv.draw->AddRectFilled(p0, p1, to_u32(render::id_color(t.tileset * 65536U + drawn), layer.opacity));
            continue;
          }
          const TileSet& ts = pr.tilesets[t.tileset];
          const TileRect r = tile_rect(ts.slice, tex.width, tex.height, drawn);
          const ImVec2 uv0{static_cast<float>(r.x) / tex.width, static_cast<float>(r.y) / tex.height};
          const ImVec2 uv1{
            static_cast<float>(r.x + r.width) / tex.width, static_cast<float>(r.y + r.height) / tex.height};
          cv.draw->AddImage(texture_id(tex), p0, p1, uv0, uv1, col);
        }
      }
      continue;
    }
    // Overlay texture: one texel per cell
    const Texture& tex = app.overlays.textures[i];
    if (tex.valid())
    {
      cv.draw->AddImage(texture_id(tex), map_min, map_max, ImVec2{0, 0}, ImVec2{1, 1}, modulate(tint, layer.opacity));
    }
  }

  if (v.grid and cv.scale >= 8.0F)
  {
    const ImU32 grid = IM_COL32(255, 255, 255, 28);
    // Lines snapped to pixel centers, so they all look alike (no anti-aliasing smear)
    const auto snap = [](float p) { return std::floor(p) + 0.5F; };
    for (int x = x0; x <= x1; ++x)
    {
      const ImVec2 a = cv.to_screen(v, static_cast<float>(x), static_cast<float>(y0));
      cv.draw->AddLine(ImVec2{snap(a.x), a.y}, ImVec2{snap(a.x), cv.to_screen(v, 0, static_cast<float>(y1)).y}, grid);
    }
    for (int y = y0; y <= y1; ++y)
    {
      const ImVec2 a = cv.to_screen(v, static_cast<float>(x0), static_cast<float>(y));
      cv.draw->AddLine(ImVec2{a.x, snap(a.y)}, ImVec2{cv.to_screen(v, static_cast<float>(x1), 0).x, snap(a.y)}, grid);
    }
  }
  cv.draw->AddRect(map_min, map_max, IM_COL32(255, 255, 255, 90));
}

void draw_objects(App& app, const Canvas& cv)
{
  const Level& lv = *app.level;
  const Registry& reg = lv.registry;
  const View& v = app.view;
  const float node_radius = std::clamp(cv.scale * 0.25F, 3.0F, 9.0F);

  // Route preview between route_from and the selected node
  memory::Vector<Entity> route = memory::make_vector<Entity>(app.memory.frame);
  if (reg.valid(app.route_from) and reg.valid(app.entity) and reg.all_of<PathNode>(app.entity))
  {
    shortest_path(reg, app.route_from, app.entity, route);
  }
  for (auto [e, edge] : reg.view<PathEdge>().each())
  {
    if (!reg.valid(edge.from) or !reg.valid(edge.to))
    {
      continue;
    }
    const GraphId g = reg.get<PathNode>(edge.from).graph;
    if (g >= lv.graphs.size() or !lv.graphs[g].visible)
    {
      continue;
    }
    const auto& a = reg.get<Position>(edge.from);
    const auto& b = reg.get<Position>(edge.to);
    const ImVec2 pa = cv.to_screen(v, a.x, a.y);
    const ImVec2 pb = cv.to_screen(v, b.x, b.y);
    bool on_route = false;
    for (std::size_t k = 1; k < route.size(); ++k)
    {
      on_route = on_route or (route[k - 1] == edge.from and route[k] == edge.to) or
        (route[k - 1] == edge.to and route[k] == edge.from);
    }
    const float thickness = (on_route ? 4.0F : 2.0F) * (edge.cost > 1.0F ? 0.6F : 1.0F);
    cv.draw->AddLine(pa, pb, on_route ? IM_COL32(255, 255, 255, 255) : to_u32(lv.graphs[g].color), thickness);
    if (!edge.bidirectional)
    {
      // Arrow head at the midpoint
      const ImVec2 m{(pa.x + pb.x) * 0.5F, (pa.y + pb.y) * 0.5F};
      const float dx = pb.x - pa.x;
      const float dy = pb.y - pa.y;
      const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-3F);
      const ImVec2 d{dx / len * 8.0F, dy / len * 8.0F};
      cv.draw->AddTriangleFilled(
        ImVec2{m.x + d.x, m.y + d.y},
        ImVec2{m.x - d.y * 0.6F, m.y + d.x * 0.6F},
        ImVec2{m.x + d.y * 0.6F, m.y - d.x * 0.6F},
        to_u32(lv.graphs[g].color));
    }
  }
  for (auto [e, pos, node] : reg.view<Position, PathNode>().each())
  {
    if (node.graph >= lv.graphs.size() or !lv.graphs[node.graph].visible)
    {
      continue;
    }
    const ImVec2 p = cv.to_screen(v, pos.x, pos.y);
    cv.draw->AddCircleFilled(p, node_radius, to_u32(lv.graphs[node.graph].color));
    if (e == app.entity or e == app.edge_start)
    {
      cv.draw->AddCircle(p, node_radius + 3.0F, IM_COL32(255, 255, 255, 255), 0, 2.0F);
    }
    if (e == app.route_from)
    {
      cv.draw->AddCircle(p, node_radius + 6.0F, IM_COL32(255, 255, 255, 160), 0, 1.0F);
    }
  }
  for (auto [e, pos, ext, label, portal] : reg.view<Position, Extent, Label, Portal>().each())
  {
    const ImVec2 p0 = cv.to_screen(v, pos.x, pos.y);
    const ImVec2 p1 = cv.to_screen(v, pos.x + ext.width, pos.y + ext.height);
    // A destination the project cannot follow is drawn red here as well as in the objects list, so
    // a broken link is visible where the level is being edited
    const auto link = portal_link_of(app, portal);
    const bool dangling = link != project::LinkState::kResolved and link != project::LinkState::kSelf;
    const ImU32 edge = dangling ? IM_COL32(245, 115, 100, 255) : IM_COL32(80, 220, 255, 255);
    const ImU32 text = dangling ? IM_COL32(255, 180, 170, 255) : IM_COL32(200, 245, 255, 255);
    cv.draw->AddRectFilled(p0, p1, dangling ? IM_COL32(245, 115, 100, 40) : IM_COL32(80, 220, 255, 50));
    cv.draw->AddRect(p0, p1, e == app.entity ? IM_COL32(255, 255, 255, 255) : edge, 0, 0, 2.0F);
    cv.draw->AddText(ImVec2{p0.x + 2, p0.y - 16}, text, label.name.c_str());
    // Exit direction tick
    const ImVec2 c{(p0.x + p1.x) * 0.5F, (p0.y + p1.y) * 0.5F};
    const Cell d = step(Cell{0, 0}, portal.exit);
    cv.draw->AddLine(c, ImVec2{c.x + d.x * 12.0F, c.y + d.y * 12.0F}, text, 2.0F);
  }
  for (auto [e, pos, label, marker] : reg.view<Position, Label, Marker>().each())
  {
    const ImVec2 p = cv.to_screen(v, pos.x, pos.y);
    cv.draw->AddTriangleFilled(
      ImVec2{p.x, p.y + 2}, ImVec2{p.x - 6, p.y - 10}, ImVec2{p.x + 6, p.y - 10}, IM_COL32(255, 120, 200, 255));
    if (e == app.entity)
    {
      cv.draw->AddCircle(ImVec2{p.x, p.y - 5}, 10.0F, IM_COL32(255, 255, 255, 255), 0, 2.0F);
    }
    cv.draw->AddText(ImVec2{p.x + 8, p.y - 14}, IM_COL32(255, 200, 235, 255), label.name.c_str());
  }
}

/// The node nearest a point (within a few screen pixels), or null
Entity node_at(const App& app, const Canvas& cv, ImVec2 cells)
{
  Entity best = kNullEntity;
  float best_d = 10.0F / cv.scale;
  for (auto [e, pos, node] : app.level->registry.view<Position, PathNode>().each())
  {
    const float d = std::hypot(pos.x - cells.x, pos.y - cells.y);
    if (d < best_d)
    {
      best_d = d;
      best = e;
    }
  }
  return best;
}

void handle_tools(App& app, const Canvas& cv, bool hovered)
{
  ImGuiIO& io = ImGui::GetIO();
  Level& lv = *app.level;
  const ImVec2 mouse_cells = cv.to_cells(app.view, io.MousePos);
  const Cell cell{static_cast<int>(std::floor(mouse_cells.x)), static_cast<int>(std::floor(mouse_cells.y))};
  const bool layer_ok = app.layer < lv.layers.size();
  const bool editable = layer_ok and !lv.layers[app.layer].locked;
  const bool pressed = hovered and ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  Tool tool = app.tool;
  if (ImGui::IsKeyDown(ImGuiKey_Space) or ImGui::IsMouseDown(ImGuiMouseButton_Middle))
  {
    tool = Tool::kPan;
  }

  switch (tool)
  {
  case Tool::kPan:
    if (
      (hovered or stroke.active) and
      (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F) or ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0F)))
    {
      app.view.x -= io.MouseDelta.x / cv.scale;
      app.view.y -= io.MouseDelta.y / cv.scale;
    }
    break;
  case Tool::kPaint:
  case Tool::kAutoTile:
  case Tool::kErase: {
    const bool erase = (tool == Tool::kErase);
    const bool on_tiles = layer_ok and lv.layers[app.layer].kind == LayerKind::kTiles;
    // Auto-tiling needs the cells the stroke wrote, to re-fill the band around them
    const bool refill = (tool == Tool::kAutoTile) and on_tiles;
    // Which rules the Auto tool paints with: the terrain the tile belongs to, or else the WFC
    // fallback. The toolbar says which, and the status line reports what the stroke did
    const TerrainId terrain = refill ? auto_tile_terrain(app) : kNoTerrain;
    // The tiles a stamp writes (the palette selection's layout, or the paint stamp), built once.
    // A terrain lays down its own tiles, so there the stroke only marks the cells it covers --
    // stamping the selection (the terrain's own tiles, as often as not) would fight it
    memory::Scope paint_scope{app.memory.frame};
    Grid<TileRef> stamp{app.memory.frame};
    const Grid<TileRef>* tiles = nullptr;
    if (!erase and on_tiles and terrain == kNoTerrain and (pressed or stroke.active))
    {
      tiles = paint_stamp(app, stamp) ? &stamp : nullptr;
    }
    auto painted = memory::make_vector<Cell>(app.memory.frame);
    memory::Vector<Cell>* touched = refill ? &painted : nullptr;
    if (pressed and editable)
    {
      static constexpr const char* kLabels[] = {"Paint", "Auto-tile", "Erase"};
      app.undo.snapshot(lv, app.layer, kLabels[erase ? 2 : (refill ? 1 : 0)]);
      stroke.active = true;
      stroke.last = cell;
      apply_line(app, cell, cell, erase, tiles, touched);
      invalidate_layer(app, app.layer);
      app.modified = true;
    }
    else if (stroke.active and down and cell != stroke.last)
    {
      apply_line(app, stroke.last, cell, erase, tiles, touched);
      stroke.last = cell;
      invalidate_layer(app, app.layer);
    }
    else if (stroke.active and down and is_scalar(lv.layers[app.layer].kind))
    {
      // Scalar brushes keep applying while held still
      apply_brush(app, cell, erase);
      invalidate_layer(app, app.layer);
    }
    if (!painted.empty())
    {
      auto_tile_stroke(app, app.layer, painted.data(), painted.size());
    }
    break;
  }
  case Tool::kFill:
    if (pressed and editable)
    {
      app.undo.snapshot(lv, app.layer, "Fill");
      flood_fill(app, cell);
      invalidate_layer(app, app.layer);
      app.modified = true;
    }
    break;
  case Tool::kRect:
  case Tool::kSelect: {
    if (tool == Tool::kSelect and pressed)
    {
      const Entity hit = pick(lv.registry, Position{mouse_cells.x, mouse_cells.y}, 8.0F / cv.scale);
      app.entity = hit;
      if (hit != kNullEntity and lv.registry.all_of<Position>(hit))
      {
        const auto& p = lv.registry.get<Position>(hit);
        stroke.moving = true;
        stroke.grab_offset = ImVec2{p.x - mouse_cells.x, p.y - mouse_cells.y};
        break;
      }
    }
    if (stroke.moving)
    {
      if (down and lv.registry.valid(app.entity))
      {
        auto& p = lv.registry.get<Position>(app.entity);
        // Snap to half cells unless Shift
        const float snap = io.KeyShift ? 0.0F : 0.5F;
        float nx = mouse_cells.x + stroke.grab_offset.x;
        float ny = mouse_cells.y + stroke.grab_offset.y;
        if (snap > 0.0F)
        {
          nx = std::round(nx / snap) * snap;
          ny = std::round(ny / snap) * snap;
        }
        if (nx != p.x or ny != p.y)
        {
          p = Position{nx, ny};
          app.modified = true;
        }
      }
      if (released)
      {
        stroke.moving = false;
      }
      break;
    }
    if (pressed and (tool == Tool::kSelect or editable))
    {
      stroke.dragging_rect = true;
      stroke.anchor = cell;
    }
    if (stroke.dragging_rect)
    {
      const ImVec2 a = cv.to_screen(
        app.view,
        static_cast<float>(std::min(stroke.anchor.x, cell.x)),
        static_cast<float>(std::min(stroke.anchor.y, cell.y)));
      const ImVec2 b = cv.to_screen(
        app.view,
        static_cast<float>(std::max(stroke.anchor.x, cell.x) + 1),
        static_cast<float>(std::max(stroke.anchor.y, cell.y) + 1));
      cv.draw->AddRect(
        a, b, tool == Tool::kSelect ? IM_COL32(255, 230, 60, 255) : IM_COL32(255, 255, 255, 255), 0, 0, 2.0F);
      if (released)
      {
        stroke.dragging_rect = false;
        if (tool == Tool::kRect)
        {
          app.undo.snapshot(lv, app.layer, "Rectangle");
          fill_rect(app, stroke.anchor, cell, io.KeyAlt);
          invalidate_layer(app, app.layer);
          app.modified = true;
        }
        else
        {
          // Region for WFC generation
          const Extents e = lv.settings.extents;
          const int rx0 = std::clamp(std::min(stroke.anchor.x, cell.x), 0, e.width);
          const int ry0 = std::clamp(std::min(stroke.anchor.y, cell.y), 0, e.height);
          const int rx1 = std::clamp(std::max(stroke.anchor.x, cell.x) + 1, 0, e.width);
          const int ry1 = std::clamp(std::max(stroke.anchor.y, cell.y) + 1, 0, e.height);
          app.generators.region_x = rx0;
          app.generators.region_y = ry0;
          app.generators.region_w = (rx1 - rx0 > 1 or ry1 - ry0 > 1) ? rx1 - rx0 : 0;
          app.generators.region_h = (rx1 - rx0 > 1 or ry1 - ry0 > 1) ? ry1 - ry0 : 0;
        }
      }
    }
    break;
  }
  case Tool::kPick:
    if (pressed and layer_ok)
    {
      eyedrop(app, cell);
    }
    break;
  case Tool::kPortal:
    if (pressed and lv.settings.extents.contains(cell))
    {
      const auto count = lv.registry.view<Portal>().size();
      app.entity = create_portal(
        lv.registry,
        format<64>("portal_%zu", count + 1),
        Position{static_cast<float>(cell.x), static_cast<float>(cell.y)},
        Extent{1, 1},
        Portal{});
      app.modified = true;
      app.panels.objects = true;
    }
    break;
  case Tool::kMarker:
    if (pressed and lv.settings.extents.contains(cell))
    {
      const auto count = lv.registry.view<Marker>().size();
      app.entity = create_marker(
        lv.registry,
        format<64>("marker_%zu", count + 1),
        "spawn",
        Position{static_cast<float>(cell.x) + 0.5F, static_cast<float>(cell.y) + 0.5F});
      app.modified = true;
      app.panels.objects = true;
    }
    break;
  case Tool::kPathNode:
    if (pressed)
    {
      if (const Entity hit = node_at(app, cv, mouse_cells); hit != kNullEntity)
      {
        app.entity = hit;
        app.graph = lv.registry.get<PathNode>(hit).graph;
        break;
      }
      if (!lv.settings.extents.contains(cell))
      {
        break;
      }
      if (lv.graphs.empty())
      {
        app.graph = add_graph(lv, "route", GraphKind::kRoad);
      }
      app.graph = std::min<GraphId>(app.graph, static_cast<GraphId>(lv.graphs.size() - 1));
      const float snap = io.KeyShift ? 0.0F : 0.5F;
      const Position p{
        snap > 0.0F ? std::round(mouse_cells.x / snap) * snap : mouse_cells.x,
        snap > 0.0F ? std::round(mouse_cells.y / snap) * snap : mouse_cells.y};
      const Entity node = create_path_node(lv.registry, app.graph, p);
      // Chain from the selected node of the same graph (hold Alt to start a new chain)
      if (
        !io.KeyAlt and lv.registry.valid(app.entity) and lv.registry.all_of<PathNode>(app.entity) and
        lv.registry.get<PathNode>(app.entity).graph == app.graph)
      {
        create_path_edge(lv.registry, app.entity, node);
      }
      app.entity = node;
      app.modified = true;
    }
    if (hovered and ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
      if (const Entity hit = node_at(app, cv, mouse_cells); hit != kNullEntity)
      {
        destroy_path_node(lv.registry, hit);
        app.entity = (app.entity == hit) ? kNullEntity : app.entity;
        app.route_from = (app.route_from == hit) ? kNullEntity : app.route_from;
        app.modified = true;
      }
    }
    break;
  case Tool::kPathEdge:
    if (pressed)
    {
      const Entity hit = node_at(app, cv, mouse_cells);
      if (hit == kNullEntity)
      {
        app.edge_start = kNullEntity;
      }
      else if (app.edge_start == kNullEntity or !lv.registry.valid(app.edge_start))
      {
        app.edge_start = hit;
      }
      else if (
        hit != app.edge_start and
        lv.registry.get<PathNode>(hit).graph == lv.registry.get<PathNode>(app.edge_start).graph)
      {
        // Toggle the edge
        Entity existing = kNullEntity;
        for (auto [e, edge] : lv.registry.view<PathEdge>().each())
        {
          if ((edge.from == app.edge_start and edge.to == hit) or (edge.from == hit and edge.to == app.edge_start))
          {
            existing = e;
          }
        }
        if (existing != kNullEntity)
        {
          lv.registry.destroy(existing);
        }
        else
        {
          create_path_edge(lv.registry, app.edge_start, hit, 1.0F, !io.KeyShift);
        }
        app.edge_start = io.KeyCtrl ? hit : kNullEntity;
        app.modified = true;
      }
    }
    break;
  }
  if (released)
  {
    stroke.active = false;
  }

  // Hover feedback: brush footprint
  if (
    hovered and lv.settings.extents.contains(cell) and
    (tool == Tool::kPaint or tool == Tool::kAutoTile or tool == Tool::kErase))
  {
    const bool stamping =
      layer_ok and lv.layers[app.layer].kind == LayerKind::kTiles and (tool == Tool::kPaint or tool == Tool::kAutoTile);
    const int r = stamping ? 0 : app.brush.radius;
    const Extents footprint = stamping ? stamp_extents(app) : Extents{1, 1};
    const ImU32 color = editable ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 80, 80, 200);
    const ImVec2 a = cv.to_screen(app.view, static_cast<float>(cell.x - r), static_cast<float>(cell.y - r));
    const ImVec2 b = cv.to_screen(
      app.view, static_cast<float>(cell.x + r + footprint.width), static_cast<float>(cell.y + r + footprint.height));
    cv.draw->AddRect(a, b, color, 0, 0, 1.5F);
    // A multi-tile stamp can have holes (a tile group's, or the palette selection's), which keep
    // what is under them: outline the cells it actually writes
    memory::Scope hover_scope{app.memory.frame};
    Grid<TileRef> hover{app.memory.frame};
    if (stamping and footprint != Extents{1, 1} and paint_stamp(app, hover))
    {
      for (int y = 0; y < hover.extents.height; ++y)
      {
        for (int x = 0; x < hover.extents.width; ++x)
        {
          if (!hover.at(x, y).empty())
          {
            cv.draw->AddRect(
              cv.to_screen(app.view, static_cast<float>(cell.x + x), static_cast<float>(cell.y + y)),
              cv.to_screen(app.view, static_cast<float>(cell.x + x + 1), static_cast<float>(cell.y + y + 1)),
              color,
              0,
              0,
              1.0F);
          }
        }
      }
    }
  }
}

void toolbar(App& app)
{
  struct Button
  {
    Tool tool;
    const char* label;
    const char* tip;
  };
  static constexpr Button kButtons[] = {
    {Tool::kPan, "Pan", "Pan (H; or middle mouse / Space+drag)"},
    {Tool::kPaint, "Paint", "Paint (B)"},
    {Tool::kAutoTile,
     "Auto",
     "Auto-tile (T): paints the tile as a shape's center; the tile adjacency rules fill in the edges "
     "and corners around it (water gets its shore, a path its sides), and painting more centers "
     "re-fills around the larger shape"},
    {Tool::kErase, "Erase", "Erase (E)"},
    {Tool::kFill, "Fill", "Flood fill (G)"},
    {Tool::kRect, "Rect", "Rectangle fill (R; Alt to erase)"},
    {Tool::kPick, "Pick", "Eyedropper (I)"},
    {Tool::kSelect, "Select", "Select/move objects; drag a region for WFC (V)"},
    {Tool::kPortal, "Portal", "Place portal (P)"},
    {Tool::kMarker, "Marker", "Place marker (M)"},
    {Tool::kPathNode,
     "Node",
     "Path nodes (N): click to add (chains from selected; Alt starts a new chain), right-click deletes"},
    {Tool::kPathEdge,
     "Edge",
     "Path edges (K): click two nodes to toggle an edge (Shift: one-way, Ctrl: keep chaining)"},
  };
  for (const auto& b : kButtons)
  {
    const bool active = (app.tool == b.tool);
    if (active)
    {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::SmallButton(b.label))
    {
      app.tool = b.tool;
    }
    if (active)
    {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("%s", b.tip);
    }
    ImGui::SameLine();
  }
  ImGui::SetNextItemWidth(80.0F);
  ImGui::SliderInt("Radius", &app.brush.radius, 0, 32);
  const Level& lv = *app.level;
  // What a stamp writes is not visible on the canvas until it is painted, so the toolbar says when
  // it is a named group rather than the tile in the palette
  if (const TileGroup* group = chosen_group(app); group != nullptr and app.tool == Tool::kPaint)
  {
    ImGui::SameLine();
    ImGui::Text("group '%s' (%d x %d)", group->name.c_str(), group->size.width, group->size.height);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Painting stamps this tile group as one thing. Choose another, or the same one again to stop, "
                        "in Tilesets > Tile groups; Ctrl+Shift+A also stops.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop##group"))
    {
      app.group.clear();
    }
  }
  if (app.tool == Tool::kAutoTile)
  {
    // Which of the two kinds of rules this tile paints with, so it is plain which tab to edit
    const TerrainId terrain = terrain_of(rule_set(app).auto_tiles, app.paint.tile);
    ImGui::SameLine();
    if (terrain != kNoTerrain)
    {
      ImGui::Text("terrain '%s'", rule_set(app).auto_tiles.terrains[terrain].name.c_str());
      if (ImGui::IsItemHovered())
      {
        ImGui::SetTooltip("This tile belongs to an auto-tiling terrain, so painting lays down its middle and tiles "
                          "the border around it -- edges and inner/outer corners. Edit it in View > Tile adjacency "
                          "rules > Auto-tiling.");
      }
    }
    else
    {
      ImGui::SetNextItemWidth(80.0F);
      ImGui::SliderInt("Refill", &app.auto_tile.radius, 1, 8);
      if (ImGui::IsItemHovered())
      {
        ImGui::SetTooltip("How many cells around what is painted the WFC adjacency rules may re-fill");
      }
      // Auto-tiling has nothing to go on unless the tile being painted is in the rules
      const bool known = find_tile(rule_set(app).wfc, app.paint.tile) < rule_set(app).wfc.tiles.size();
      ImGui::SameLine();
      if (!known)
      {
        ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "no rules for this tile");
        if (ImGui::IsItemHovered())
        {
          ImGui::SetTooltip("The tile being painted is in neither kind of tile adjacency rules, so nothing can be "
                            "filled in around it. Add it in View > Tile adjacency rules.");
        }
      }
      else
      {
        ImGui::TextDisabled("WFC rules");
        if (ImGui::IsItemHovered())
        {
          ImGui::SetTooltip("This tile has no auto-tiling terrain, so the WFC adjacency rules are scored against its "
                            "neighbors instead. Those rules cannot prescribe inner and outer corners: give the tile a "
                            "terrain under View > Tile adjacency rules > Auto-tiling for that.");
        }
      }
    }
  }
  if (app.layer < lv.layers.size() and is_scalar(lv.layers[app.layer].kind))
  {
    static constexpr const char* kModes[] = {"Set", "Raise", "Lower", "Smooth", "Flatten"};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0F);
    int mode = static_cast<int>(app.brush.scalar_mode);
    if (ImGui::Combo("##mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
    {
      app.brush.scalar_mode = static_cast<ScalarMode>(mode);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0F);
    ImGui::DragFloat("Strength", &app.brush.strength, 0.001F, 0.0F, 1.0F, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0F);
    ImGui::DragFloat("Value", &app.brush.value, 0.005F);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0F);
  float zoom = app.view.cell_pixels;
  if (ImGui::SliderFloat("Zoom", &zoom, 1.0F, 128.0F, "%.0f px", ImGuiSliderFlags_Logarithmic))
  {
    app.view.cell_pixels = zoom;
  }
}

}  // namespace

TerrainId auto_tile_terrain(const App& app) { return terrain_of(rule_set(app).auto_tiles, app.paint.tile); }

void auto_tile_stroke(App& app, std::size_t layer, const Cell* painted, std::size_t count)
{
  Level& lv = *app.level;
  LVL_ASSERT_LT(layer, lv.layers.size());
  auto& grid = plane<TileGrid>(lv.layers[layer]);
  const TerrainId terrain = auto_tile_terrain(app);
  AutoTileStatus status;
  if (terrain != kNoTerrain)
  {
    // The terrain prescribes the tile of every cell it covers, edges and inner/outer corners
    const auto result =
      procgen::paint_terrain(rule_set(app).auto_tiles, terrain, painted, count, grid, app.memory.frame);
    status.terrain = true;
    status.name = rule_set(app).auto_tiles.terrains[terrain].name.view();
    status.cells = result.covered;
    status.changed = result.changed;
    status.trouble = result.missing;
  }
  else
  {
    // No terrain: the WFC rules can still tidy the band against hard constraints
    const auto result = procgen::autotile(
      rule_set(app).wfc,
      painted,
      count,
      procgen::AutoTileParams{.radius = app.auto_tile.radius, .iterations = app.auto_tile.iterations},
      grid,
      app.memory.frame);
    status.terrain = false;
    status.name = "WFC rules";
    status.cells = result.considered;
    status.changed = result.changed;
    status.trouble = result.unresolved;
  }
  app.auto_tile_status = status;
  invalidate_layer(app, layer);
}

void viewport_window(App& app)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{4, 4});
  const bool visible =
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!visible)
  {
    ImGui::End();
    return;
  }
  toolbar(app);
  update_overlays(app);

  Canvas cv;
  cv.origin = ImGui::GetCursorScreenPos();
  cv.size = ImGui::GetContentRegionAvail();
  cv.size.y -= ImGui::GetTextLineHeightWithSpacing();
  cv.size.x = std::max(cv.size.x, 16.0F);
  cv.size.y = std::max(cv.size.y, 16.0F);
  const Extents e = app.level->settings.extents;
  View& v = app.view;
  if (v.fit)
  {
    v.cell_pixels = std::clamp(
      std::min(cv.size.x / static_cast<float>(e.width), cv.size.y / static_cast<float>(e.height)) * 0.95F,
      1.0F,
      128.0F);
    v.x = (static_cast<float>(e.width) - cv.size.x / v.cell_pixels) * 0.5F;
    v.y = (static_cast<float>(e.height) - cv.size.y / v.cell_pixels) * 0.5F;
    // The dock layout settles over the first frames: keep fitting until then
    v.fit = (app.frame_count < 3);
  }
  cv.scale = v.cell_pixels;
  ImGui::InvisibleButton(
    "canvas",
    cv.size,
    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();

  // Zoom around the cursor
  if (hovered and io.MouseWheel != 0.0F)
  {
    const ImVec2 before = cv.to_cells(v, io.MousePos);
    v.cell_pixels = std::clamp(v.cell_pixels * std::pow(1.15F, io.MouseWheel), 1.0F, 128.0F);
    cv.scale = v.cell_pixels;
    const ImVec2 after = cv.to_cells(v, io.MousePos);
    v.x += before.x - after.x;
    v.y += before.y - after.y;
  }
  // Keyboard scrolling
  if ((hovered or ImGui::IsWindowFocused()) and !io.WantTextInput and !io.KeyCtrl)
  {
    const float speed = 600.0F * io.DeltaTime / cv.scale;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) or ImGui::IsKeyDown(ImGuiKey_A))
    {
      v.x -= speed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow) or ImGui::IsKeyDown(ImGuiKey_D))
    {
      v.x += speed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow) or ImGui::IsKeyDown(ImGuiKey_W))
    {
      v.y -= speed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow) or ImGui::IsKeyDown(ImGuiKey_S))
    {
      v.y += speed;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F))
    {
      v.fit = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) and !busy(app) and app.level->registry.valid(app.entity))
    {
      if (app.level->registry.all_of<PathNode>(app.entity))
      {
        destroy_path_node(app.level->registry, app.entity);
      }
      else
      {
        app.level->registry.destroy(app.entity);
      }
      app.entity = kNullEntity;
      app.modified = true;
    }
  }

  cv.draw = ImGui::GetWindowDrawList();
  cv.draw->PushClipRect(cv.origin, ImVec2{cv.origin.x + cv.size.x, cv.origin.y + cv.size.y}, true);
  draw_layers(app, cv);
  if (v.objects)
  {
    draw_objects(app, cv);
  }
  const Generators& g = app.generators;
  if (g.region_w > 0 and g.region_h > 0)
  {
    cv.draw->AddRect(
      cv.to_screen(v, static_cast<float>(g.region_x), static_cast<float>(g.region_y)),
      cv.to_screen(v, static_cast<float>(g.region_x + g.region_w), static_cast<float>(g.region_y + g.region_h)),
      IM_COL32(255, 230, 60, 200),
      0,
      0,
      2.0F);
  }
  if (std::holds_alternative<std::monostate>(app.job))
  {
    handle_tools(app, cv, hovered);
  }
  cv.draw->PopClipRect();

  // Status line
  const ImVec2 mouse = cv.to_cells(v, io.MousePos);
  const Cell cell{static_cast<int>(std::floor(mouse.x)), static_cast<int>(std::floor(mouse.y))};
  if (app.layer < app.level->layers.size())
  {
    const Layer& layer = app.level->layers[app.layer];
    // Cursor cell only while the mouse is over the canvas (elsewhere its position may be undefined)
    if (hovered)
    {
      ImGui::Text("(%d, %d)  %s  |", cell.x, cell.y, describe(app, cell));
      ImGui::SameLine();
    }
    ImGui::Text("layer '%s' (%s)%s", layer.name.c_str(), enum_name(layer.kind).data(), layer.locked ? " [locked]" : "");
    const AutoTileStatus& status = app.auto_tile_status;
    if (app.tool == Tool::kAutoTile and (status.cells > 0 or status.trouble > 0))
    {
      ImGui::SameLine();
      if (status.trouble > 0)
      {
        ImGui::TextDisabled(
          "| %s: %zu of %zu cells tiled, %zu the rules cannot fill",
          status.name.c_str(),
          status.changed,
          status.cells,
          status.trouble);
      }
      else
      {
        ImGui::TextDisabled("| %s: %zu of %zu cells tiled", status.name.c_str(), status.changed, status.cells);
      }
    }
  }
  ImGui::End();
}

}  // namespace lvl::editor
