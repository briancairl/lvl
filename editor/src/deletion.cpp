/**
 * @copyright 2026-present Brian Cairl
 *
 * @file deletion.cpp
 *
 * Delete buttons: request, confirm, apply. Deletes are applied after every panel has drawn, so no
 * panel is iterating over what gets removed.
 */

// C++ Standard Library
#include <algorithm>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/logging.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

/// Keeps a catalog id pointing at the same entry after entry <code>removed</code> is erased
std::uint16_t remap(std::uint16_t v, std::uint16_t removed)
{
  return (v == removed) ? 0 : (v > removed ? static_cast<std::uint16_t>(v - 1) : v);
}

/// Forgets selections that refer to entities which no longer exist
void drop_stale_selection(App& app)
{
  const Registry& reg = app.level->registry;
  app.entity = reg.valid(app.entity) ? app.entity : kNullEntity;
  app.route_from = reg.valid(app.route_from) ? app.route_from : kNullEntity;
  app.edge_start = reg.valid(app.edge_start) ? app.edge_start : kNullEntity;
}

void apply(App& app, const PendingDelete& d)
{
  Level& lv = *app.level;
  project::Project& pr = *app.project;
  switch (d.kind)
  {
  case DeleteKind::kNone:
    return;
  case DeleteKind::kLayer:
    if (d.index >= lv.layers.size())
    {
      return;
    }
    remove_layer(lv, d.index);
    app.layer =
      (app.layer > d.index) ? app.layer - 1 : std::min(app.layer, lv.layers.empty() ? 0 : lv.layers.size() - 1);
    app.generators.height_layer = (app.generators.height_layer > d.index) ? app.generators.height_layer - 1 : 0;
    app.undo.clear();
    break;
  case DeleteKind::kSemantic:
    if (d.index == 0 or d.index >= pr.semantics.size())
    {
      return;
    }
    project::remove_semantic(pr, lv, static_cast<SemanticId>(d.index));
    app.paint.semantic = remap(app.paint.semantic, static_cast<std::uint16_t>(d.index));
    app.undo.clear();
    break;
  case DeleteKind::kMaterial:
    if (d.index == 0 or d.index >= pr.materials.size())
    {
      return;
    }
    project::remove_material(pr, lv, static_cast<MaterialId>(d.index));
    app.paint.material = remap(app.paint.material, static_cast<std::uint16_t>(d.index));
    app.undo.clear();
    break;
  case DeleteKind::kSeason:
    if (d.index >= pr.seasons.size() or pr.seasons.size() <= 1)
    {
      return;
    }
    project::remove_season(pr, static_cast<SeasonId>(d.index));
    app.view.season = 0;
    break;
  case DeleteKind::kTerrain:
    if (d.index >= rule_set(app).auto_tiles.terrains.size())
    {
      return;
    }
    remove_terrain(rule_set(app).auto_tiles, static_cast<TerrainId>(d.index));
    app.terrain = 0;
    break;
  case DeleteKind::kStepTerrain:
    if (d.index >= rule_set(app).steps.terrains.size())
    {
      return;
    }
    remove_step_terrain(rule_set(app).steps, static_cast<StepTerrainId>(d.index));
    app.step_terrain = 0;
    break;
  case DeleteKind::kLevelMember:
    if (d.index >= pr.levels.size())
    {
      return;
    }
    // The member leaves the index; its file stays on disk. Portals naming it are left as they are
    // and show as dangling, which is what they now are -- quietly clearing them would throw away
    // what the user wrote on the strength of a removal they may be undoing by hand.
    project::remove_level(pr, d.index, app.memory.frame);
    app.project_modified = true;
    break;
  case DeleteKind::kRuleSet:
    if (d.index >= pr.rules.size())
    {
      return;
    }
    project::remove_rules(pr, d.index);
    app.project_modified = true;
    break;
  case DeleteKind::kGraph:
    if (d.index >= lv.graphs.size())
    {
      return;
    }
    remove_graph(lv, static_cast<GraphId>(d.index));
    app.graph = 0;
    drop_stale_selection(app);
    break;
  case DeleteKind::kObject:
    if (!lv.registry.valid(d.entity))
    {
      return;
    }
    if (lv.registry.all_of<PathNode>(d.entity))
    {
      destroy_path_node(lv.registry, d.entity);
    }
    else
    {
      lv.registry.destroy(d.entity);
    }
    drop_stale_selection(app);
    break;
  case DeleteKind::kTileGroup:
    if (d.owner >= pr.tilesets.size() or d.index >= pr.tilesets[d.owner].groups.size())
    {
      return;
    }
    remove_group(pr.tilesets[d.owner], static_cast<GroupId>(d.index));
    // The groups after it have moved down one, so what is being stamped follows or stops
    if (app.group.tileset == d.owner and app.group.active())
    {
      app.group.group = (app.group.group == d.index)
        ? kNoGroup
        : static_cast<GroupId>(app.group.group - ((app.group.group > d.index) ? 1 : 0));
    }
    app.project_modified = true;
    break;
  case DeleteKind::kScatterProfile:
    if (d.index >= pr.scatter.size())
    {
      return;
    }
    project::remove_scatter(pr, d.index);
    app.generators.scatter_profile = (app.generators.scatter_profile > d.index)
      ? app.generators.scatter_profile - 1
      : std::min(app.generators.scatter_profile, pr.scatter.empty() ? 0 : pr.scatter.size() - 1);
    app.project_modified = true;
    break;
  case DeleteKind::kScatterObject:
    if (d.owner >= pr.scatter.size() or d.index >= pr.scatter[d.owner].count)
    {
      return;
    }
    project::remove_scatter_object(pr, d.owner, d.index);
    app.project_modified = true;
    break;
  case DeleteKind::kProperty:
    if (d.owner >= pr.tilesets.size() or d.index >= pr.tilesets[d.owner].properties.size())
    {
      return;
    }
    pr.tilesets[d.owner].properties.erase(
      pr.tilesets[d.owner].properties.begin() + static_cast<std::ptrdiff_t>(d.index));
    break;
  }
  LVL_LOG_INFO() << "Deleted " << d.what;
  app.modified = true;
  invalidate_view(app);
}

}  // namespace

bool delete_button(const char* tooltip)
{
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.75F, 0.2F, 0.2F, 1.0F});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.9F, 0.25F, 0.25F, 1.0F});
  const bool clicked = ImGui::SmallButton("x##delete");
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("%s\n(Shift+click: no confirmation)", tooltip);
  }
  ImGui::SameLine();
  return clicked;
}

void request_delete(App& app, const PendingDelete& request)
{
  // The dialog itself is opened by process_deletes(), outside any panel's ID scope
  app.pending_delete = request;
  app.pending_delete.confirmed = ImGui::GetIO().KeyShift;
}

void process_deletes(App& app)
{
  PendingDelete& d = app.pending_delete;
  // Jobs read the document without locks: deletes wait for them
  if (d.kind == DeleteKind::kNone or busy(app))
  {
    return;
  }
  if (d.confirmed)
  {
    apply(app, d);
    d = PendingDelete{};
    return;
  }
  if (!ImGui::IsPopupOpen("Delete?"))
  {
    ImGui::OpenPopup("Delete?");
  }
  if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Delete %s?", d.what.c_str());
    if (!d.detail.empty())
    {
      ImGui::PushTextWrapPos(460.0F);
      ImGui::TextUnformatted(d.detail.c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("This can't be undone.");
    if (ImGui::Button("Delete") or ImGui::IsKeyPressed(ImGuiKey_Enter))
    {
      d.confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") or ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
      d = PendingDelete{};
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace lvl::editor
