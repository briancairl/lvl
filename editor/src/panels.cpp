/**
 * @copyright 2026-present Brian Cairl
 *
 * @file panels.cpp
 *
 * Layers, tilesets (slicer + palette), tile metadata, catalogs & seasons, objects & paths,
 * mini-preview and log panels.
 */

// C++ Standard Library
#include <algorithm>
#include <cmath>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/render/compose.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

/// Collision mask editor: four quadrant toggles plus flags
bool collision_edit(std::uint8_t& mask)
{
  bool changed = false;
  ImGui::TextUnformatted("Collision");
  const auto quadrant = [&](const char* id, std::uint8_t bit) {
    const bool on = (mask & bit) != 0;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4{0.85F, 0.25F, 0.25F, 1.0F} : ImVec4{0.25F, 0.25F, 0.28F, 1.0F});
    if (ImGui::Button(id, ImVec2{22, 22}))
    {
      mask ^= bit;
      changed = true;
    }
    ImGui::PopStyleColor();
  };
  ImGui::BeginGroup();
  quadrant("##nw", collision::kNorthWest);
  ImGui::SameLine(0, 2);
  quadrant("##ne", collision::kNorthEast);
  quadrant("##sw", collision::kSouthWest);
  ImGui::SameLine(0, 2);
  quadrant("##se", collision::kSouthEast);
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  const auto flag = [&](const char* label, std::uint8_t bit) {
    bool on = (mask & bit) != 0;
    if (ImGui::Checkbox(label, &on))
    {
      mask = static_cast<std::uint8_t>(on ? (mask | bit) : (mask & ~bit));
      changed = true;
    }
  };
  flag("Platform", collision::kPlatform);
  flag("Trigger", collision::kTrigger);
  flag("Blocks sight", collision::kSightBlocker);
  if (ImGui::SmallButton("Solid"))
  {
    mask |= collision::kSolid;
    changed = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("None"))
  {
    mask = 0;
    changed = true;
  }
  ImGui::EndGroup();
  return changed;
}

}  // namespace

bool tile_uv(const App& app, TileRef tile, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1)
{
  const project::Project& pr = *app.project;
  if (tile.empty() or tile.tileset >= pr.tilesets.size() or !app.assets.textures[tile.tileset].valid())
  {
    return false;
  }
  const Texture& tex = app.assets.textures[tile.tileset];
  const TileRect r = tile_rect(pr.tilesets[tile.tileset].slice, tex.width, tex.height, tile.tile);
  texture = texture_id(tex);
  uv0 = ImVec2{static_cast<float>(r.x) / tex.width, static_cast<float>(r.y) / tex.height};
  uv1 = ImVec2{static_cast<float>(r.x + r.width) / tex.width, static_cast<float>(r.y + r.height) / tex.height};
  return true;
}

void tile_image(const App& app, TileRef tile, float size)
{
  ImTextureID texture{};
  ImVec2 uv0;
  ImVec2 uv1;
  if (!tile_uv(app, tile, texture, uv0, uv1))
  {
    ImGui::Dummy(ImVec2{size, size});
    return;
  }
  ImGui::Image(texture, ImVec2{size, size}, uv0, uv1);
}

// -------------------------------------------------------------------------------------------------
// Layers
// -------------------------------------------------------------------------------------------------

