/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io_images.cpp
 *
 * Layer cells as PNG files beside the level file, for levels whose <code>Settings::layer_images</code>
 * is set: one image per layer, one pixel per cell, in a folder named after the level file, which the
 * level file references instead of holding the cells itself. Both formats use this, so a layer looks
 * the same whether the document around it is JSON or binary.
 *
 * The pixel holds the cell's bytes, low byte first, so the image is the plane exactly -- PNG is
 * lossless and no layer kind's cell is wider than four bytes:
 *
 * | kind                | red           | green          | blue       | alpha       |
 * |---------------------|---------------|----------------|------------|-------------|
 * | tiles               | tileset, low  | tileset, high  | tile, low  | tile, high  |
 * | semantic, material  | id, low       | id, high       | 0          | opaque      |
 * | height, water       | the float's bit pattern, low byte first                   |
 * | collision           | mask          | 0              | 0          | opaque      |
 *
 * So an empty tile cell (tileset 0xFFFF) is a transparent pixel, and an id or mask layer reads as
 * shades of red over black -- enough to recognize a layer at a glance, and exact either way.
 */

// C++ Standard Library
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

// LVL
#include "io_common.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/image.hpp"
#include "lvl/core/jobs.hpp"

namespace lvl::level
{
namespace
{

/// The folder <code>path</code> sits in, without its trailing separator ("" when it names none)
std::string_view folder_of(std::string_view path)
{
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos)
  {
    return {};
  }
  // "/foo" sits in the root, not in ""
  return (slash == 0) ? path.substr(0, 1) : path.substr(0, slash);
}

/// <code>folder</code> and <code>name</code> with a separator between them
Path joined(std::string_view folder, std::string_view name)
{
  if (folder.empty())
  {
    return Path{name};
  }
  const char* const separator = (folder.back() == '/') ? "" : "/";
  return Path{format<Path::capacity()>(
    "%.*s%s%.*s",
    static_cast<int>(folder.size()),
    folder.data(),
    separator,
    static_cast<int>(name.size()),
    name.data())};
}

/// The file a reference read from the level file at <code>path</code> names
Path resolved(const char* path, std::string_view reference)
{
  if (reference.empty() or reference.front() == '/')
  {
    return Path{reference};
  }
  return joined(folder_of(std::string_view{path}), reference);
}

constexpr std::uint8_t low(std::uint16_t v) { return static_cast<std::uint8_t>(v & 0xFFU); }
constexpr std::uint8_t high(std::uint16_t v) { return static_cast<std::uint8_t>(v >> 8U); }
constexpr std::uint16_t word(std::uint8_t lo, std::uint8_t hi)
{
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(hi) << 8U | lo);
}

// One cell <-> one pixel (see the file comment); the byte order is written out rather than memcpy'd,
// so the files mean the same thing on a big-endian host

image::Rgba to_pixel(TileRef t) { return image::Rgba{low(t.tileset), high(t.tileset), low(t.tile), high(t.tile)}; }

image::Rgba to_pixel(std::uint16_t v) { return image::Rgba{low(v), high(v), 0, 255}; }

image::Rgba to_pixel(std::uint8_t v) { return image::Rgba{v, 0, 0, 255}; }

image::Rgba to_pixel(float v)
{
  const auto bits = std::bit_cast<std::uint32_t>(v);
  return image::Rgba{
    static_cast<std::uint8_t>(bits & 0xFFU),
    static_cast<std::uint8_t>((bits >> 8U) & 0xFFU),
    static_cast<std::uint8_t>((bits >> 16U) & 0xFFU),
    static_cast<std::uint8_t>((bits >> 24U) & 0xFFU)};
}

void from_pixel(image::Rgba p, TileRef& t) { t = TileRef{word(p.r, p.g), word(p.b, p.a)}; }

void from_pixel(image::Rgba p, std::uint16_t& v) { v = word(p.r, p.g); }

void from_pixel(image::Rgba p, std::uint8_t& v) { v = p.r; }

