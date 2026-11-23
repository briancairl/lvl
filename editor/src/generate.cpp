/**
 * @copyright 2026-present Brian Cairl
 *
 * @file generate.cpp
 *
 * Generators (heightmap from layered noise / seed images, iterative water-shedding, WFC), as tabs of
 * the Generate panel. The tile adjacency rules the WFC generator reads have their own window (see
 * adjacency.cpp).
 */

// C++ Standard Library
#include <algorithm>

// Dear ImGui
#include <imgui.h>

// LVL
#include "internal.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/random.hpp"

namespace lvl::editor
{
namespace
{

using namespace level;

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
  if (ImGui::SmallButton("Edit rules..."))
  {
    app.panels.adjacency = true;
    ImGui::SetWindowFocus("Tile adjacency rules");
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Opens the Tile adjacency rules window");
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

/// Combo over every scalar layer (a heightmap, or water depth); returns false if there are none
bool scalar_layer_combo(const char* label, const Level& lv, std::size_t& index)
{
  std::size_t first = lv.layers.size();
  for (std::size_t i = 0; i < lv.layers.size(); ++i)
  {
    first = (first == lv.layers.size() and is_scalar(lv.layers[i].kind)) ? i : first;
  }
  if (first == lv.layers.size())
  {
    ImGui::TextDisabled("(no height or water layer)");
    return false;
  }
  if (index >= lv.layers.size() or !is_scalar(lv.layers[index].kind))
  {
    index = first;
  }
  if (ImGui::BeginCombo(label, lv.layers[index].name.c_str()))
  {
    for (std::size_t i = 0; i < lv.layers.size(); ++i)
    {
      if (
        is_scalar(lv.layers[i].kind) and
        ImGui::Selectable(format<96>("%s##%zu", lv.layers[i].name.c_str(), i), i == index))
      {
        index = i;
      }
    }
    ImGui::EndCombo();
  }
  return true;
}

/// Combo over the semantic catalog
void semantic_combo(const char* label, const Level& lv, int& id)
{
  id = std::clamp(id, 0, static_cast<int>(lv.semantics.size()) - 1);
  if (ImGui::BeginCombo(label, lv.semantics[static_cast<std::size_t>(id)].name.c_str()))
  {
    for (std::size_t i = 0; i < lv.semantics.size(); ++i)
    {
      if (ImGui::Selectable(format<96>("%s##%zu", lv.semantics[i].name.c_str(), i), static_cast<int>(i) == id))
      {
        id = static_cast<int>(i);
      }
    }
    ImGui::EndCombo();
  }
}

/**
 * @brief The tiles a fill writes: the palette selection, or else the tile being painted
 *
 * The selection is what the fill scatters, exactly as it is what the paint tool stamps, so the
 * same handful of grass variants serves both.
 */
void fill_tiles(const App& app, memory::Arena& arena, memory::Vector<TileRef>& tiles, memory::Vector<float>& weights)
{
  const TileSelection& selection = app.selection;
  if (selection.count > 0 and selection.tileset < app.level->tilesets.size())
  {
    const auto id = static_cast<TileSetId>(selection.tileset);
    const TileSet& ts = app.level->tilesets[selection.tileset];
    for (const TileIndex tile : selected_tiles(app, arena))
    {
      tiles.push_back(TileRef{id, tile});
      weights.push_back(tile < ts.tiles.size() ? ts.tiles[tile].scatter_weight : 1.0F);
    }
    if (!app.generators.fill_scatter and tiles.size() > 1)
    {
      tiles.resize(1);
      weights.resize(1);
    }
  }
  else if (!app.paint.tile.empty())
  {
    tiles.push_back(app.paint.tile);
    weights.push_back(1.0F);
  }
}

/**
 * @brief The weight rows of the Fill tab: each tile, its share of the scatter, and its weight
 *
 * The weights live on the tiles (<code>TileInfo::scatter_weight</code>), so they are the ones the
 * Tile metadata panel shows and they travel in the tileset; this is only where they are reachable
 * while a fill is being set up. The share is what the weights actually come to, which is the
 * number being chosen.
 */
void fill_weights_section(App& app, const memory::Vector<TileRef>& tiles, const memory::Vector<float>& weights)
{
  TileSet& ts = app.level->tilesets[app.selection.tileset];
  float total = 0.0F;
  for (const float w : weights)
  {
    total += std::max(w, 0.0F);
  }
  const float rows = std::min(static_cast<float>(tiles.size()), 6.0F);
  ImGui::BeginChild(
    "weights", ImVec2{0.0F, rows * (ImGui::GetFrameHeightWithSpacing() + 2.0F)}, ImGuiChildFlags_Border);
  for (std::size_t i = 0; i < tiles.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    tile_image(app, tiles[i], ImGui::GetFrameHeight());
    ImGui::SameLine();
    const float even = 100.0F / static_cast<float>(tiles.size());
    const float share = (total > 0.0F) ? (std::max(weights[i], 0.0F) / total * 100.0F) : even;
    ImGui::TextDisabled("%5.1f%%", static_cast<double>(share));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    float weight = weights[i];
    if (ImGui::DragFloat("##weight", &weight, 0.05F, 0.0F, 1000.0F, "%.2f") and tiles[i].tile < ts.tiles.size())
    {
      ts.tiles[tiles[i].tile].scatter_weight = std::max(weight, 0.0F);
      app.modified = true;
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (total <= 0.0F)
  {
    ImGui::TextDisabled("Every weight is zero, so the scatter is even.");
  }
}

void fill_section(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  if (app.layer < lv.layers.size() and lv.layers[app.layer].kind == LayerKind::kTiles)
  {
    g.fill_layer = app.layer;
  }
  if (!layer_combo("Target tile layer", lv, LayerKind::kTiles, g.fill_layer))
  {
    return;
  }

  ImGui::SeparatorText("Tiles");
  memory::Scope scope{app.memory.frame};
  auto tiles = memory::make_vector<TileRef>(app.memory.frame);
  auto weights = memory::make_vector<float>(app.memory.frame);
  fill_tiles(app, app.memory.frame, tiles, weights);
  if (app.selection.count > 1)
  {
    ImGui::Checkbox("Scatter the selection", &g.fill_scatter);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("On: every filled cell takes one of the selected tiles, by its position -- so variants of a "
                        "ground cover scatter and stay put. Off: every cell takes the first selected tile.");
    }
  }
  if (tiles.empty())
  {
    ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "No tile to fill with");
    ImGui::TextDisabled("Pick one in the palette, or ctrl+click several to scatter them.");
  }
  else if (tiles.size() == 1)
  {
    ImGui::Text("Fills with 1 tile");
  }
  else
  {
    ImGui::Text("Scatters %zu tiles", tiles.size());
  }
  if (app.selection.count > 0)
  {
    ImGui::SameLine();
    deselect_all_button(app);
  }
  if (tiles.size() > 1)
  {
    fill_weights_section(app, tiles, weights);
  }
  else if (app.selection.count > 0)
  {
    selection_preview(app, 24.0F, 2);
  }

  ImGui::SeparatorText("Where");
  ImGui::Checkbox("Height band", &g.fill_use_band);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Fill only where a scalar layer's value falls inside the band");
  }
  if (g.fill_use_band and scalar_layer_combo("Band layer", lv, g.fill_band_layer))
  {
    // The layer's own range, so the numbers mean something whatever the heightmap holds
    const Layer& band = lv.layers[g.fill_band_layer];
    const float lo = std::min(band.range_min, band.range_max);
    const float hi = std::max(band.range_min, band.range_max);
    const float speed = std::max((hi - lo) / 400.0F, 1e-4F);
    ImGui::DragFloatRange2("Low / high", &g.fill_lo, &g.fill_hi, speed, lo, hi, "%.3f", "%.3f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Whole range"))
    {
      g.fill_lo = lo;
      g.fill_hi = hi;
    }
  }
  ImGui::Checkbox("Ground type", &g.fill_use_id);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Fill only where a semantic layer holds one ground type");
  }
  if (g.fill_use_id and layer_combo("Semantic layer", lv, LayerKind::kSemantic, g.fill_id_layer))
  {
    semantic_combo("Ground type", lv, g.fill_id);
  }
  ImGui::Checkbox("Only empty cells", &g.fill_only_empty);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Leave every cell that already has a tile, so what is drawn survives the fill");
  }
  ImGui::Checkbox("Honour the selected region", &g.fill_region);
  ImGui::SameLine();
  if (g.region_w > 0 and g.fill_region)
  {
    ImGui::Text("(%d,%d %dx%d)", g.region_x, g.region_y, g.region_w, g.region_h);
  }
  else
  {
    ImGui::TextDisabled("(whole map; drag with the Select tool to limit)");
  }

  seed_input(app);
  if (ImGui::Button("Fill", ImVec2{-1, 0}) and !tiles.empty())
  {
    apply_fill(app, g.fill_layer, tiles.data(), weights.data(), tiles.size());
  }
  const procgen::FillResult& result = g.fill_result;
  if (result.matched > 0 or result.changed > 0)
  {
    ImGui::TextDisabled("Last fill: %zu cells matched, %zu changed", result.matched, result.changed);
  }
}

