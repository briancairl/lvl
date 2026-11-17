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

void* stb_malloc(std::size_t size)
{
  LVL_ASSERT_NON_NULL(stb_scratch) << "stb allocation outside of an lvl::image call";
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

Status decode(const void* data, std::size_t size, memory::Arena& dest, memory::Arena& scratch, Image& out)
{
  LVL_ASSERT_NE(&dest, &scratch) << "decoded images must not live in the scratch arena";
  detail::StbScope scope{scratch};
  int w = 0;
  int h = 0;
  int channels = 0;
  stbi_uc* const decoded =
    stbi_load_from_memory(static_cast<const stbi_uc*>(data), static_cast<int>(size), &w, &h, &channels, STBI_rgb_alpha);
  if (decoded == nullptr)
  {
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
  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, scratch, data, size));
  if (Status s = decode(data, size, dest, scratch, out); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  return Status::okay();
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
    return Status::error("PNG encoding failed for '%s'", path);
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