void layers_panel(App& app)
{
  if (!ImGui::Begin("Layers", &app.panels.layers))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  Level& lv = *app.level;
  const project::Project& pr = *app.project;
  if (ImGui::Button("Add"))
  {
    ImGui::OpenPopup("add_layer");
  }
  if (ImGui::BeginPopup("add_layer"))
  {
    for (std::size_t k = 0; k < kLayerKindCount; ++k)
    {
      const auto kind = static_cast<LayerKind>(k);
      if (ImGui::Selectable(enum_name(kind).data()) and lv.layers.size() < kMaxLayers)
      {
        app.layer = add_layer(lv, kind, enum_name(kind));
        app.undo.clear();
        invalidate_view(app);
        app.modified = true;
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(app.layer >= lv.layers.size());
  if (ImGui::Button("Remove"))
  {
    remove_layer(lv, app.layer);
    app.layer = std::min(app.layer, lv.layers.empty() ? 0 : lv.layers.size() - 1);
    app.undo.clear();
    invalidate_view(app);
    app.modified = true;
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##up", ImGuiDir_Up) and app.layer + 1 < lv.layers.size())
  {
    move_layer(lv, app.layer, app.layer + 1);
    ++app.layer;
    app.undo.clear();
    invalidate_view(app);
    app.modified = true;
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##down", ImGuiDir_Down) and app.layer > 0 and app.layer < lv.layers.size())
  {
    move_layer(lv, app.layer, app.layer - 1);
    --app.layer;
    app.undo.clear();
    invalidate_view(app);
    app.modified = true;
  }
  ImGui::EndDisabled();
  ImGui::Separator();

  // Top layer first
  for (std::size_t n = lv.layers.size(); n-- > 0;)
  {
    Layer& layer = lv.layers[n];
    ImGui::PushID(static_cast<int>(n));
    if (delete_button("Delete layer"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kLayer,
          .index = n,
          .what = format<128>("layer '%s'", layer.name.c_str()),
          .detail = "Its cells are lost, and the undo history is cleared."});
    }
    if (ImGui::Checkbox("##visible", &layer.visible))
    {
      app.minimap_dirty = true;
    }
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Visible");
    }
    ImGui::SameLine();
    ImGui::Checkbox("##locked", &layer.locked);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Locked");
    }
    ImGui::SameLine();
    if (ImGui::Selectable(format<128>("%s  [%s]", layer.name.c_str(), enum_name(layer.kind).data()), app.layer == n))
    {
      app.layer = n;
    }
    ImGui::PopID();
  }

  if (app.layer < lv.layers.size())
  {
    Layer& layer = lv.layers[app.layer];
    ImGui::SeparatorText("Layer");
    input_text("Name", layer.name);
    if (ImGui::SliderFloat("Opacity", &layer.opacity, 0.0F, 1.0F))
    {
      app.minimap_dirty = true;
    }
    if (layer.kind != LayerKind::kTiles)
    {
      if (is_scalar(layer.kind))
      {
        if (enum_combo("Preview", layer.preview))
        {
          invalidate_layer(app, app.layer);
        }
        if (ImGui::DragFloatRange2("Range", &layer.range_min, &layer.range_max, 0.01F))
        {
          invalidate_layer(app, app.layer);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit"))
        {
          fit_range(layer);
          invalidate_layer(app, app.layer);
        }
        if (
          layer.preview == Preview::kContours and
          ImGui::DragFloat("Contour step", &layer.contour_interval, 0.005F, 0.01F, 1.0F))
        {
          invalidate_layer(app, app.layer);
        }
      }
      // What painting writes on this layer
      ImGui::SeparatorText("Paint value");
      switch (layer.kind)
      {
      case LayerKind::kSemantic:
        catalog_combo("Semantic", pr.semantics, app.paint.semantic);
        break;
      case LayerKind::kMaterial:
        catalog_combo("Material", pr.materials, app.paint.material);
        break;
      case LayerKind::kCollision:
        collision_edit(app.paint.collision);
        break;
      default:
        ImGui::TextDisabled("Brush mode/strength/value in the viewport toolbar");
        break;
      }
    }
    else
    {
      ImGui::TextDisabled("Paint with the tile selected in Tilesets");
      tile_image(app, app.paint.tile, 32.0F);
    }
  }
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Tilesets: list, slicer, palette
// -------------------------------------------------------------------------------------------------

// -------------------------------------------------------------------------------------------------
// Tile groups
// -------------------------------------------------------------------------------------------------

namespace
{

/**
 * @brief The tileset's named multi-tile stamps: the list, what is chosen, and how one is made
 *
 * A group is made out of the palette selection, because that is already how several tiles are
 * picked and already how they are laid out -- the house in the atlas is the house on the map.
 */
void groups_section(App& app, TileSet& ts)
{
  ImGui::TextDisabled("Stamped as one thing. Pick the tiles in the palette, name it, then add it.");
  if (app.group.active() and app.group.tileset != app.tileset)
  {
    const project::Project& pr = *app.project;
    ImGui::TextColored(
      ImVec4{0.85F, 0.8F, 0.45F, 1.0F},
      "A group of '%s' is being stamped.",
      app.group.tileset < pr.tilesets.size() ? pr.tilesets[app.group.tileset].name.c_str() : "?");
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop"))
    {
      app.group.clear();
    }
  }
  if (ts.groups.empty())
  {
    ImGui::TextDisabled("This tileset has no groups.");
  }
  for (std::size_t i = 0; i < ts.groups.size(); ++i)
  {
    const TileGroup& group = ts.groups[i];
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Remove this tile group"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kTileGroup,
          .index = i,
          .owner = app.tileset,
          .what = format<128>("tile group '%s'", group.name.c_str()),
          .detail = "Only the group goes; the tiles it stamped onto the map stay where they are."});
    }
    const bool chosen = app.group.tileset == app.tileset and app.group.group == i;
    if (ImGui::Selectable(format<96>("%s  (%d x %d)", group.name.c_str(), group.size.width, group.size.height), chosen))
    {
      choose_group(app, chosen ? kNoGroup : static_cast<GroupId>(i));
    }
    ImGui::PopID();
  }

  // The chosen group, drawn as it will be stamped (a hole shows through as a gap)
  if (const TileGroup* group = chosen_group(app); group != nullptr and app.group.tileset == app.tileset)
  {
    ImGui::SeparatorText(group->name.c_str());
    if (input_text("Name##group", ts.groups[app.group.group].name))
    {
      app.project_modified = true;
    }
    const float thumb = 24.0F;
    for (int y = 0; y < group->size.height; ++y)
    {
      for (int x = 0; x < group->size.width; ++x)
      {
        if (x > 0)
        {
          ImGui::SameLine();
        }
        if (const TileIndex tile = group_cell(ts, *group, x, y); tile == kNoTileIndex)
        {
          ImGui::Dummy(ImVec2{thumb, thumb});
        }
        else
        {
          tile_image(app, TileRef{static_cast<TileSetId>(app.tileset), tile}, thumb);
        }
      }
    }
  }

  ImGui::SeparatorText("New group");
  const std::size_t selected = selection_size(app);
  ImGui::SetNextItemWidth(160.0F);
  input_text("Name##new_group", app.group_name);
  ImGui::SameLine();
  const bool ready = selected > 0 and !app.group_name.empty();
  ImGui::BeginDisabled(!ready);
  const bool add = ImGui::Button("From the palette selection");
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
  {
    ImGui::SetTooltip(
      selected == 0 ? "Select the group's tiles in the palette first"
                    : (app.group_name.empty() ? "Name the group first"
                                              : "Adds the selection as a group, or replaces the one of that name"));
  }
  if (add)
  {
    memory::Scope scope{app.memory.frame};
    Grid<TileRef> layout{app.memory.frame};
    if (!palette_layout(app, layout))
    {
      report(app, Status::error("the palette selection is empty"));
    }
    else
    {
      // Only this tileset's tiles can be in its own group; the selection is of one tileset anyway
      auto cells = memory::make_vector<TileIndex>(app.memory.frame, layout.extents.count(), kNoTileIndex);
      for (std::size_t i = 0; i < layout.extents.count(); ++i)
      {
        const TileRef tile = layout.values[i];
        cells[i] = (tile.empty() or tile.tileset != app.tileset) ? kNoTileIndex : tile.tile;
      }
      const GroupId id = set_group(ts, app.group_name.view(), layout.extents, cells.data());
      if (id == kNoGroup)
      {
        report(app, Status::error("could not add tile group '%s'", app.group_name.c_str()));
      }
      else
      {
        LVL_LOG_INFO() << "Tile group '" << app.group_name.c_str() << "' holds " << layout.extents.width << "x"
                       << layout.extents.height << " cells";
        app.project_modified = true;
        choose_group(app, id);
      }
    }
  }
}

}  // namespace

void tilesets_panel(App& app)
{
  if (!ImGui::Begin("Tilesets", &app.panels.tilesets))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  project::Project& pr = *app.project;
  Level& lv = *app.level;
  if (ImGui::Button("New tileset") and pr.tilesets.size() < kMaxTileSets)
  {
    app.tileset = project::add_tileset(pr, format<64>("tileset_%zu", pr.tilesets.size()));
    app.modified = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Import..."))
  {
    open_file_dialog(app, FileAction::kLoadTileSet);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(app.tileset >= pr.tilesets.size());
  if (ImGui::Button("Export..."))
  {
    open_file_dialog(app, FileAction::kSaveTileSet);
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove"))
  {
    // Refused while the project has several levels: only the open one can be renumbered
    if (const Status s = project::remove_tileset(pr, lv, static_cast<TileSetId>(app.tileset)); !s.ok)
    {
      report(app, s);
    }
    else
    {
      app.tileset = 0;
      app.selection.clear();
      // The tilesets after it were renumbered, so the index the chosen group holds means nothing now
      app.group = GroupSelection{};
      app.undo.clear();
      app.modified = true;
      reload_atlases(app);
    }
  }
  ImGui::EndDisabled();
  if (pr.tilesets.empty())
  {
    ImGui::TextDisabled("No tilesets. Create one and choose an atlas image, or File > New demo level.");
    ImGui::End();
    return;
  }
  app.tileset = std::min(app.tileset, pr.tilesets.size() - 1);
  if (ImGui::BeginCombo("Tileset", pr.tilesets[app.tileset].name.c_str()))
  {
    for (std::size_t i = 0; i < pr.tilesets.size(); ++i)
    {
      if (ImGui::Selectable(format<96>("%s##%zu", pr.tilesets[i].name.c_str(), i), i == app.tileset))
      {
        app.tileset = i;
      }
    }
    ImGui::EndCombo();
  }
  TileSet& ts = pr.tilesets[app.tileset];
  const Texture& tex = app.assets.textures[app.tileset];

  if (ImGui::CollapsingHeader("Slicer", ImGuiTreeNodeFlags_DefaultOpen))
  {
    input_text("Name", ts.name);
    input_text("Atlas", ts.image);
    ImGui::SameLine();
    if (ImGui::SmallButton("..."))
    {
      open_file_dialog(app, FileAction::kAtlasImage);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload"))
    {
      reload_atlases(app);
    }
    if (!app.assets.status[app.tileset].ok)
    {
      ImGui::TextColored(ImVec4{1, 0.5F, 0.4F, 1}, "%s", app.assets.status[app.tileset].message.c_str());
    }
    bool resliced = false;
    resliced |= ImGui::InputInt2("Tile size", &ts.slice.tile_width);
    resliced |= ImGui::InputInt2("Margin", &ts.slice.margin_x);
    resliced |= ImGui::InputInt2("Spacing", &ts.slice.spacing_x);
    resliced |= ImGui::InputInt2("Columns/rows (0 = fit)", &ts.slice.columns);
    ts.slice.tile_width = std::max(1, ts.slice.tile_width);
    ts.slice.tile_height = std::max(1, ts.slice.tile_height);
    ts.slice.margin_x = std::max(0, ts.slice.margin_x);
    ts.slice.margin_y = std::max(0, ts.slice.margin_y);
    ts.slice.spacing_x = std::max(0, ts.slice.spacing_x);
    ts.slice.spacing_y = std::max(0, ts.slice.spacing_y);
    ts.slice.columns = std::max(0, ts.slice.columns);
    ts.slice.rows = std::max(0, ts.slice.rows);
    if (resliced)
    {
      reslice(ts);
      app.selection.clear();
      if (app.tileset < kMaxTileSets and !app.assets.images[app.tileset].empty())
      {
        render::average_tile_colors(ts, app.assets.images[app.tileset], app.assets.tile_colors[app.tileset]);
      }
      invalidate_view(app);
      app.modified = true;
    }
    const Extents grid = slice_grid(ts.slice, ts.image_width, ts.image_height);
    ImGui::TextDisabled(
      "%dx%d px atlas -> %d x %d = %zu tiles",
      ts.image_width,
      ts.image_height,
      grid.width,
      grid.height,
      ts.tiles.size());
  }

  if (ImGui::CollapsingHeader("Tile groups"))
  {
    groups_section(app, ts);
  }

  if (ImGui::CollapsingHeader("Palette", ImGuiTreeNodeFlags_DefaultOpen))
  {
    static float zoom = 2.0F;
    ImGui::SetNextItemWidth(120.0F);
    ImGui::SliderFloat("Zoom##palette", &zoom, 0.5F, 6.0F, "%.1fx");
    ImGui::SameLine();
    ImGui::TextDisabled("click: tile, drag: stamp, ctrl/shift+click: several");
    if (selection_size(app) > 0)
    {
      ImGui::Text("%zu tiles selected", selection_size(app));
      ImGui::SameLine();
      deselect_all_button(app);
      ImGui::SameLine();
      ImGui::TextDisabled("(Tile metadata > Batch edit, Generate > WFC rules)");
    }
    if (!tex.valid())
    {
      ImGui::TextDisabled("No atlas loaded");
    }
    else
    {
      // The atlas, with the slicing grid and the selection drawn over it
      ImGui::BeginChild("atlas", ImVec2{0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      const ImVec2 size{tex.width * zoom, tex.height * zoom};
      ImGui::Image(texture_id(tex), size);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      const Extents grid = slice_grid(ts.slice, tex.width, tex.height);
      static int drag_start = -1;
      const auto tile_at = [&](ImVec2 p) -> int {
        for (int i = 0; i < static_cast<int>(grid.count()); ++i)
        {
          const TileRect r = tile_rect(ts.slice, tex.width, tex.height, static_cast<TileIndex>(i));
          if (
            p.x >= origin.x + r.x * zoom and p.y >= origin.y + r.y * zoom and
            p.x < origin.x + (r.x + r.width) * zoom and p.y < origin.y + (r.y + r.height) * zoom)
          {
            return i;
          }
        }
        return -1;
      };
      for (int i = 0; i < static_cast<int>(grid.count()); ++i)
      {
        const TileRect r = tile_rect(ts.slice, tex.width, tex.height, static_cast<TileIndex>(i));
        const ImVec2 a{origin.x + r.x * zoom, origin.y + r.y * zoom};
        const ImVec2 b{a.x + r.width * zoom, a.y + r.height * zoom};
        const bool hidden = static_cast<std::size_t>(i) < ts.tiles.size() and ts.tiles[i].hidden;
        if (app.selection.has(app.tileset, static_cast<TileIndex>(i)))
        {
          draw->AddRectFilled(a, b, IM_COL32(255, 230, 60, 50));
          draw->AddRect(a, b, IM_COL32(255, 230, 60, 200));
        }
        else
        {
          draw->AddRect(a, b, hidden ? IM_COL32(255, 80, 80, 60) : IM_COL32(255, 255, 255, 40));
        }
      }
      if (ImGui::IsItemHovered())
      {
        const int hover = tile_at(ImGui::GetIO().MousePos);
        if (hover >= 0)
        {
          const TileInfo& info = ts.tiles[hover];
          ImGui::SetTooltip("#%d %s", hover, info.name.c_str());
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
          {
            drag_start = hover;
          }
        }
        if (drag_start >= 0 and ImGui::IsMouseReleased(ImGuiMouseButton_Left) and hover >= 0)
        {
          // Ctrl and shift build up a selection (ctrl toggles, shift extends from the anchor); a
          // plain click or drag replaces it, and is also what the paint stamp follows
          const ImGuiIO& io = ImGui::GetIO();
          const bool multi = io.KeyCtrl or io.KeyShift;
          const int cols = std::max(1, grid.width);
          const int from = (io.KeyShift and app.selection.tileset == app.tileset) ? app.selection.anchor : drag_start;
          const int x0 = std::min(from % cols, hover % cols);
          const int y0 = std::min(from / cols, hover / cols);
          const int x1 = std::max(from % cols, hover % cols);
          const int y1 = std::max(from / cols, hover / cols);
          // Picking in the palette is the other kind of stamp, so the chosen group steps aside
          app.group.clear();
          app.selection.retarget(app.tileset);
          if (!multi)
          {
            app.selection.clear();
          }
          for (int y = y0; y <= y1; ++y)
          {
            for (int x = x0; x <= x1; ++x)
            {
              const auto tile = static_cast<TileIndex>(y * cols + x);
              if (tile < ts.tiles.size())
              {
                app.selection.set(tile, io.KeyCtrl ? !app.selection.has(tile) : true);
              }
            }
          }
          app.selection.anchor = static_cast<TileIndex>(hover);
          app.tile = multi ? static_cast<TileIndex>(hover) : static_cast<TileIndex>(y0 * cols + x0);
          if (!multi)
          {
            app.paint.tile = TileRef{static_cast<TileSetId>(app.tileset), app.tile};
            app.paint.stamp_width = x1 - x0 + 1;
            app.paint.stamp_height = y1 - y0 + 1;
            if (app.tool != Tool::kPaint and app.tool != Tool::kFill and app.tool != Tool::kRect)
            {
              app.tool = Tool::kPaint;
            }
          }
          drag_start = -1;
        }
      }
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
      {
        drag_start = -1;
      }
      // Selection
      if (app.paint.tile.tileset == app.tileset and grid.width > 0)
      {
        const TileRect r0 = tile_rect(ts.slice, tex.width, tex.height, app.paint.tile.tile);
        const ImVec2 a{origin.x + r0.x * zoom, origin.y + r0.y * zoom};
        const ImVec2 b{
          a.x + (app.paint.stamp_width * (ts.slice.tile_width + ts.slice.spacing_x) - ts.slice.spacing_x) * zoom,
          a.y + (app.paint.stamp_height * (ts.slice.tile_height + ts.slice.spacing_y) - ts.slice.spacing_y) * zoom};
        draw->AddRect(a, b, IM_COL32(255, 230, 60, 255), 0, 0, 2.0F);
      }
      ImGui::EndChild();
    }
  }
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Tile metadata
// -------------------------------------------------------------------------------------------------

namespace
{

/// Batch edit: the fields ticked here are applied to every tile selected in the palette
void batch_edit_section(App& app, TileSet& ts)
{
  const project::Project& pr = *app.project;
  TileEdit& edit = app.batch;
  ImGui::SeparatorText("Batch edit");
  const std::size_t count = selection_size(app);
  if (count < 2)
  {
    ImGui::TextDisabled("Ctrl/shift+click tiles in the palette to edit several of them at once");
    if (count == 1)
    {
      ImGui::SameLine();
      deselect_all_button(app);
    }
    return;
  }
  ImGui::Text("%zu tiles selected", count);
  ImGui::SameLine();
  if (ImGui::SmallButton(format<64>("Take from tile %u", static_cast<unsigned>(app.tile))))
  {
    const TileInfo& info = ts.tiles[app.tile];
    edit.semantic = info.semantic;
    edit.material = info.material;
    edit.collision = info.collision;
    edit.hidden = info.hidden;
    edit.scatter_weight = info.scatter_weight;
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Fills the fields below with the metadata of the tile shown above");
  }
  ImGui::SameLine();
  if (deselect_all_button(app))
  {
    return;
  }

  selection_preview(app, 24.0F, 5);

  // One field per row, behind the checkbox that says whether the batch edit writes it
  batch_field(
    "##batch_semantic", edit.set_semantic, [&] { catalog_combo("Semantic type##batch", pr.semantics, edit.semantic); });
  batch_field(
    "##batch_material", edit.set_material, [&] { catalog_combo("Material##batch", pr.materials, edit.material); });
  batch_field("##batch_hidden", edit.set_hidden, [&] {
    ImGui::Checkbox("Hidden from palettes/generation##batch", &edit.hidden);
  });
  batch_field("##batch_collision", edit.set_collision, [&] { collision_edit(edit.collision); });
  batch_field("##batch_weight", edit.set_scatter_weight, [&] {
    ImGui::SetNextItemWidth(110.0F);
    ImGui::DragFloat("Scatter weight##batch", &edit.scatter_weight, 0.05F, 0.0F, 1000.0F, "%.2f");
  });
  // A property is either set to a value or removed by key
  if (batch_field("##batch_property", edit.set_property, [&] {
        ImGui::SetNextItemWidth(110.0F);
        input_text("##batch_key", edit.property_key);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0F);
        input_text("##batch_value", edit.property_value);
        ImGui::SameLine();
        ImGui::TextUnformatted("Property");
      }))
  {
    edit.clear_property = edit.set_property ? false : edit.clear_property;
  }
  if (ImGui::Checkbox("Remove the property with that key instead", &edit.clear_property) and edit.clear_property)
  {
    edit.set_property = false;
  }

  if (ImGui::Button(format<64>("Apply to %zu tiles", count)))
  {
    const auto tiles = selected_tiles(app, app.memory.frame);
    const std::size_t edited = apply_edit(ts, edit, tiles.data(), tiles.size());
    LVL_LOG_INFO() << "Batch edit applied to " << edited << " tiles of '" << ts.name.view() << '\'';
    app.modified = true;
    invalidate_view(app);
  }
}

}  // namespace

void tile_meta_panel(App& app)
{
  if (!ImGui::Begin("Tile metadata", &app.panels.tile_meta))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  project::Project& pr = *app.project;
  if (app.tileset >= pr.tilesets.size() or pr.tilesets[app.tileset].tiles.empty())
  {
    ImGui::TextDisabled("Select a tile in the Tilesets palette");
    ImGui::End();
    return;
  }
  TileSet& ts = pr.tilesets[app.tileset];
  app.tile = std::min<TileIndex>(app.tile, static_cast<TileIndex>(ts.tiles.size() - 1));
  int index = app.tile;
  tile_image(app, TileRef{static_cast<TileSetId>(app.tileset), app.tile}, 48.0F);
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::SetNextItemWidth(100.0F);
  if (ImGui::InputInt("Tile", &index))
  {
    app.tile = static_cast<TileIndex>(std::clamp(index, 0, static_cast<int>(ts.tiles.size()) - 1));
  }
  TileInfo& info = ts.tiles[app.tile];
  bool changed = input_text("Name", info.name);
  changed |= ImGui::Checkbox("Hidden from palettes/generation", &info.hidden);
  ImGui::EndGroup();
  changed |= catalog_combo("Semantic type", pr.semantics, info.semantic);
  changed |= catalog_combo("Material", pr.materials, info.material);
  changed |= collision_edit(info.collision);
  ImGui::SetNextItemWidth(110.0F);
  changed |= ImGui::DragFloat("Scatter weight", &info.scatter_weight, 0.05F, 0.0F, 1000.0F, "%.2f");
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("How often a scattered fill (Generate > Fill) picks this tile, against the others it is "
                      "scattered with: 2 is picked twice as often as 1, and 0 keeps it out of the scatter.");
  }

  ImGui::SeparatorText("Properties");
  for (std::size_t i = 0; i < ts.properties.size(); ++i)
  {
    TileProperty& p = ts.properties[i];
    if (p.tile != app.tile)
    {
      continue;
    }
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Delete property"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kProperty,
          .index = i,
          .owner = app.tileset,
          .what = format<128>("property '%s' of tile %u", p.key.c_str(), static_cast<unsigned>(p.tile))});
    }
    ImGui::SetNextItemWidth(110.0F);
    changed |= input_text("##key", p.key);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0F);
    changed |= input_text("##value", p.value);
    ImGui::PopID();
  }
  if (ImGui::SmallButton("Add property"))
  {
    ts.properties.push_back(TileProperty{.tile = app.tile, .key = "key", .value = "value"});
    changed = true;
  }

  ImGui::SeparatorText("Seasonal variants");
  ImGui::TextDisabled("Tile and material used instead of this one during a season");
  for (std::size_t s = 0; s < pr.seasons.size(); ++s)
  {
    const Season& season = pr.seasons[s];
    ImGui::PushID(static_cast<int>(s));
    const TileIndex alt = seasonal_tile(ts, app.tile, season.name.view());
    const SeasonalVariant* existing = nullptr;
    for (const auto& v : ts.seasonal)
    {
      existing = (v.tile == app.tile and v.season == season.name) ? &v : existing;
    }
    SeasonalVariant variant = existing != nullptr
      ? *existing
      : SeasonalVariant{.tile = app.tile, .season = season.name, .alternate = app.tile, .material = kNoMaterial};
    tile_image(app, TileRef{static_cast<TileSetId>(app.tileset), alt}, 24.0F);
    ImGui::SameLine();
    ImGui::Text("%-12s", season.name.c_str());
    ImGui::SameLine();
    int alternate = variant.alternate;
    ImGui::SetNextItemWidth(90.0F);
    bool vchanged = ImGui::InputInt("##alt", &alternate);
    ImGui::SameLine();
    if (ImGui::SmallButton("Use palette tile") and app.paint.tile.tileset == app.tileset)
    {
      alternate = app.paint.tile.tile;
      vchanged = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0F);
    vchanged |= catalog_combo("##mat", pr.materials, variant.material);
    if (vchanged)
    {
      variant.alternate = static_cast<TileIndex>(std::clamp(alternate, 0, static_cast<int>(ts.tiles.size()) - 1));
      set_seasonal_variant(ts, variant);
      changed = true;
    }
    ImGui::PopID();
  }
  batch_edit_section(app, ts);
  if (changed)
  {
    app.modified = true;
    invalidate_view(app);
  }
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Catalogs & seasons
// -------------------------------------------------------------------------------------------------

void catalogs_panel(App& app)
{
  if (!ImGui::Begin("Catalogs & seasons", &app.panels.catalogs))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  Level& lv = *app.level;
  project::Project& pr = *app.project;
  bool changed = false;
  if (ImGui::CollapsingHeader("Level", ImGuiTreeNodeFlags_DefaultOpen))
  {
    changed |= input_text("Level name", lv.settings.name);
    ImGui::Text("Size: %d x %d cells (Edit > Resize)", lv.settings.extents.width, lv.settings.extents.height);
    changed |= ImGui::InputInt2("Cell size (px)", &lv.settings.cell_width);
    lv.settings.cell_width = std::max(1, lv.settings.cell_width);
    lv.settings.cell_height = std::max(1, lv.settings.cell_height);
    int seed = static_cast<int>(lv.settings.seed & 0x7FFFFFFF);
    if (ImGui::InputInt("Seed", &seed))
    {
      lv.settings.seed = static_cast<std::uint64_t>(std::max(0, seed));
      changed = true;
    }
  }
  if (ImGui::CollapsingHeader("Seasons", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImGui::TextDisabled(
      "The project cycles through its seasons in order; tilesets switch to seasonal variants by name.");
    std::uint32_t year = 0;
    for (std::size_t i = 0; i < pr.seasons.size(); ++i)
    {
      Season& s = pr.seasons[i];
      year += s.duration_days;
      ImGui::PushID(static_cast<int>(i));
      ImGui::BeginDisabled(pr.seasons.size() <= 1);
      if (delete_button(pr.seasons.size() <= 1 ? "A project needs at least one season" : "Delete season"))
      {
        request_delete(
          app,
          PendingDelete{
            .kind = DeleteKind::kSeason,
            .index = i,
            .what = format<128>("season '%s'", s.name.c_str()),
            .detail = "Tile variants for it stay in their tilesets (seasons are matched by name)."});
      }
      ImGui::EndDisabled();
      if (ImGui::TreeNode("season", "%s (%u days)", s.name.c_str(), s.duration_days))
      {
        changed |= input_text("Name", s.name);
        int days = static_cast<int>(s.duration_days);
        if (ImGui::InputInt("Duration (days)", &days))
        {
          s.duration_days = static_cast<std::uint32_t>(std::max(1, days));
          changed = true;
        }
        changed |= ImGui::DragFloat("Temperature (C)", &s.temperature, 0.1F, -60.0F, 60.0F);
        changed |= ImGui::DragFloat("Precipitation (mm/day)", &s.precipitation, 0.05F, 0.0F, 100.0F);
        changed |= ImGui::SliderFloat("Humidity", &s.humidity, 0.0F, 1.0F);
        changed |= ImGui::DragFloat("Wind (m/s)", &s.wind, 0.1F, 0.0F, 60.0F);
        changed |= ImGui::DragFloat("Snow below (C)", &s.snow_below, 0.1F, -30.0F, 10.0F);
        if (color_edit("Tint", s.tint))
        {
          changed = true;
          invalidate_view(app);
        }
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
    ImGui::TextDisabled("Year: %u days", year);
    if (ImGui::SmallButton("Add season") and pr.seasons.size() < 255)
    {
      project::add_season(pr, format<64>("season_%zu", pr.seasons.size() + 1));
      changed = true;
    }
  }
  const auto catalog_editor = [&](const char* title, auto& catalog, DeleteKind kind, auto&& extra) {
    if (!ImGui::CollapsingHeader(title))
    {
      return;
    }
    ImGui::PushID(title);
    for (std::size_t i = 1; i < catalog.size(); ++i)
    {
      ImGui::PushID(static_cast<int>(i));
      // A retired entry is still listed, greyed, because its id is still reserved: hiding it would
      // leave the user wondering why the ids below it never close up. "Restore" brings it back,
      // which is what every cell still holding its id already says it means.
      if (catalog[i].retired)
      {
        ImGui::TextDisabled("%s (removed)", catalog[i].name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore"))
        {
          catalog[i].retired = false;
          changed = true;
          invalidate_view(app);
        }
        ImGui::PopID();
        continue;
      }
      if (delete_button(kind == DeleteKind::kSemantic ? "Delete semantic type" : "Delete material"))
      {
        const bool semantic = (kind == DeleteKind::kSemantic);
        const std::size_t uses = semantic ? project::semantic_uses(pr, lv, static_cast<SemanticId>(i))
                                          : project::material_uses(pr, lv, static_cast<MaterialId>(i));
        request_delete(
          app,
          PendingDelete{
            .kind = kind,
            .index = i,
            .what = format<128>("%s '%s'", semantic ? "semantic type" : "material", catalog[i].name.c_str()),
            // The count is of this level only: the project's other levels are not loaded, so a
            // count of zero does not mean nothing anywhere uses the entry
            // Its id stays reserved, because the project's other levels are not loaded and their
            // cells would otherwise keep an id that had come to mean something else
            .detail = (uses == 0) ? FixedString<256>{"Nothing in this level uses it. Its id stays reserved until the "
                                                     "catalogs are compacted."}
                                  : FixedString<256>{format<256>(
                                      "%zu cells, tiles%s in this level use it; they become 'none'. Its id stays "
                                      "reserved until the catalogs are compacted. The undo history is cleared.",
                                      uses,
                                      semantic ? " and WFC rule tiles" : ", and seasonal variants")}});
      }
      if (color_edit("##color", catalog[i].color))
      {
        changed = true;
        invalidate_view(app);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(140.0F);
      changed |= input_text("##name", catalog[i].name);
      extra(catalog[i]);
      ImGui::PopID();
    }
    if (ImGui::SmallButton("Add") and catalog.size() < 0xFFFF)
    {
      catalog.emplace_back();
      catalog.back().name.assign(format<64>("entry_%zu", catalog.size() - 1));
      changed = true;
    }
    ImGui::PopID();
  };
  catalog_editor("Semantic types", pr.semantics, DeleteKind::kSemantic, [](SemanticType&) {});
  catalog_editor("Materials", pr.materials, DeleteKind::kMaterial, [&changed](Material& m) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0F);
    changed |= ImGui::DragFloat("friction", &m.friction, 0.01F, 0.0F, 4.0F);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0F);
    changed |= ImGui::DragFloat("speed", &m.speed, 0.01F, 0.0F, 4.0F);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    changed |= input_text("sound", m.sound);
  });

  // Compaction is the only thing that renumbers the catalogs, which is why it is a deliberate act
  // rather than something a removal does: every level of the project has to be rewritten together.
  if (project::needs_compaction(pr))
  {
    ImGui::Separator();
    project::CatalogPlan plan{app.memory.frame};
    project::plan_compaction(pr, plan);
    ImGui::TextWrapped(
      "%zu semantic type(s) and %zu material(s) are removed but still hold their ids.",
      plan.retired_semantics,
      plan.retired_materials);
    if (pr.levels.size() > 1)
    {
      ImGui::TextDisabled("Compacting needs every level rewritten at once, which this build cannot do yet.");
    }
    else if (ImGui::Button("Compact catalogs"))
    {
      project::apply_compaction(plan, lv);
      project::finish_compaction(pr, plan);
      // Every id below a dropped entry has moved, so the snapshots in the ring no longer mean what
      // their planes do
      app.undo.clear();
      invalidate_view(app);
      changed = true;
    }
  }
  app.modified |= changed;
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Project: its member levels, tilesets and rule sets
// -------------------------------------------------------------------------------------------------

void project_panel(App& app)
{
  if (!ImGui::Begin("Project", &app.panels.project))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  project::Project& pr = *app.project;
  const Level& lv = *app.level;

  if (app.project_path.empty())
  {
    // What opening a bare level gives: a project synthesized around it, with nowhere to save to
    ImGui::TextWrapped("This level has no project folder yet. File > New project writes one.");
  }
  else
  {
    ImGui::Text("%s%s", pr.name.c_str(), app.project_modified ? " *" : "");
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("%s", app.project_path.c_str());
    }
  }
  if (input_text("Name", pr.name))
  {
    app.project_modified = true;
  }

  ImGui::SeparatorText("Levels");
  const std::size_t open_member = project::find_level(pr, lv.settings.name.view());
  for (std::size_t i = 0; i < pr.levels.size(); ++i)
  {
    project::LevelEntry& entry = pr.levels[i];
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Remove from the project (the file stays on disk)"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kLevelMember,
          .index = i,
          .what = format<128>("level '%s' from the project", entry.name.c_str()),
          .detail = "Its file stays on disk. Portals pointing at it will show as dangling."});
    }
    const bool is_open = (i == open_member);
    if (is_open)
    {
      // The open one is the document: it is renamed through the level's own settings, not here
      ImGui::TextDisabled("%s (open)", entry.name.c_str());
    }
    else
    {
      ImGui::SetNextItemWidth(140.0F);
      if (input_text("##name", entry.name))
      {
        app.project_modified = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Open"))
      {
        // Leaves the unsaved-changes prompt to request_switch, as quitting does
        request_switch(app, i);
      }
    }
    ImGui::SameLine();
    if (entry.missing)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.45F, 0.40F, 1.0F});
      ImGui::Text("%s (file not found)", entry.file.c_str());
      ImGui::PopStyleColor();
    }
    else
    {
      ImGui::TextDisabled(
        "%dx%d, %u portal(s)  %s", entry.extents.width, entry.extents.height, entry.portal_count, entry.file.c_str());
    }
    ImGui::PopID();
  }
  if (pr.levels.empty())
  {
    ImGui::TextDisabled("No levels yet. Saving this one makes it the first.");
  }
  if (ImGui::SmallButton("Add a level..."))
  {
    open_file_dialog(app, FileAction::kAddLevel);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Rescan"))
  {
    // What the index says is only as fresh as the last save this editor did
    if (auto* job = prepare_job<RescanProjectJob>(app); job != nullptr)
    {
      job->project = app.project;
      job->scratch = app.spare;
      job->scratch_project = app.spare_project;
      start_job(app);
    }
  }

  ImGui::SeparatorText("Tilesets");
  for (std::size_t i = 0; i < pr.tilesets.size(); ++i)
  {
    const level::TileSet& ts = pr.tilesets[i];
    const bool outside =
      !ts.image.empty() and !app.project_path.empty() and !project::is_inside(pr.root.view(), ts.image.view());
    ImGui::PushID(static_cast<int>(i));
    ImGui::BulletText("%s  %s", ts.name.c_str(), ts.image.empty() ? "(no atlas)" : ts.image.c_str());
    if (outside)
    {
      // A reference that points outside the tree is allowed -- the import prompt's "Link in place"
      // -- but it is what stops the folder being self-contained, so it is listed as such with the
      // way to fix it beside it
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.80F, 0.40F, 1.0F});
      ImGui::TextUnformatted("outside the project");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy in"))
      {
        app.tileset = i;
        copy_asset_into_project(app, FileAction::kAtlasImage, ts.image.view());
      }
    }
    ImGui::PopID();
  }
  if (pr.tilesets.empty())
  {
    ImGui::TextDisabled("None; import one in Tilesets");
  }
  if (
    !app.generators.seed_image.empty() and !app.project_path.empty() and
    !project::is_inside(pr.root.view(), app.generators.seed_image.view()))
  {
    ImGui::BulletText("heightmap seed  %s", app.generators.seed_image.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.80F, 0.40F, 1.0F});
    ImGui::TextUnformatted("outside the project");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy in##seed"))
    {
      copy_asset_into_project(app, FileAction::kSeedImage, app.generators.seed_image.view());
    }
  }

  ImGui::SeparatorText("Rule sets");
  for (std::size_t i = 0; i < pr.rules.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (delete_button("Delete rule set"))
    {
      request_delete(
        app,
        PendingDelete{
          .kind = DeleteKind::kRuleSet,
          .index = i,
          .what = format<128>("rule set '%s'", pr.rules[i].wfc.name.c_str()),
          .detail = "Levels naming it fall back to the project's first set."});
    }
    const bool used = (i == project::rules_of(pr, lv));
    ImGui::Text(
      "%s%s  %zu tile(s), %zu terrain(s)",
      pr.rules[i].wfc.name.c_str(),
      used ? " (this level)" : "",
      pr.rules[i].wfc.tiles.size(),
      pr.rules[i].auto_tiles.terrains.size() + pr.rules[i].steps.terrains.size());
    ImGui::PopID();
  }
  if (ImGui::SmallButton("Add a rule set"))
  {
    project::add_rules(pr, format<64>("rules_%zu", pr.rules.size() + 1));
    app.project_modified = true;
  }
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Objects & paths
// -------------------------------------------------------------------------------------------------

