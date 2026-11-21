/**
 * @copyright 2026-present Brian Cairl
 *
 * @file generate.cpp
 *
 * Generators (heightmap from layered noise / seed images, iterative water-shedding, WFC) and the
 * visual WFC rule editor, as tabs of the Generate panel.
 */

// C++ Standard Library
#include <algorithm>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/random.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

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

/// Combo over the layers of one kind; returns false if there are none
bool layer_combo(const char* label, const Level& lv, LayerKind kind, std::size_t& index)
{
  const std::size_t first = find_layer(lv, kind);
  if (first == lv.layers.size())
  {
    ImGui::TextDisabled("(no %s layer)", enum_name(kind).data());
    return false;
  }
  if (index >= lv.layers.size() or lv.layers[index].kind != kind)
  {
    index = first;
  }
  if (ImGui::BeginCombo(label, lv.layers[index].name.c_str()))
  {
    for (std::size_t i = 0; i < lv.layers.size(); ++i)
    {
      if (
        lv.layers[i].kind == kind and
        ImGui::Selectable(format<96>("%s##%zu", lv.layers[i].name.c_str(), i), i == index))
      {
        index = i;
      }
    }
    ImGui::EndCombo();
  }
  return true;
}

void seed_input(App& app)
{
  int seed = static_cast<int>(app.level->settings.seed & 0x7FFFFFFF);
  ImGui::SetNextItemWidth(120.0F);
  if (ImGui::InputInt("Seed", &seed))
  {
    app.level->settings.seed = static_cast<std::uint64_t>(std::max(seed, 0));
    app.modified = true;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Randomize"))
  {
    static random::State rng = random::seeded(0xC0FFEE);
    app.level->settings.seed = random::next(rng) & 0x7FFFFFFF;
    app.modified = true;
  }
}

void heightmap_section(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  procgen::HeightmapParams& p = g.heightmap;
  if (!layer_combo("Height layer", lv, LayerKind::kHeight, g.height_layer))
  {
    if (ImGui::SmallButton("Add height layer"))
    {
      g.height_layer = add_layer(lv, LayerKind::kHeight, "height");
      invalidate_view(app);
    }
    return;
  }
  for (std::size_t i = 0; i < p.layer_count; ++i)
  {
    procgen::NoiseLayer& n = p.layers[i];
    ImGui::PushID(static_cast<int>(i));
    const bool open =
      ImGui::TreeNodeEx("noise", ImGuiTreeNodeFlags_DefaultOpen, "Noise %zu (%s)", i + 1, enum_name(n.kind).data());
    if (open)
    {
      ImGui::Checkbox("Enabled", &n.enabled);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(100.0F);
      enum_combo("Kind", n.kind);
      if (i > 0)
      {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0F);
        enum_combo("Blend", n.blend);
      }
      ImGui::DragFloat("Frequency", &n.frequency, 0.0005F, 0.0005F, 1.0F, "%.4f");
      ImGui::DragFloat("Amplitude", &n.amplitude, 0.01F, 0.0F, 4.0F);
      ImGui::SliderInt("Octaves", &n.octaves, 1, 10);
      ImGui::DragFloat("Lacunarity", &n.lacunarity, 0.01F, 1.0F, 4.0F);
      ImGui::DragFloat("Gain", &n.gain, 0.01F, 0.0F, 1.0F);
      ImGui::DragFloat2("Offset", &n.offset_x, 1.0F);
      int seed_offset = static_cast<int>(n.seed_offset);
      if (ImGui::InputInt("Seed offset", &seed_offset))
      {
        n.seed_offset = static_cast<std::uint32_t>(std::max(seed_offset, 0));
      }
      if (p.layer_count > 1 and ImGui::SmallButton("Remove noise layer"))
      {
        for (std::size_t k = i; k + 1 < p.layer_count; ++k)
        {
          p.layers[k] = p.layers[k + 1];
        }
        --p.layer_count;
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (p.layer_count < procgen::kMaxNoiseLayers and ImGui::SmallButton("Add noise layer"))
  {
    p.layers[p.layer_count++] = procgen::NoiseLayer{
      .frequency = 0.08F, .amplitude = 0.3F, .seed_offset = static_cast<std::uint32_t>(p.layer_count)};
  }
  ImGui::SeparatorText("Seed image");
  input_text("Image", g.seed_image);
  ImGui::SameLine();
  if (ImGui::SmallButton("...##seed"))
  {
    open_file_dialog(app, FileAction::kSeedImage);
  }
  ImGui::SliderFloat("Image weight", &p.seed_weight, 0.0F, 1.0F);
  ImGui::SameLine();
  ImGui::Checkbox("Invert", &p.seed_invert);
  ImGui::SeparatorText("Shaping");
  ImGui::SliderFloat("Island falloff", &p.falloff, 0.0F, 1.0F);
  ImGui::Checkbox("Normalize", &p.normalize);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(100.0F);
  ImGui::DragFloat("Exponent", &p.exponent, 0.01F, 0.1F, 5.0F);
  seed_input(app);
  if (ImGui::Button("Generate heightmap", ImVec2{-1, 0}))
  {
    start_heightmap(app);
  }
}

void watershed_section(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  procgen::WatershedParams& p = g.watershed;
  if (!layer_combo("Height layer##ws", lv, LayerKind::kHeight, g.height_layer))
  {
    return;
  }
  ImGui::SliderInt("Iterations", &p.iterations, 1, 20);
  ImGui::Checkbox("Hydraulic erosion", &p.erode);
  if (p.erode and ImGui::TreeNode("Erosion"))
  {
    int droplets = static_cast<int>(p.erosion.droplets);
    if (ImGui::DragInt("Droplets / iteration", &droplets, 100.0F, 0, 2000000))
    {
      p.erosion.droplets = static_cast<std::uint32_t>(std::max(droplets, 0));
    }
    ImGui::SliderInt("Lifetime", &p.erosion.max_lifetime, 4, 200);
    ImGui::SliderFloat("Inertia", &p.erosion.inertia, 0.0F, 1.0F);
    ImGui::DragFloat("Capacity", &p.erosion.capacity, 0.05F, 0.0F, 32.0F);
    ImGui::SliderFloat("Erosion", &p.erosion.erosion, 0.0F, 1.0F);
    ImGui::SliderFloat("Deposition", &p.erosion.deposition, 0.0F, 1.0F);
    ImGui::SliderFloat("Evaporation", &p.erosion.evaporation, 0.0F, 0.5F);
    ImGui::DragFloat("Gravity", &p.erosion.gravity, 0.05F, 0.0F, 32.0F);
    ImGui::SliderInt("Brush radius", &p.erosion.radius, 1, 6);
    ImGui::TreePop();
  }
  ImGui::DragFloat("Rainfall", &p.rainfall, 0.01F, 0.01F, 100.0F);
  ImGui::DragFloat("River threshold", &p.river_threshold, 1.0F, 1.0F, 100000.0F, "%.0f cells");
  ImGui::DragFloat("River depth", &p.river_depth, 0.001F, 0.0F, 1.0F, "%.3f");
  ImGui::DragFloat("Carve", &p.carve, 0.01F, 0.0F, 4.0F);
  enum_combo("Depressions", p.mode);
  if (p.mode == procgen::DepressionMode::kFillLakes)
  {
    ImGui::DragFloat("Lake min depth", &p.lake_min_depth, 0.0005F, 0.0F, 1.0F, "%.4f");
  }
  else
  {
    ImGui::DragFloat("Tunnel threshold", &p.tunnel_threshold, 0.005F, 0.0F, 10.0F, "%.3f");
    ImGui::TextDisabled("Breach cuts deeper than this become tunnels");
  }
  ImGui::Checkbox("Mark river/lake/tunnel in semantic layer", &g.watershed_semantics);
  seed_input(app);
  if (ImGui::Button("Run water-shedding", ImVec2{-1, 0}))
  {
    start_watershed(app);
  }
}

void wfc_section(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  ImGui::Text("Rules: %zu tiles, %zu adjacencies", lv.wfc.tiles.size(), lv.wfc.adjacency.size());
  ImGui::SameLine();
  if (ImGui::SmallButton("Edit rules"))
  {
    app.generator_tab_request = GeneratorTab::kWfcRules;
  }
  static std::size_t target = 0;
  if (app.layer < lv.layers.size() and lv.layers[app.layer].kind == LayerKind::kTiles)
  {
    target = app.layer;
  }
  if (!layer_combo("Target tile layer", lv, LayerKind::kTiles, target))
  {
    return;
  }
  if (ImGui::SmallButton("Learn rules from target layer"))
  {
    learn(lv.wfc, plane<TileGrid>(lv.layers[target]));
    app.modified = true;
  }
  ImGui::SeparatorText("Constraints from layers");
  ImGui::Checkbox("Height bands", &g.wfc_use_height);
  ImGui::SameLine();
  ImGui::Checkbox("Water", &g.wfc_use_water);
  ImGui::SameLine();
  ImGui::Checkbox("Semantics", &g.wfc_use_semantic);
  if (g.wfc_use_water)
  {
    ImGui::DragFloat("Wet above", &g.wfc_water_threshold, 0.0005F, 0.0F, 10.0F, "%.4f");
  }
  ImGui::Checkbox("Keep existing rule tiles", &g.wfc_keep_existing);
  ImGui::SliderInt("Attempts", &g.wfc_attempts, 1, 100);
  if (g.region_w > 0)
  {
    ImGui::Text("Region: %d,%d %dx%d", g.region_x, g.region_y, g.region_w, g.region_h);
    ImGui::SameLine();
    if (ImGui::SmallButton("Whole map"))
    {
      g.region_w = 0;
      g.region_h = 0;
    }
  }
  else
  {
    ImGui::TextDisabled("Region: whole map (drag with the Select tool to limit)");
  }
  seed_input(app);
  if (ImGui::Button("Generate tiles (WFC)", ImVec2{-1, 0}))
  {
    start_wfc(app, target);
  }
}

}  // namespace

void start_heightmap(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  const procgen::HeightmapParams& p = g.heightmap;
  if (g.height_layer >= lv.layers.size() or lv.layers[g.height_layer].kind != LayerKind::kHeight)
  {
    report(app, Status::error("no height layer selected"));
    return;
  }
  if (auto* job = prepare_job<HeightmapJob>(app); job != nullptr)
  {
    job->params = p;
    job->seed = lv.settings.seed;
    job->extents = lv.settings.extents;
    job->seed_image = (p.seed_weight > 0.0F) ? g.seed_image : Path{};
    start_job(app);
  }
}

void start_watershed(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  const procgen::WatershedParams& p = g.watershed;
  if (g.height_layer >= lv.layers.size() or lv.layers[g.height_layer].kind != LayerKind::kHeight)
  {
    report(app, Status::error("no height layer selected"));
    return;
  }
  // Make sure the output layers exist before the job copies them
  if (find_layer(lv, LayerKind::kWater) == lv.layers.size())
  {
    add_layer(lv, LayerKind::kWater, "water");
    app.undo.clear();
    invalidate_view(app);
  }
  if (g.watershed_semantics and find_layer(lv, LayerKind::kSemantic) == lv.layers.size())
  {
    add_layer(lv, LayerKind::kSemantic, "semantics");
    app.undo.clear();
    invalidate_view(app);
  }
  if (auto* job = prepare_job<WatershedJob>(app); job != nullptr)
  {
    memory::Arena& arena = job->memory->general;
    job->params = p;
    job->seed = lv.settings.seed;
    const auto& height = plane<ScalarGrid>(lv.layers[g.height_layer]);
    job->height = ScalarGrid{arena};
    job->height.extents = height.extents;
    job->height.values.assign(height.values.begin(), height.values.end());
    job->water = ScalarGrid{arena};
    job->water.assign(height.extents, 0.0F);
    job->semantics = g.watershed_semantics;
    job->semantic = IdGrid{arena};
    if (job->semantics)
    {
      const auto& sem = plane<IdGrid>(lv.layers[find_layer(lv, LayerKind::kSemantic)]);
      job->semantic.extents = sem.extents;
      job->semantic.values.assign(sem.values.begin(), sem.values.end());
      job->river_id = intern_semantic(lv, "river");
      job->lake_id = intern_semantic(lv, "lake");
      job->tunnel_id = intern_semantic(lv, "tunnel");
    }
    start_job(app);
  }
}

void start_wfc(App& app, std::size_t target)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  if (target >= lv.layers.size() or lv.layers[target].kind != LayerKind::kTiles)
  {
    report(app, Status::error("the WFC target must be a tile layer"));
    return;
  }
  if (auto* job = prepare_job<WfcJob>(app); job != nullptr)
  {
    job->level = app.level;
    job->layer = target;
    const auto& tiles = plane<TileGrid>(lv.layers[target]);
    job->tiles = TileGrid{job->memory->general};
    job->tiles.extents = tiles.extents;
    job->tiles.values.assign(tiles.values.begin(), tiles.values.end());
    job->params = procgen::WfcParams{
      .seed = lv.settings.seed,
      .max_attempts = g.wfc_attempts,
      .keep_existing = g.wfc_keep_existing,
      .region = procgen::Region{g.region_x, g.region_y, g.region_w, g.region_h}};
    procgen::WfcConstraints& c = job->constraints;
    if (const std::size_t h = find_layer(lv, LayerKind::kHeight); g.wfc_use_height and h < lv.layers.size())
    {
      const std::size_t height =
        (g.height_layer < lv.layers.size() and lv.layers[g.height_layer].kind == LayerKind::kHeight) ? g.height_layer
                                                                                                     : h;
      c.height = &plane<ScalarGrid>(lv.layers[height]);
      // Bands are relative to the layer's preview range (normalized heights)
      c.height_lo = lv.layers[height].range_min;
      c.height_hi = lv.layers[height].range_max;
    }
    if (const std::size_t w = find_layer(lv, LayerKind::kWater); g.wfc_use_water and w < lv.layers.size())
    {
      c.water = &plane<ScalarGrid>(lv.layers[w]);
      c.water_threshold = g.wfc_water_threshold;
    }
    if (const std::size_t s = find_layer(lv, LayerKind::kSemantic); g.wfc_use_semantic and s < lv.layers.size())
    {
      c.semantic = &plane<IdGrid>(lv.layers[s]);
    }
    start_job(app);
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
        "WFC rules: %zu neighbor pair(s) from a %d x %d palette layout", pairs, bounds.width, bounds.height);
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
    LVL_LOG_INFO() << "Batch constraints applied to " << edited << " WFC rule tiles";
    app.modified = true;
  }
}