/**
 * @brief Generate > Height steps: the heightmap cut into levels, and the cliffs between them drawn
 *
 * Two passes, run by two buttons, because the first is the one that matters: the levels are what
 * the game is given, and the tiles are this editor showing what it will draw from them. Recutting
 * the levels redraws the tiles too unless that is turned off, since leaving the two out of step is
 * never what is wanted.
 */
void steps_section(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  if (lv.steps.terrains.empty())
  {
    ImGui::TextDisabled("No step terrains yet.");
    if (ImGui::SmallButton("Edit rules..."))
    {
      app.panels.adjacency = true;
      app.rules_mode = RulesMode::kSteps;
      ImGui::SetWindowFocus("Tile adjacency rules");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("opens the Height steps tab");
    return;
  }
  ImGui::Text("%zu step terrain(s)", lv.steps.terrains.size());
  ImGui::SameLine();
  if (ImGui::SmallButton("Edit rules..."))
  {
    app.panels.adjacency = true;
    app.rules_mode = RulesMode::kSteps;
    ImGui::SetWindowFocus("Tile adjacency rules");
  }

  ImGui::SeparatorText("Levels");
  const bool has_height = layer_combo("Heightmap", lv, LayerKind::kHeight, g.step_height_layer);
  const bool has_levels = layer_combo("Level layer", lv, LayerKind::kLevel, g.step_level_layer);
  if (!has_levels)
  {
    if (ImGui::Button("Add a level layer", ImVec2{-1, 0}))
    {
      g.step_level_layer = add_layer(lv, LayerKind::kLevel, "levels");
      app.modified = true;
    }
    return;
  }
  if (!has_height)
  {
    ImGui::TextDisabled("Without a heightmap the levels can only be drawn by hand.");
  }
  int mode = static_cast<int>(g.terrace.mode);
  ImGui::SetNextItemWidth(160.0F);
  if (ImGui::Combo("Cut by", &mode, "Number of levels\0Height per level\0"))
  {
    g.terrace.mode = static_cast<procgen::TerraceMode>(mode);
  }
  if (g.terrace.mode == procgen::TerraceMode::kCount)
  {
    ImGui::SetNextItemWidth(160.0F);
    ImGui::SliderInt("Levels", &g.terrace.levels, 1, 32);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("The height range is cut into this many bands of equal height");
    }
  }
  else
  {
    ImGui::SetNextItemWidth(160.0F);
    ImGui::DragFloat("Height per level", &g.terrace.rate, 0.002F, 0.001F, 100.0F, "%.3f");
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Each band is this tall, so a steeper map simply gets more of them");
    }
  }
  ImGui::SetNextItemWidth(160.0F);
  ImGui::DragInt("Level ceiling", &g.terrace.max_level, 1.0F, 1, 255);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Levels are held down to this, so one stray peak cannot run the count up");
  }
  ImGui::Checkbox("Tidy the levels", &g.step_tidy);
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Squares the terraces off so every one of them is a shape the step tiles can draw.\n"
                      "Without it a terraced heightmap is full of one-cell spurs and 45-degree edges,\n"
                      "and gft's table has no tile for any of those -- they come out flat.");
  }
  if (g.step_tidy)
  {
    ImGui::Indent();
    ImGui::SetNextItemWidth(160.0F);
    ImGui::SliderInt("Narrowest feature", &g.tidy.min_feature, 1, 6);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("Terraces and holes narrower than this are smoothed away. 1 leaves the smoothing off "
                        "and only squares off what has no tile.");
    }
    ImGui::Checkbox("Shave spurs", &g.tidy.remove_spurs);
    ImGui::SameLine();
    ImGui::Checkbox("Fill notches", &g.tidy.fill_notches);
    ImGui::SameLine();
    ImGui::Checkbox("Square off", &g.tidy.square_corners);
    if (ImGui::IsItemHovered())
    {
      ImGui::SetTooltip("The one that matters: it grows a terrace until every cell of it is a shape the table "
                        "has a tile for. Turning it off will leave cells flat that should be cliffs.");
    }
    ImGui::Unindent();
  }
  ImGui::Checkbox("Redraw the tiles too", &g.step_tile_after_terrace);
  ImGui::BeginDisabled(!has_height);
  if (ImGui::Button("Cut the heightmap into levels", ImVec2{-1, 0}))
  {
    apply_terrace(app);
  }
  ImGui::EndDisabled();
  if (g.terrace_result.rate > 0.0F)
  {
    ImGui::TextDisabled(
      "Last cut: %d level(s), %.4f tall, %zu cell(s) moved",
      g.terrace_result.top + 1,
      static_cast<double>(g.terrace_result.rate),
      g.terrace_result.changed);
    if (g.step_tidy)
    {
      ImGui::TextDisabled("Tidying moved %zu more over %d round(s)", g.tidy_result.changed, g.tidy_result.passes);
      if (!g.tidy_result.stable)
      {
        ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "The tidy did not settle; raise the rounds");
      }
    }
  }

  ImGui::SeparatorText("Tiles and collision");
  if (app.layer < lv.layers.size() and lv.layers[app.layer].kind == LayerKind::kTiles)
  {
    g.step_tile_layer = app.layer;
  }
  if (!layer_combo("Target tile layer", lv, LayerKind::kTiles, g.step_tile_layer))
  {
    return;
  }
  ImGui::Checkbox("Keep tiles at levels no terrain covers", &g.step_keep_untermed);
  int collision = static_cast<int>(g.step_collision);
  ImGui::SetNextItemWidth(220.0F);
  if (ImGui::Combo("Block steps", &collision, "Don't touch collision\0Below the step\0Above the step\0"))
  {
    g.step_collision = static_cast<procgen::StepCollision>(collision);
  }
  if (ImGui::IsItemHovered())
  {
    ImGui::SetTooltip("Marks the cells on one side of every cliff solid, so it cannot be walked through.\n"
                      "Below: the foot of the cliff, which leaves the ground on top walkable.\n"
                      "Above: its edge. Either way the mark is cleared again where the cliff has gone.");
  }
  bool has_collision = true;
  if (g.step_collision != procgen::StepCollision::kNone)
  {
    has_collision = layer_combo("Collision layer", lv, LayerKind::kCollision, g.step_collision_layer);
  }
  ImGui::BeginDisabled(!has_collision);
  if (ImGui::Button("Draw the steps", ImVec2{-1, 0}))
  {
    apply_steps(app);
  }
  ImGui::EndDisabled();
  const procgen::StepFillResult& r = g.step_result;
  if (r.tiled > 0 or r.untermed > 0 or r.missing > 0)
  {
    ImGui::TextDisabled("Last draw: %zu tiled, %zu changed, %zu blocked", r.tiled, r.changed, r.blocked);
    if (r.untermed > 0)
    {
      ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "%zu cell(s) at a level no terrain covers", r.untermed);
    }
    if (r.missing > 0)
    {
      ImGui::TextColored(
        ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "%zu cell(s) whose terrain has no tile for the shape", r.missing);
    }
    if (r.unexpressed > 0)
    {
      ImGui::TextColored(
        ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "%zu cell(s) drawn flat: no tile exists for their shape", r.unexpressed);
      if (ImGui::IsItemHovered())
      {
        ImGui::SetTooltip("One-cell spurs or 45-degree edges in the levels. Tidy the levels to square them off.");
      }
    }
  }
}

}  // namespace