void from_pixel(image::Rgba p, float& v)
{
  const auto bits = static_cast<std::uint32_t>(p.r) | static_cast<std::uint32_t>(p.g) << 8U |
    static_cast<std::uint32_t>(p.b) << 16U | static_cast<std::uint32_t>(p.a) << 24U;
  v = std::bit_cast<float>(bits);
}

void pack(const Layer& layer, image::Image& out)
{
  std::visit(
    [&out](const auto& grid) {
      for (std::size_t i = 0; i < grid.values.size(); ++i)
      {
        out.pixels[i] = to_pixel(grid.values[i]);
      }
    },
    layer.data);
}

void unpack(const image::Image& img, Layer& layer)
{
  std::visit(
    [&img](auto& grid) {
      for (std::size_t i = 0; i < grid.values.size(); ++i)
      {
        from_pixel(img.pixels[i], grid.values[i]);
      }
    },
    layer.data);
}

/// A layer's file name: its index, so the folder reads bottom to top, then its name
Path image_file_name(const Layer& layer, std::size_t index)
{
  char buffer[Path::capacity()];
  auto size = static_cast<std::size_t>(std::snprintf(buffer, sizeof(buffer), "%02zu", index));
  // Whatever a file system (or a shell) might make of a layer name becomes '_'
  const std::string_view name = layer.name.view();
  if (!name.empty() and size + 1 < sizeof(buffer))
  {
    buffer[size++] = '_';
  }
  for (const char c : name)
  {
    if (size + 6 >= sizeof(buffer))
    {
      break;
    }
    const bool keep =
      (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '-' or c == '_' or c == '.';
    buffer[size++] = keep ? c : '_';
  }
  std::memcpy(buffer + size, ".png", 5);
  return Path{std::string_view{buffer, size + 4}};
}

/// Room for a sub-arena's alignment padding and an allocation's bookkeeping
constexpr std::size_t kSlack = 1024;

/// Least an encode or decode is given to work in beside the pixels (stb's hash table alone is 128 KiB)
constexpr std::size_t kMinWorkingBytes = 1UL << 20;

/// <code>a + b</code>, or all of memory if that would not fit a size_t (as it can on wasm32: the
/// biggest grids multiply out of 32 bits, and nothing that large fits an arena anyway)
std::size_t saturating(std::size_t a, std::size_t b)
{
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  return (a > kMax - b) ? kMax : a + b;
}

/// Whether a sub-arena of <code>need</code> bytes (and its alignment padding) can be carved from
/// <code>arena</code>; written without adding, since <code>need</code> may be anything up to a saturated size_t
bool has_room(const memory::Arena& arena, std::size_t need)
{
  constexpr std::size_t kPadding = 2 * memory::kSubArenaAlignment;
  const std::size_t room = memory::available(arena);
  return room > kPadding and need <= room - kPadding;
}

/**
 * @brief Memory one layer's image takes to write: the pixels, plus what the PNG encoder works in
 *
 * stb's encoder holds a filtered copy of the image, the deflate output as it doubles and then the
 * finished file; five times the pixels covers that even for cells that do not compress at all.
 */
std::size_t save_bytes(const Layer& layer)
{
  constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
  const std::size_t pixels = extents(layer).count() * sizeof(image::Rgba);
  const std::size_t working = (pixels > kMax / 5) ? kMax : std::max(5 * pixels, kMinWorkingBytes);
  return saturating(saturating(pixels, working), 2 * (kSlack + memory::kSubArenaAlignment));
}

/// Memory one layer's image takes to read: the decoded pixels, plus what the decoder works in
std::size_t load_bytes(const image::Info& info)
{
  const std::size_t working = std::max(info.scratch_bytes, kMinWorkingBytes);
  return saturating(saturating(info.pixel_bytes(), working), 2 * (kSlack + memory::kSubArenaAlignment));
}

/// Writes one layer's cells to <code>file</code>, working in <code>arena</code> (spent wholesale)
Status write_image(const char* file, const Layer& layer, memory::Arena& arena)
{
  const Extents e = extents(layer);
  const std::size_t pixel_bytes = e.count() * sizeof(image::Rgba) + kSlack;
  if (!has_room(arena, saturating(pixel_bytes, kMinWorkingBytes)))
  {
    return Status::error(
      "layer '%s' as a %dx%d image needs about %zu MiB, and %zu MiB is free (raise --memory)",
      layer.name.c_str(),
      e.width,
      e.height,
      save_bytes(layer) >> 20,
      memory::available(arena) >> 20);
  }
  // The pixels in their own sub-arena: the encoder rewinds the one it is given
  memory::Arena pixels;
  memory::Arena encode;
  memory::push(arena, pixels, pixel_bytes, "layer-pixels");
  memory::push(arena, encode, "png-encode");
  image::Image img = image::create(pixels, e.width, e.height);
  pack(layer, img);
  return image::save_png(file, img, encode);
}

/// Reads one layer's cells from <code>file</code>, working in <code>arena</code> (spent wholesale)
Status read_image(const char* file, const image::Info& info, Layer& layer, memory::Arena& arena)
{
  const std::size_t pixel_bytes = info.pixel_bytes() + kSlack;
  if (!has_room(arena, saturating(pixel_bytes, kMinWorkingBytes)))
  {
    return Status::error(
      "layer '%s': '%s' (%dx%d) needs about %zu MiB to decode, and %zu MiB is free (raise --memory)",
      layer.name.c_str(),
      file,
      info.width,
      info.height,
      load_bytes(info) >> 20,
      memory::available(arena) >> 20);
  }
  // Destination and scratch have to be arenas of their own (see image::load)
  memory::Arena pixels;
  memory::Arena scratch;
  memory::push(arena, pixels, pixel_bytes, "layer-pixels");
  memory::push(arena, scratch, "image-scratch");
  image::Image img;
  if (const Status s = image::load(file, pixels, scratch, img); !s.ok)
  {
    return Status::error("layer '%s': %s", layer.name.c_str(), s.message.c_str());
  }
  const Extents e = extents(layer);
  if (img.width != e.width or img.height != e.height)
  {
    return Status::error(
      "layer '%s': '%s' is %dx%d, but the level is %dx%d cells",
      layer.name.c_str(),
      file,
      img.width,
      img.height,
      e.width,
      e.height);
  }
  unpack(img, layer);
  return Status::okay();
}

}  // namespace

