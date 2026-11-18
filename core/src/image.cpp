/**
 * @copyright 2026-present Brian Cairl
 *
 * @file image.cpp
 *
 * The single implementation translation unit for stb_image, stb_image_write and stb_image_resize2,
 * with their allocators routed into a thread-local scratch arena.
 */

// C++ Standard Library
#include <algorithm>
#include <cstring>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/image.hpp"
#include "lvl/core/logging.hpp"

namespace lvl::image::detail
{
namespace
{

/// Scratch arena stb allocates from on this thread (set for the duration of one stb call)
thread_local memory::Arena* stb_scratch = nullptr;

/// Each block is preceded by its size, so realloc can copy the right amount
constexpr std::size_t kHeader = 16UL;

}  // namespace

/// Set when an stb allocation was refused (so failures can be reported as "out of scratch memory")
thread_local bool stb_out_of_memory = false;

void* stb_malloc(std::size_t size)
{
  LVL_ASSERT_NON_NULL(stb_scratch) << "stb allocation outside of an lvl::image call";
  // Refuse (stb then fails cleanly) rather than assert: image sizes come from user files
  if (size + 2 * kHeader > memory::available(*stb_scratch))
  {
    stb_out_of_memory = true;
    return nullptr;
  }
  auto* const block = static_cast<std::uint8_t*>(memory::allocate_bytes(*stb_scratch, size + kHeader, kHeader));
  std::memcpy(block, &size, sizeof(size));
  return block + kHeader;
}

void stb_free(void* p)
{
  if (p != nullptr and stb_scratch != nullptr)
  {
    auto* const block = static_cast<std::uint8_t*>(p) - kHeader;
    std::size_t size = 0;
    std::memcpy(&size, block, sizeof(size));
    memory::release_bytes(*stb_scratch, block, size + kHeader);
  }
}

void* stb_realloc(void* p, std::size_t new_size)
{
  if (p == nullptr)
  {
    return stb_malloc(new_size);
  }
  auto* const block = static_cast<std::uint8_t*>(p) - kHeader;
  std::size_t old_size = 0;
  std::memcpy(&old_size, block, sizeof(old_size));
  // Growing buffers (e.g. zlib output) are usually the newest block: extend in place
  if (memory::try_resize_bytes(*stb_scratch, block, new_size + kHeader))
  {
    std::memcpy(block, &new_size, sizeof(new_size));
    return p;
  }
  void* const q = stb_malloc(new_size);
  if (q == nullptr)
  {
    return nullptr;
  }
  std::memcpy(q, p, std::min(old_size, new_size));
  return q;
}

/// Routes stb allocations to <code>scratch</code> for its lifetime, then rewinds it
class StbScope
{
public:
  explicit StbScope(memory::Arena& scratch) : scope_{scratch}
  {
    LVL_ASSERT_NULL(stb_scratch) << "nested lvl::image calls on one thread";
    stb_scratch = &scratch;
    stb_out_of_memory = false;
  }
  ~StbScope() { stb_scratch = nullptr; }

  StbScope(const StbScope&) = delete;
  StbScope& operator=(const StbScope&) = delete;

private:
  memory::Scope scope_;
};

}  // namespace lvl::image::detail

#define STBI_MALLOC(sz) ::lvl::image::detail::stb_malloc(sz)
#define STBI_REALLOC(p, newsz) ::lvl::image::detail::stb_realloc(p, newsz)
#define STBI_FREE(p) ::lvl::image::detail::stb_free(p)
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STBIW_MALLOC(sz) ::lvl::image::detail::stb_malloc(sz)
#define STBIW_REALLOC(p, newsz) ::lvl::image::detail::stb_realloc(p, newsz)
#define STBIW_FREE(p) ::lvl::image::detail::stb_free(p)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STBIR_MALLOC(size, user_data) ((void)(user_data), ::lvl::image::detail::stb_malloc(size))
#define STBIR_FREE(ptr, user_data) ((void)(user_data), ::lvl::image::detail::stb_free(ptr))
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