void apply_terrace(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  if (g.step_height_layer >= lv.layers.size() or g.step_level_layer >= lv.layers.size())
  {
    return;
  }
  app.undo.snapshot(lv, g.step_level_layer, "Cut levels");
  auto& levels = plane<IdGrid>(lv.layers[g.step_level_layer]);
  g.terrace_result = procgen::terrace(plane<ScalarGrid>(lv.layers[g.step_height_layer]), g.terrace, levels);
  g.tidy_result = procgen::TidyResult{};
  if (g.step_tidy)
  {
    g.tidy_result = procgen::tidy_levels(levels, g.tidy, app.memory.frame);
    LVL_LOG_INFO() << "Tidy: " << g.tidy_result.changed << " cell(s) moved over " << g.tidy_result.passes << " round(s)"
                   << (g.tidy_result.stable ? "" : " (not settled)");
  }
  invalidate_layer(app, g.step_level_layer);
  app.modified = app.modified or (g.terrace_result.changed > 0);
  LVL_LOG_INFO() << "Terrace: " << (g.terrace_result.top + 1) << " level(s), " << g.terrace_result.changed
                 << " cell(s) moved";
  if (g.step_tile_after_terrace)
  {
    apply_steps(app);
  }
}

void apply_steps(App& app)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  if (g.step_level_layer >= lv.layers.size() or g.step_tile_layer >= lv.layers.size())
  {
    return;
  }
  const bool block = (g.step_collision != procgen::StepCollision::kNone) and
    (g.step_collision_layer < lv.layers.size()) and (lv.layers[g.step_collision_layer].kind == LayerKind::kCollision);
  app.undo.snapshot(lv, g.step_tile_layer, "Draw steps");
  g.step_result = procgen::tile_steps(
    lv.steps,
    plane<IdGrid>(lv.layers[g.step_level_layer]),
    procgen::StepFillParams{
      .collision = block ? g.step_collision : procgen::StepCollision::kNone, .keep_untermed = g.step_keep_untermed},
    plane<TileGrid>(lv.layers[g.step_tile_layer]),
    block ? &plane<MaskGrid>(lv.layers[g.step_collision_layer]) : nullptr);
  invalidate_layer(app, g.step_tile_layer);
  if (block)
  {
    invalidate_layer(app, g.step_collision_layer);
  }
  app.modified = app.modified or (g.step_result.changed > 0) or (g.step_result.blocked > 0);
  LVL_LOG_INFO() << "Steps: " << g.step_result.tiled << " tiled, " << g.step_result.changed << " changed, "
                 << g.step_result.blocked << " blocked";
}