Path layer_image_folder(const char* path)
{
  return Path{format<Path::capacity()>(
    "%.*s.layers", static_cast<int>(file::without_extension(std::string_view{path}).size()), path)};
}

Path layer_image_path(const char* path, const Level& level, std::size_t index)
{
  LVL_ASSERT_LT(index, level.layers.size());
  return joined(layer_image_folder(path).view(), image_file_name(level.layers[index], index).view());
}

namespace io
{

Path layer_image_reference(const char* path, const Level& level, std::size_t index)
{
  // The folder is named after the level file and sits beside it, so what the level file should call
  // it is known without consulting the file system: the folder's own name, then the image's
  const Path folder = layer_image_folder(path);
  const std::string_view in = folder_of(folder.view());
  std::string_view name = folder.view();
  name.remove_prefix(in.empty() ? 0 : ((in == "/") ? 1 : in.size() + 1));
  return joined(name, image_file_name(level.layers[index], index).view());
}

Status save_layer_images(const char* path, const Level& level, IoContext& ctx, memory::Vector<LayerHeader>& headers)
{
  const std::size_t layer_count = level.layers.size();
  LVL_ASSERT_EQ(headers.size(), layer_count);
  const Path folder = layer_image_folder(path);
  LVL_RETURN_IF_ERROR(file::make_directory(folder.c_str()));
  progress_stage(ctx, "Writing layer images");

  memory::Arena& shared = shared_arena(ctx);
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, kMaxIoWorkers);
  memory::Scope scope{shared};
  auto results = memory::make_vector<Status>(shared, workers, Status::okay());
  memory::Arena buffers[kMaxIoWorkers];
  // A wave at a time: one sub-arena per layer, carved here so bump allocation stays on this thread,
  // filled and encoded in parallel, then reclaimed -- so peak use is one wave's images
  for (std::size_t first = 0; first < layer_count;)
  {
    memory::Scope wave_scope{shared};
    std::size_t count = 0;
    while (count < workers and first + count < layer_count)
    {
      const std::size_t need = save_bytes(level.layers[first + count]);
      if (!has_room(shared, need))
      {
        break;
      }
      memory::push(shared, buffers[count], need, "layer-image");
      ++count;
    }
    if (count == 0)
    {
      // This one layer's image does not fit beside anything else: give it everything there is
      memory::push(shared, buffers[0], "layer-image");
      count = 1;
    }
    jobs::parallel_for(count, count, [&](std::size_t k, std::size_t) {
      const Path file = layer_image_path(path, level, first + k);
      results[k] = write_image(file.c_str(), level.layers[first + k], buffers[k]);
      progress_advance(ctx);
    });
    for (const auto& result : results)
    {
      LVL_RETURN_IF_ERROR(result);
    }
    for (std::size_t k = 0; k < count; ++k)
    {
      headers[first + k].image = layer_image_reference(path, level, first + k);
      memory::reset(buffers[k]);
    }
    first += count;
  }
  return Status::okay();
}