/**
 * @brief The two destination fields of a portal, as combos over what the project knows
 *
 * Levels address each other by name, and the project's index holds every member's name and the
 * names of its portals, so both can be picked rather than typed -- which is the point: a typed path
 * was indistinguishable from a working link. A value that resolves to nothing is kept and shown, in
 * red and disabled, so it can be seen and replaced rather than silently dropped.
 */
bool portal_target(App& app, Portal& portal)
{
  const project::Project& pr = *app.project;
  const Level& lv = *app.level;
  bool changed = false;

  const bool self = portal.target_level.empty() or portal.target_level == lv.settings.name;
  const char* preview = self ? "(this level)" : portal.target_level.c_str();
  if (ImGui::BeginCombo("Target level", preview))
  {
    if (ImGui::Selectable("(this level)", self))
    {
      portal.target_level.clear();
      changed = true;
    }
    for (const project::LevelEntry& entry : pr.levels)
    {
      if (entry.name == lv.settings.name)
      {
        // Already offered as "(this level)"
        continue;
      }
      if (ImGui::Selectable(entry.name.c_str(), portal.target_level == entry.name))
      {
        portal.target_level = entry.name;
        portal.target_portal.clear();
        changed = true;
      }
    }
    // A name no member has: still listed, so it can be seen rather than silently replaced
    if (!self and project::find_level(pr, portal.target_level.view()) == project::kNoLevel)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.45F, 0.40F, 1.0F});
      ImGui::Selectable(
        format<96>("%s (no such level)", portal.target_level.c_str()), true, ImGuiSelectableFlags_Disabled);
      ImGui::PopStyleColor();
    }
    ImGui::EndCombo();
  }

  // The destination's portals: from the live registry when it is this level, so one made this
  // session is offered before any save; from the index otherwise, which is what a project knows
  // about a level it has not loaded
  const char* portal_preview = portal.target_portal.empty() ? "(none)" : portal.target_portal.c_str();
  if (ImGui::BeginCombo("Target portal", portal_preview))
  {
    if (ImGui::Selectable("(none)", portal.target_portal.empty()))
    {
      portal.target_portal.clear();
      changed = true;
    }
    bool listed = false;
    const auto offer = [&portal, &changed, &listed](std::string_view name) {
      listed = true;
      if (ImGui::Selectable(
            format<96>("%.*s", static_cast<int>(name.size()), name.data()), portal.target_portal == name))
      {
        portal.target_portal.assign(name);
        changed = true;
      }
    };
    if (self)
    {
      for (auto [e, label, other] : lv.registry.view<Label, Portal>().each())
      {
        offer(label.name.view());
      }
    }
    else if (const std::size_t member = project::find_level(pr, portal.target_level.view());
             member != project::kNoLevel)
    {
      const project::LevelEntry& entry = pr.levels[member];
      const Name* names = project::portals_of(pr, entry);
      for (std::uint32_t i = 0; i < entry.portal_count; ++i)
      {
        offer(names[i].view());
      }
    }
    if (!listed)
    {
      ImGui::TextDisabled(self ? "This level has no other portal" : "That level had no portals when it was saved");
    }
    if (!portal.target_portal.empty() and portal_link_of(app, portal) == project::LinkState::kUnknownPortal)
    {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.45F, 0.40F, 1.0F});
      ImGui::Selectable(
        format<96>("%s (no such portal)", portal.target_portal.c_str()), true, ImGuiSelectableFlags_Disabled);
      ImGui::PopStyleColor();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void objects_panel(App& app)
{
  if (!ImGui::Begin("Objects & paths", &app.panels.objects))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  Level& lv = *app.level;
  Registry& reg = lv.registry;
  bool changed = false;

  if (ImGui::CollapsingHeader("Path graphs", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::SmallButton("Add graph"))
    {
      app.graph = add_graph(lv, format<64>("graph_%zu", lv.graphs.size() + 1), GraphKind::kRoad);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Import..."))
    {
      open_file_dialog(app, FileAction::kLoadPaths);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Export..."))
    {
      open_file_dialog(app, FileAction::kSavePaths);
    }
    for (std::size_t g = 0; g < lv.graphs.size(); ++g)
    {
      PathGraph& graph = lv.graphs[g];
      ImGui::PushID(static_cast<int>(g));
      std::size_t nodes = 0;
      for (auto [e, node] : reg.view<PathNode>().each())
      {
        nodes += (node.graph == g) ? 1 : 0;
      }
      if (delete_button("Delete graph"))
      {
        request_delete(
          app,
          PendingDelete{
            .kind = DeleteKind::kGraph,
            .index = g,
            .what = format<128>("path graph '%s'", graph.name.c_str()),
            .detail = format<256>("Its %zu nodes and their edges are deleted too.", nodes)});
      }
      ImGui::Checkbox("##vis", &graph.visible);
      ImGui::SameLine();
      changed |= color_edit("##color", graph.color);
      ImGui::SameLine();
      if (ImGui::Selectable(format<128>("%s (%zu nodes)", graph.name.c_str(), nodes), app.graph == g))
      {
        app.graph = static_cast<GraphId>(g);
      }
      ImGui::PopID();
    }
    if (app.graph < lv.graphs.size())
    {
      PathGraph& graph = lv.graphs[app.graph];
      changed |= input_text("Graph name", graph.name);
      changed |= enum_combo("Kind", graph.kind);
      if (ImGui::SmallButton("Delete graph"))
      {
        remove_graph(lv, app.graph);
        app.graph = 0;
        app.entity = kNullEntity;
        app.route_from = kNullEntity;
        changed = true;
      }
    }
    ImGui::TextDisabled("Tools: Node (N), Edge (K). Select a node, then 'Route from here' to preview routes.");
  }

  if (ImGui::CollapsingHeader("Portals & markers", ImGuiTreeNodeFlags_DefaultOpen))
  {
    for (auto [e, label, portal] : reg.view<Label, Portal>().each())
    {
      ImGui::PushID(static_cast<int>(entt::to_integral(e)));
      if (delete_button("Delete portal"))
      {
        request_delete(
          app,
          PendingDelete{
            .kind = DeleteKind::kObject, .entity = e, .what = format<128>("portal '%s'", label.name.c_str())});
      }
      ImGui::PopID();
      // A link the project cannot follow is drawn red, with what is missing in the tooltip, so a
      // typo is not indistinguishable from a working link
      const auto link = portal_link_of(app, portal);
      const bool dangling = link != project::LinkState::kResolved and link != project::LinkState::kSelf;
      if (dangling)
      {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95F, 0.45F, 0.40F, 1.0F});
      }
      if (ImGui::Selectable(
            format<128>(
              "portal: %s -> %s:%s##%u",
              label.name.c_str(),
              portal.target_level.empty() ? "(this level)" : portal.target_level.c_str(),
              portal.target_portal.c_str(),
              static_cast<unsigned>(e)),
            app.entity == e))
      {
        app.entity = e;
      }
      if (dangling)
      {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
        {
          ImGui::SetTooltip("%s", link_message(link, portal));
        }
      }
    }
    for (auto [e, label, marker] : reg.view<Label, Marker>().each())
    {
      ImGui::PushID(static_cast<int>(entt::to_integral(e)));
      if (delete_button("Delete marker"))
      {
        request_delete(
          app,
          PendingDelete{
            .kind = DeleteKind::kObject, .entity = e, .what = format<128>("marker '%s'", label.name.c_str())});
      }
      ImGui::PopID();
      if (ImGui::Selectable(
            format<128>("marker: %s (%s)##%u", label.name.c_str(), marker.type.c_str(), static_cast<unsigned>(e)),
            app.entity == e))
      {
        app.entity = e;
      }
    }
  }

  if (reg.valid(app.entity))
  {
    ImGui::SeparatorText("Selected");
    const Entity e = app.entity;
    if (auto* label = reg.try_get<Label>(e); label != nullptr)
    {
      changed |= input_text("Name", label->name);
    }
    if (auto* p = reg.try_get<Position>(e); p != nullptr)
    {
      changed |= ImGui::DragFloat2("Position", &p->x, 0.05F);
    }
    if (auto* ext = reg.try_get<Extent>(e); ext != nullptr)
    {
      changed |= ImGui::DragFloat2("Size", &ext->width, 0.05F, 0.1F, 1000.0F);
    }
    if (auto* portal = reg.try_get<Portal>(e); portal != nullptr)
    {
      changed |= portal_target(app, *portal);
      changed |= enum_combo("Exit facing", portal->exit);
      changed |= ImGui::Checkbox("Requires interaction", &portal->interact);
    }
    if (auto* marker = reg.try_get<Marker>(e); marker != nullptr)
    {
      changed |= input_text("Type", marker->type);
    }
    if (auto* node = reg.try_get<PathNode>(e); node != nullptr)
    {
      ImGui::Text("Graph: %s", node->graph < lv.graphs.size() ? lv.graphs[node->graph].name.c_str() : "?");
      changed |= ImGui::DragFloat("Wait (s)", &node->wait, 0.1F, 0.0F, 3600.0F);
      if (ImGui::SmallButton("Route from here"))
      {
        app.route_from = e;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear route"))
      {
        app.route_from = kNullEntity;
      }
      // Edges touching this node
      for (auto [edge_e, edge] : reg.view<PathEdge>().each())
      {
        if (edge.from != e and edge.to != e)
        {
          continue;
        }
        ImGui::PushID(static_cast<int>(edge_e));
        ImGui::SetNextItemWidth(80.0F);
        changed |= ImGui::DragFloat("cost", &edge.cost, 0.05F, 0.01F, 100.0F);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("two-way", &edge.bidirectional);
        ImGui::PopID();
      }
    }
    if (ImGui::Button("Delete"))
    {
      if (reg.all_of<PathNode>(e))
      {
        destroy_path_node(reg, e);
      }
      else
      {
        reg.destroy(e);
      }
      app.entity = kNullEntity;
      changed = true;
    }
  }
  app.modified |= changed;
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Mini-preview
// -------------------------------------------------------------------------------------------------

void minimap_panel(App& app)
{
  if (!ImGui::Begin("Mini-preview", &app.panels.minimap))
  {
    ImGui::End();
    return;
  }
  ImGui::SetNextItemWidth(120.0F);
  if (ImGui::SliderInt("Max size", &app.minimap_size, 32, 1024))
  {
    app.minimap_dirty = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Export..."))
  {
    open_file_dialog(app, FileAction::kExportMinimap);
  }
  ImGui::Checkbox("Objects", &app.export_options.objects);
  ImGui::SameLine();
  ImGui::Checkbox("Overlays", &app.export_options.overlays);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(80.0F);
  ImGui::InputInt("Map export px/cell", &app.export_options.pixels_per_cell);
  app.export_options.pixels_per_cell = std::clamp(app.export_options.pixels_per_cell, 0, 256);
  if (app.minimap_dirty and std::holds_alternative<std::monostate>(app.job))
  {
    memory::Arena& frame = app.memory.frame;
    memory::Scope scope{frame};
    memory::Arena result;
    memory::Arena work;
    memory::push(frame, result, memory::available(frame) / 4, "minimap");
    memory::push(frame, work, "minimap-work");
    render::ComposeOptions options = app.export_options;
    options.season = view_season(app);
    options.tint = app.view.tint;
    image::Image img;
    if (render::minimap(
          *app.project,
          *app.level,
          app.assets.atlases(app.project->tilesets.size()),
          options,
          app.minimap_size,
          result,
          work,
          img)
          .ok)
    {
      upload(app.minimap_texture, img, true);
    }
    memory::pop(frame, work);
    memory::pop(frame, result);
    app.minimap_dirty = false;
  }
  if (app.minimap_texture.valid())
  {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float s = std::min(avail.x / app.minimap_texture.width, avail.y / app.minimap_texture.height);
    const ImVec2 size{app.minimap_texture.width * std::max(s, 0.1F), app.minimap_texture.height * std::max(s, 0.1F)};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(texture_id(app.minimap_texture), size);
    // Click to center the viewport
    if (ImGui::IsItemHovered() and ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      const auto& e = app.level->settings.extents;
      const ImVec2 m = ImGui::GetIO().MousePos;
      const float cx = (m.x - origin.x) / size.x * e.width;
      const float cy = (m.y - origin.y) / size.y * e.height;
      const float visible_w = 400.0F / app.view.cell_pixels;
      const float visible_h = 300.0F / app.view.cell_pixels;
      app.view.x = cx - visible_w;
      app.view.y = cy - visible_h;
    }
  }
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// Log
// -------------------------------------------------------------------------------------------------

void log_panel(App& app)
{
  if (!ImGui::Begin("Log", &app.panels.log))
  {
    ImGui::End();
    return;
  }
  if (ImGui::SmallButton("Clear"))
  {
    clearLogHistory();
  }
  ImGui::SameLine();
  static bool follow = true;
  ImGui::Checkbox("Follow", &follow);
  ImGui::BeginChild("lines", ImVec2{0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  visitLogHistory([](const LogHistoryLine& line) {
    // Info/debug follow the style's text colors, so the log reads in light and dark styles alike
    ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    switch (line.severity)
    {
    case LogSeverity::kDebug:
      color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
      break;
    case LogSeverity::kWarn:
      color = ImVec4{0.9F, 0.6F, 0.1F, 1.0F};
      break;
    case LogSeverity::kError:
    case LogSeverity::kFatal:
      color = ImVec4{0.9F, 0.3F, 0.25F, 1.0F};
      break;
    default:
      break;
    }
    // Drop the "[LVL] (Severity:file:line) " prefix for display
    const char* text = line.text;
    if (const char* close = std::strstr(text, ") "); close != nullptr and std::strncmp(text, "[LVL]", 5) == 0)
    {
      text = close + 2;
    }
    ImGui::TextColored(color, "%s", text);
  });
  static std::size_t seen = 0;
  if (follow and getLogHistorySequence() != seen)
  {
    ImGui::SetScrollHereY(1.0F);
    seen = getLogHistorySequence();
  }
  ImGui::EndChild();
  ImGui::End();
}

// -------------------------------------------------------------------------------------------------
// UI style
// -------------------------------------------------------------------------------------------------

void style_panel(App& app)
{
  if (!ImGui::Begin("Style", &app.panels.style))
  {
    ImGui::End();
    return;
  }
  static style::Preset preset = style::Preset::kLvl;
  ImGui::SetNextItemWidth(120.0F);
  enum_combo("##preset", preset);
  ImGui::SameLine();
  if (ImGui::Button("Apply preset"))
  {
    style::apply(preset, app.style_reference);
    style::activate(app.style_reference);
  }
  ImGui::SameLine();
  if (ImGui::Button("Revert"))
  {
    style::activate(app.style_reference);
  }
  ImGui::SetNextItemWidth(120.0F);
  ImGui::SliderFloat("Font scale", &ImGui::GetIO().FontGlobalScale, 0.5F, 2.5F, "%.2f");

  if (ImGui::Button("Save as default"))
  {
    file_action(app, FileAction::kSaveStyle, style::default_path());
  }
  ImGui::SameLine();
  if (ImGui::Button("Load default"))
  {
    file_action(app, FileAction::kLoadStyle, style::default_path());
  }
  ImGui::SameLine();
  if (ImGui::Button("Save..."))
  {
    open_file_dialog(app, FileAction::kSaveStyle);
  }
  ImGui::SameLine();
  if (ImGui::Button("Load..."))
  {
    open_file_dialog(app, FileAction::kLoadStyle);
  }
  ImGui::TextDisabled("Default profile: %s (loaded at startup)", style::default_path());
  ImGui::Separator();
  // Dear ImGui's own style editor; its "Revert Ref" restores the reference above
  ImGui::BeginChild("editor");
  ImGui::ShowStyleEditor(&app.style_reference.style);
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace lvl::editor
