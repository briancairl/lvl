/**
 * @copyright 2026-present Brian Cairl
 *
 * @file tileset.cpp
 */

// C++ Standard Library
#include <algorithm>

// LVL
#include "lvl/level/tileset.hpp"

namespace lvl::level
{

Extents slice_grid(const SliceParams& p, int image_width, int image_height)
{
  if (p.tile_width <= 0 or p.tile_height <= 0 or image_width <= 0 or image_height <= 0)
  {
    return Extents{0, 0};
  }
  const auto fit = [](int size, int margin, int tile, int spacing) {
    const int usable = size - margin;
    return (usable < tile) ? 0 : 1 + (usable - tile) / (tile + spacing);
  };
  int columns = fit(image_width, p.margin_x, p.tile_width, p.spacing_x);
  int rows = fit(image_height, p.margin_y, p.tile_height, p.spacing_y);
  if (p.columns > 0)
  {
    columns = std::min(columns, p.columns);
  }
  if (p.rows > 0)
  {
    rows = std::min(rows, p.rows);
  }
  // Tile indices are 16 bits
  while (columns * rows > 0xFFFF and rows > 0)
  {
    --rows;
  }
  return Extents{columns, rows};
}

TileRect tile_rect(const SliceParams& p, int image_width, int image_height, TileIndex index)
{
  const Extents grid = slice_grid(p, image_width, image_height);
  if (grid.width == 0)
  {
    return TileRect{};
  }
  const int col = static_cast<int>(index) % grid.width;
  const int row = static_cast<int>(index) / grid.width;
  return TileRect{
    p.margin_x + col * (p.tile_width + p.spacing_x),
    p.margin_y + row * (p.tile_height + p.spacing_y),
    p.tile_width,
    p.tile_height};
}

void reslice(TileSet& tileset)
{
  const Extents grid = slice_grid(tileset.slice, tileset.image_width, tileset.image_height);
  tileset.tiles.resize(grid.count());
  const auto count = static_cast<TileIndex>(grid.count());
  std::erase_if(tileset.properties, [count](const TileProperty& p) { return p.tile >= count; });
  std::erase_if(
    tileset.seasonal, [count](const SeasonalVariant& v) { return v.tile >= count or v.alternate >= count; });
  // A group keeps its shape across a re-slice: a cell naming a tile the atlas no longer has becomes
  // a hole, rather than the whole group going away. Re-slicing is how a tile size is corrected, and
  // the correction usually brings the tiles back
  for (TileIndex& cell : tileset.group_cells)
  {
    cell = (cell >= count) ? kNoTileIndex : cell;
  }
}

GroupId find_group(const TileSet& tileset, std::string_view name)
{
  for (std::size_t i = 0; i < tileset.groups.size(); ++i)
  {
    if (tileset.groups[i].name == name)
    {
      return static_cast<GroupId>(i);
    }
  }
  return kNoGroup;
}

const TileIndex* group_cells_of(const TileSet& tileset, const TileGroup& group)
{
  const std::size_t count = group.size.count();
  if (count == 0 or group.first + count > tileset.group_cells.size())
  {
    return nullptr;
  }
  return tileset.group_cells.data() + group.first;
}

TileIndex group_cell(const TileSet& tileset, const TileGroup& group, int x, int y)
{
  const TileIndex* cells = group_cells_of(tileset, group);
  if (cells == nullptr or !group.size.contains(x, y))
  {
    return kNoTileIndex;
  }
  return cells[group.size.index(x, y)];
}

GroupId set_group(TileSet& tileset, std::string_view name, Extents size, const TileIndex* cells)
{
  if (
    name.empty() or cells == nullptr or size.width <= 0 or size.height <= 0 or size.width > kMaxGroupExtent or
    size.height > kMaxGroupExtent)
  {
    return kNoGroup;
  }
  const auto tiles = static_cast<TileIndex>(std::min<std::size_t>(tileset.tiles.size(), kNoTileIndex));
  const std::size_t count = size.count();
  const bool any = std::any_of(cells, cells + count, [tiles](TileIndex t) { return t < tiles; });
  if (!any)
  {
    return kNoGroup;
  }
  // Replacing a group of the same name removes it first, so a footprint that changed size does not
  // leave its old cells stranded in the table
  if (const GroupId existing = find_group(tileset, name); existing != kNoGroup)
  {
    remove_group(tileset, existing);
  }
  if (tileset.groups.size() >= kNoGroup)
  {
    return kNoGroup;
  }
  TileGroup group;
  group.name = name;
  group.size = size;
  group.first = static_cast<std::uint32_t>(tileset.group_cells.size());
  for (std::size_t i = 0; i < count; ++i)
  {
    tileset.group_cells.push_back((cells[i] < tiles) ? cells[i] : kNoTileIndex);
  }
  tileset.groups.push_back(group);
  return static_cast<GroupId>(tileset.groups.size() - 1);
}

std::size_t validate_groups(TileSet& tileset)
{
  const auto tiles = static_cast<TileIndex>(std::min<std::size_t>(tileset.tiles.size(), kNoTileIndex));
  for (TileIndex& cell : tileset.group_cells)
  {
    cell = (cell >= tiles) ? kNoTileIndex : cell;
  }
  // The table is always written packed and in the order the groups are listed, so anything else is
  // a file that cannot say which cells belong to which group -- and a group reading another's cells
  // is worse than a tileset with no groups, so the lot goes
  std::size_t next = 0;
  bool packed = true;
  for (const TileGroup& group : tileset.groups)
  {
    const std::size_t count = group.size.count();
    packed = packed and count > 0 and group.size.width <= kMaxGroupExtent and group.size.height <= kMaxGroupExtent and
      group.first == next;
    next += count;
  }
  if (packed and next == tileset.group_cells.size())
  {
    return 0;
  }
  const std::size_t dropped = tileset.groups.size();
  tileset.groups.clear();
  tileset.group_cells.clear();
  return dropped;
}

void remove_group(TileSet& tileset, GroupId index)
{
  if (index >= tileset.groups.size())
  {
    return;
  }
  const TileGroup group = tileset.groups[index];
  const std::size_t count = group.size.count();
  const std::size_t first = group.first;
  const std::size_t last = std::min(first + count, tileset.group_cells.size());
  tileset.group_cells.erase(
    tileset.group_cells.begin() + static_cast<std::ptrdiff_t>(std::min(first, tileset.group_cells.size())),
    tileset.group_cells.begin() + static_cast<std::ptrdiff_t>(last));
  tileset.groups.erase(tileset.groups.begin() + static_cast<std::ptrdiff_t>(index));
  const auto removed = static_cast<std::uint32_t>(last - std::min(first, last));
  for (TileGroup& g : tileset.groups)
  {
    g.first -= (g.first > first) ? removed : 0;
  }
}

namespace
{

const SeasonalVariant* find_variant(const TileSet& tileset, TileIndex tile, std::string_view season)
{
  for (const auto& v : tileset.seasonal)
  {
    if (v.tile == tile and v.season == season)
    {
      return &v;
    }
  }
  return nullptr;
}

}  // namespace

TileIndex seasonal_tile(const TileSet& tileset, TileIndex tile, std::string_view season)
{
  const auto* v = find_variant(tileset, tile, season);
  return (v == nullptr) ? tile : v->alternate;
}

MaterialId seasonal_material(const TileSet& tileset, TileIndex tile, std::string_view season)
{
  const MaterialId base = (tile < tileset.tiles.size()) ? tileset.tiles[tile].material : kNoMaterial;
  const auto* v = find_variant(tileset, tile, season);
  return (v == nullptr or v->material == kNoMaterial) ? base : v->material;
}

void resolve_season(const TileSet& tileset, std::string_view season, memory::Vector<TileIndex>& out)
{
  out.resize(tileset.tiles.size());
  for (std::size_t i = 0; i < out.size(); ++i)
  {
    out[i] = static_cast<TileIndex>(i);
  }
  for (const auto& v : tileset.seasonal)
  {
    if (v.season == season and v.tile < out.size())
    {
      out[v.tile] = v.alternate;
    }
  }
}

void set_seasonal_variant(TileSet& tileset, const SeasonalVariant& variant)
{
  std::erase_if(tileset.seasonal, [&variant](const SeasonalVariant& v) {
    return v.tile == variant.tile and v.season == variant.season;
  });
  if (variant.alternate != variant.tile or variant.material != kNoMaterial)
  {
    tileset.seasonal.push_back(variant);
  }
}

const Name* find_property(const TileSet& tileset, TileIndex tile, std::string_view key)
{
  for (const auto& p : tileset.properties)
  {
    if (p.tile == tile and p.key == key)
    {
      return &p.value;
    }
  }
  return nullptr;
}

void set_property(TileSet& tileset, TileIndex tile, std::string_view key, std::string_view value)
{
  for (auto& p : tileset.properties)
  {
    if (p.tile == tile and p.key == key)
    {
      p.value.assign(value);
      return;
    }
  }
  tileset.properties.push_back(TileProperty{.tile = tile, .key = Name{key}, .value = Name{value}});
}

void remove_property(TileSet& tileset, TileIndex tile, std::string_view key)
{
  std::erase_if(tileset.properties, [tile, key](const TileProperty& p) { return p.tile == tile and p.key == key; });
}

std::size_t apply_edit(TileSet& tileset, const TileEdit& edit, const TileIndex* tiles, std::size_t count)
{
  std::size_t edited = 0;
  for (std::size_t i = 0; i < count; ++i)
  {
    const TileIndex tile = tiles[i];
    if (tile >= tileset.tiles.size())
    {
      continue;
    }
    TileInfo& info = tileset.tiles[tile];
    info.semantic = edit.set_semantic ? edit.semantic : info.semantic;
    info.material = edit.set_material ? edit.material : info.material;
    info.collision = edit.set_collision ? edit.collision : info.collision;
    info.hidden = edit.set_hidden ? edit.hidden : info.hidden;
    info.scatter_weight = edit.set_scatter_weight ? edit.scatter_weight : info.scatter_weight;
    if (edit.set_property and !edit.property_key.empty())
    {
      set_property(tileset, tile, edit.property_key.view(), edit.property_value.view());
    }
    if (edit.clear_property and !edit.property_key.empty())
    {
      remove_property(tileset, tile, edit.property_key.view());
    }
    ++edited;
  }
  return edited;
}

bool load(json::Reader& r, const Json& j, SliceParams& p)
{
  r.object(j, "slice");
  r.get(j, "tile_width", p.tile_width);
  r.get(j, "tile_height", p.tile_height);
  r.get_optional(j, "margin_x", p.margin_x);
  r.get_optional(j, "margin_y", p.margin_y);
  r.get_optional(j, "spacing_x", p.spacing_x);
  r.get_optional(j, "spacing_y", p.spacing_y);
  r.get_optional(j, "columns", p.columns);
  r.get_optional(j, "rows", p.rows);
  if (r.ok() and (p.tile_width <= 0 or p.tile_height <= 0))
  {
    return r.fail("tile size must be positive");
  }
  return r.ok();
}

void save(Json& j, const SliceParams& p)
{
  j = Json::object();
  json::write(j, "tile_width", p.tile_width);
  json::write(j, "tile_height", p.tile_height);
  json::write(j, "margin_x", p.margin_x);
  json::write(j, "margin_y", p.margin_y);
  json::write(j, "spacing_x", p.spacing_x);
  json::write(j, "spacing_y", p.spacing_y);
  json::write(j, "columns", p.columns);
  json::write(j, "rows", p.rows);
}

}  // namespace lvl::level