Status load_layer_images(
  const char* path,
  Level& level,
  const memory::Vector<LayerHeader>& headers,
  std::size_t base,
  IoContext& ctx)
{
  const std::size_t count = headers.size();
  if (std::none_of(headers.begin(), headers.end(), [](const LayerHeader& h) { return !h.image.empty(); }))
  {
    return Status::okay();
  }
  progress_stage(ctx, "Loading layer images");
  memory::Arena& shared = shared_arena(ctx);
  const std::size_t workers = std::min<std::size_t>(ctx.worker_count, kMaxIoWorkers);
  memory::Scope scope{shared};
  auto results = memory::make_vector<Status>(shared, workers, Status::okay());
  auto files = memory::make_vector<Path>(shared, workers, Path{});
  auto infos = memory::make_vector<image::Info>(shared, workers, image::Info{});
  auto indices = memory::make_vector<std::size_t>(shared, workers, 0UL);
  memory::Arena buffers[kMaxIoWorkers];

  // As with saving, a wave at a time; what a decode needs is read from each image's header first
  for (std::size_t first = 0; first < count;)
  {
    while (first < count and headers[first].image.empty())
    {
      ++first;
    }
    if (first >= count)
    {
      break;
    }
    memory::Scope wave_scope{shared};
    std::size_t wave = 0;
    std::size_t next = first;
    while (wave < workers and next < count)
    {
      if (headers[next].image.empty())
      {
        ++next;
        continue;
      }
      const Path file = resolved(path, headers[next].image.view());
      image::Info info;
      if (const Status s = image::probe(file.c_str(), ctx.workers[0].scratch, info); !s.ok)
      {
        return Status::error("layer '%s': %s", headers[next].name.c_str(), s.message.c_str());
      }
      const bool fits = has_room(shared, load_bytes(info));
      if (wave > 0 and !fits)
      {
        break;
      }
      // The first image of a wave gets whatever there is, so no level is too big to load layer by layer
      if (fits)
      {
        memory::push(shared, buffers[wave], load_bytes(info), "layer-image");
      }
      else
      {
        memory::push(shared, buffers[wave], "layer-image");
      }
      files[wave] = file;
      infos[wave] = info;
      indices[wave] = next;
      ++wave;
      ++next;
      if (!fits)
      {
        // That image took the whole arena: nothing can be read beside it, not even another header
        break;
      }
    }
    jobs::parallel_for(wave, wave, [&](std::size_t k, std::size_t) {
      results[k] = read_image(files[k].c_str(), infos[k], level.layers[base + indices[k]], buffers[k]);
      progress_advance(ctx);
    });
    for (const auto& result : results)
    {
      LVL_RETURN_IF_ERROR(result);
    }
    for (std::size_t k = 0; k < wave; ++k)
    {
      memory::reset(buffers[k]);
    }
    first = next;
  }
  return Status::okay();
}

}  // namespace io
}  // namespace lvl::level