namespace lvl::image
{

Image create(memory::Arena& arena, int width, int height, Rgba fill)
{
  LVL_ASSERT_GT(width, 0);
  LVL_ASSERT_GT(height, 0);
  Image img{.width = width, .height = height, .pixels = nullptr};
  img.pixels = memory::allocate<Rgba>(arena, img.size(), fill);
  return img;
}

namespace
{

constexpr std::size_t kMiB = 1UL << 20;

/// Scratch stb needs to decode an image beyond the encoded data itself (measured against stb's
/// allocations; bump allocation reclaims little in between):
/// - a copy of the compressed stream (PNG IDAT chunks are concatenated);
/// - the inflated rows (one filter byte per row);
/// - the image at its stored channel count and depth;
/// - conversions: 16 -> 8 bit, then to 4 channels, when the file isn't already RGBA8;
/// plus slack
std::size_t decode_bytes(const Info& i)
{
  const std::size_t pixels = static_cast<std::size_t>(i.width) * static_cast<std::size_t>(i.height);
  const std::size_t channels = static_cast<std::size_t>(std::max(i.channels, 1));
  const std::size_t depth = i.sixteen_bit ? 2 : 1;
  const std::size_t stored = pixels * channels * depth;
  const std::size_t rows = static_cast<std::size_t>(i.height) * depth;
  const std::size_t to_8_bit = i.sixteen_bit ? pixels * channels : 0;
  const std::size_t to_rgba = (channels != 4) ? pixels * sizeof(Rgba) : 0;
  const std::size_t total = i.file_bytes + (stored + rows) + stored + to_8_bit + to_rgba;
  return total + total / 32 + 64UL * 1024UL;
}

}  // namespace

Status info(const void* data, std::size_t size, memory::Arena& scratch, Info& out)
{
  out = Info{};
  const auto* bytes = static_cast<const stbi_uc*>(data);
  detail::StbScope scope{scratch};
  if (stbi_info_from_memory(bytes, static_cast<int>(size), &out.width, &out.height, &out.channels) == 0)
  {
    return Status::error(
      detail::stb_out_of_memory ? "out of scratch memory reading the image header" : "unrecognized or corrupt image");
  }
  if (out.width <= 0 or out.height <= 0 or out.width > 32768 or out.height > 32768)
  {
    return Status::error("unsupported image size %dx%d", out.width, out.height);
  }
  out.sixteen_bit = stbi_is_16_bit_from_memory(bytes, static_cast<int>(size)) != 0;
  out.file_bytes = size;
  out.scratch_bytes = size + decode_bytes(out);
  return Status::okay();
}

Status probe(const char* path, memory::Arena& scratch, Info& out)
{
  memory::Scope scope{scratch};
  // Headers are at the start of the file (JPEG may put metadata before its size marker)
  constexpr std::size_t kPrefix = 256UL * 1024UL;
  char* data = nullptr;
  std::size_t size = 0;
  std::size_t total = 0;
  LVL_RETURN_IF_ERROR(file::read_prefix(path, scratch, kPrefix, data, size, total));
  Status s = info(data, size, scratch, out);
  if (!s.ok and total > size)
  {
    LVL_RETURN_IF_ERROR(file::read_prefix(path, scratch, total, data, size, total));
    s = info(data, size, scratch, out);
  }
  if (!s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  // Sizes were computed from the prefix: account for the whole file
  out.file_bytes = total;
  out.scratch_bytes = total + decode_bytes(out);
  return Status::okay();
}

Status decode(const void* data, std::size_t size, memory::Arena& dest, memory::Arena& scratch, Image& out)
{
  LVL_ASSERT_NE(&dest, &scratch) << "decoded images must not live in the scratch arena";
  Info header;
  LVL_RETURN_IF_ERROR(info(data, size, scratch, header));
  const std::size_t needed = decode_bytes(header);
  if (needed > memory::available(scratch))
  {
    return Status::error(
      "decoding this %dx%d image needs about %zu MiB of scratch memory, but only %zu MiB is available "
      "(raise --memory)",
      header.width,
      header.height,
      (needed + kMiB - 1) / kMiB,
      memory::available(scratch) / kMiB);
  }
  if (header.pixel_bytes() + 64 > memory::available(dest))
  {
    return Status::error(
      "this %dx%d image needs %zu MiB, but only %zu MiB is available in '%s' (raise --memory)",
      header.width,
      header.height,
      (header.pixel_bytes() + kMiB - 1) / kMiB,
      memory::available(dest) / kMiB,
      dest.label);
  }
  detail::StbScope scope{scratch};
  int w = 0;
  int h = 0;
  int channels = 0;
  stbi_uc* const decoded =
    stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &w, &h, &channels, STBI_rgb_alpha);
  if (decoded == nullptr)
  {
    if (detail::stb_out_of_memory)
    {
      return Status::error(
        "ran out of scratch memory decoding a %dx%d image (raise --memory)", header.width, header.height);
    }
    return Status::error("cannot decode image: %s", stbi_failure_reason());
  }
  out = Image{.width = w, .height = h, .pixels = nullptr};
  out.pixels = memory::allocate<Rgba>(dest, out.size());
  std::memcpy(static_cast<void*>(out.pixels), decoded, out.size() * sizeof(Rgba));
  return Status::okay();
}

Status load(const char* path, memory::Arena& dest, memory::Arena& scratch, Image& out)
{
  // The encoded file is read into scratch *before* decoding: decode() rewinds scratch to its own
  // mark, which is after the file data, so the file stays valid while it's decoded.
  memory::Scope scope{scratch};
  std::size_t total = 0;
  LVL_RETURN_IF_ERROR(file::size(path, total));
  if (total + 1 > memory::available(scratch))
  {
    return Status::error(
      "%s: the file (%zu MiB) does not fit in %zu MiB of scratch memory (raise --memory)",
      path,
      (total + kMiB - 1) / kMiB,
      memory::available(scratch) / kMiB);
  }
  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, scratch, data, size));
  if (Status s = decode(data, size, dest, scratch, out); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  return Status::okay();
}