void apply_fill(App& app, std::size_t layer, const level::TileRef* tiles, const float* weights, std::size_t count)
{
  Level& lv = *app.level;
  Generators& g = app.generators;
  LVL_ASSERT_LT(layer, lv.layers.size());
  procgen::FillMask mask;
  if (g.fill_use_band and g.fill_band_layer < lv.layers.size() and is_scalar(lv.layers[g.fill_band_layer].kind))
  {
    mask.band = &plane<ScalarGrid>(lv.layers[g.fill_band_layer]);
    mask.lo = std::min(g.fill_lo, g.fill_hi);
    mask.hi = std::max(g.fill_lo, g.fill_hi);
  }
  if (g.fill_use_id and g.fill_id_layer < lv.layers.size() and lv.layers[g.fill_id_layer].kind == LayerKind::kSemantic)
  {
    mask.ids = &plane<IdGrid>(lv.layers[g.fill_id_layer]);
    mask.id = static_cast<std::uint16_t>(std::max(g.fill_id, 0));
  }
  if (g.fill_region and g.region_w > 0 and g.region_h > 0)
  {
    mask.region = procgen::Region{.x = g.region_x, .y = g.region_y, .width = g.region_w, .height = g.region_h};
  }
  mask.only_empty = g.fill_only_empty;
  app.undo.snapshot(lv, layer, "Fill");
  g.fill_result =
    procgen::fill_masked(tiles, weights, count, mask, lv.settings.seed, plane<TileGrid>(lv.layers[layer]));
  if (g.fill_result.changed > 0)
  {
    app.modified = true;
  }
  invalidate_layer(app, layer);
  LVL_LOG_INFO() << "Fill: " << g.fill_result.matched << " cell(s) matched, " << g.fill_result.changed << " changed";
}

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
  // One tab per generator
  if (ImGui::BeginTabBar("generators", ImGuiTabBarFlags_FittingPolicyScroll))
  {
    if (ImGui::BeginTabItem("Heightmap"))
    {
      ImGui::BeginChild("tab");
      heightmap_section(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Water-shedding"))
    {
      ImGui::BeginChild("tab");
      watershed_section(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("WFC"))
    {
      ImGui::BeginChild("tab");
      wfc_section(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Fill"))
    {
      ImGui::BeginChild("tab");
      fill_section(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Height steps"))
    {
      ImGui::BeginChild("tab");
      steps_section(app);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

}  // namespace lvl::editor