/// The visual WFC rule editor (the "WFC rules" tab)
void wfc_rules_tab(App& app)
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
    app.panels.wfc_neighbors = true;
    ImGui::SetWindowFocus("WFC neighbors");
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
  palette_layout_section(app);
  if (rules.tiles.empty())
  {
    ImGui::TextDisabled(
      "No rule tiles. Select tiles in the palette and add them, or learn from a tile layer (Generate).");
    app.modified |= changed;
    return;
  }
  app.wfc_tile = std::min<WfcTileId>(app.wfc_tile, static_cast<WfcTileId>(rules.tiles.size() - 1));

  // Tile strip: pick the tile whose constraints/neighbors are edited (ctrl+click: batch selection)
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
  ImGui::SeparatorText("Allowed neighbors (click to toggle)");
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
    ImGui::PushID(static_cast<int>(d));
    ImGui::Text("%-10s", enum_name(d).data());
    ImGui::SameLine();
    if (ImGui::SmallButton("all"))
    {
      for (WfcTileId b = 0; b < rules.tiles.size(); ++b)
      {
        set_allowed(rules, a, b, d, true);
      }
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("none"))
    {
      for (WfcTileId b = 0; b < rules.tiles.size(); ++b)
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
        for (WfcTileId b = 0; b < rules.tiles.size(); ++b)
        {
          set_allowed(rules, a, b, other, allowed(rules, a, b, d));
        }
      }
      changed = true;
    }
    constexpr float kSmall = 24.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, static_cast<int>(avail / (kSmall + ImGui::GetStyle().ItemSpacing.x)));
    for (WfcTileId b = 0; b < rules.tiles.size(); ++b)
    {
      ImGui::PushID(b);
      if (b % per_row != 0)
      {
        ImGui::SameLine();
      }
      const bool on = allowed(rules, a, b, d);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      tile_image(app, rules.tiles[b].tile, kSmall);
      if (ImGui::IsItemHovered())
      {
        wfc_pair_tooltip(app, a, b, d, "Click to allow or forbid it there");
      }
      if (ImGui::IsItemClicked())
      {
        set_allowed(rules, a, b, d, !on);
        changed = true;
      }
      ImDrawList* draw = ImGui::GetWindowDrawList();
      if (on)
      {
        draw->AddRect(p, ImVec2{p.x + kSmall, p.y + kSmall}, IM_COL32(80, 230, 120, 255), 0, 0, 2.0F);
      }
      else
      {
        draw->AddRectFilled(p, ImVec2{p.x + kSmall, p.y + kSmall}, IM_COL32(0, 0, 0, 150));
      }
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  ImGui::TextDisabled("Rules are symmetric: allowing B east of A also allows A west of B.");
  app.modified |= changed;
}