Status load(const char* path, memory::Arena& arena, Image& out)
{
  Info header;
  LVL_RETURN_IF_ERROR(probe(path, arena, header));
  constexpr std::size_t kSlack = 64;
  const std::size_t pixel_bytes = header.pixel_bytes() + kSlack;
  // Pixels, then scratch above them (popped before returning, so only the pixels remain)
  if (pixel_bytes + 4 * kSlack > memory::available(arena))
  {
    return Status::error(
      "%s: a %dx%d image needs %zu MiB, but only %zu MiB is available in '%s' (raise --memory)",
      path,
      header.width,
      header.height,
      (pixel_bytes + kMiB - 1) / kMiB,
      memory::available(arena) / kMiB,
      arena.label);
  }
  memory::Arena slot;
  memory::initialize(slot, memory::allocate_bytes(arena, pixel_bytes, alignof(Rgba)), pixel_bytes, "image-pixels");
  memory::Arena scratch;
  memory::push(arena, scratch, "image-scratch");
  const Status status = load(path, slot, scratch, out);
  memory::pop(arena, scratch);
  if (!status.ok)
  {
    // Give the reserved pixels back too (they are the newest block)
    memory::release_bytes(arena, slot.data, pixel_bytes);
  }
  return status;
}

Status save_png(const char* path, const Image& img, memory::Arena& scratch)
{
  LVL_ASSERT_FALSE(img.empty());
  detail::StbScope scope{scratch};
  int length = 0;
  unsigned char* const png = stbi_write_png_to_mem(
    reinterpret_cast<const unsigned char*>(img.pixels),
    img.width * static_cast<int>(sizeof(Rgba)),
    img.width,
    img.height,
    4,
    &length);
  if (png == nullptr)
  {
    return detail::stb_out_of_memory
      ? Status::error("ran out of scratch memory encoding %s (%dx%d; raise --memory)", path, img.width, img.height)
      : Status::error("PNG encoding failed for '%s'", path);
  }
  return file::write(path, png, static_cast<std::size_t>(length));
}

Image resize(const Image& src, int width, int height, memory::Arena& dest, memory::Arena& scratch)
{
  LVL_ASSERT_FALSE(src.empty());
  LVL_ASSERT_NE(&dest, &scratch) << "resized images must not live in the scratch arena";
  Image out = create(dest, width, height);
  detail::StbScope scope{scratch};
  stbir_resize_uint8_srgb(
    reinterpret_cast<const unsigned char*>(src.pixels),
    src.width,
    src.height,
    0,
    reinterpret_cast<unsigned char*>(out.pixels),
    width,
    height,
    0,
    STBIR_RGBA);
  return out;
}

}  // namespace lvl::image