void generate_panel(App& app)
{
  if (!ImGui::Begin("Generate", &app.panels.generate))
  {
    ImGui::End();
    return;
  }
  if (skip_while_busy(app))
  {
    return;
  }
  // One tab per generator; the WFC rule editor has its own tab next to WFC generation
  if (ImGui::BeginTabBar("generators", ImGuiTabBarFlags_FittingPolicyScroll))
  {
    static constexpr GeneratorTab kTabs[] = {
      GeneratorTab::kHeightmap, GeneratorTab::kWatershed, GeneratorTab::kWfc, GeneratorTab::kWfcRules};
    for (const GeneratorTab tab : kTabs)
    {
      const char* label = "";
      switch (tab)
      {
      case GeneratorTab::kHeightmap:
        label = "Heightmap";
        break;
      case GeneratorTab::kWatershed:
        label = "Water-shedding";
        break;
      case GeneratorTab::kWfc:
        label = "WFC";
        break;
      case GeneratorTab::kWfcRules:
        label = "WFC rules";
        break;
      case GeneratorTab::kNone:
        continue;
      }
      const bool select = (app.generator_tab_request == tab);
      if (!ImGui::BeginTabItem(label, nullptr, select ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None))
      {
        continue;
      }
      ImGui::BeginChild("tab");
      switch (tab)
      {
      case GeneratorTab::kHeightmap:
        heightmap_section(app);
        break;
      case GeneratorTab::kWatershed:
        watershed_section(app);
        break;
      case GeneratorTab::kWfc:
        wfc_section(app);
        break;
      case GeneratorTab::kWfcRules:
        wfc_rules_tab(app);
        break;
      case GeneratorTab::kNone:
        break;
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    app.generator_tab_request = GeneratorTab::kNone;
    ImGui::EndTabBar();
  }
  ImGui::End();
}

}  // namespace lvl::editor
