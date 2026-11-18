/**
 * @copyright 2026-present Brian Cairl
 *
 * @file image.hpp
 *
 * RGBA8 images (tileset atlases, seed images, exports) on top of stb_image / stb_image_write /
 * stb_image_resize2. stb's own allocations are routed (STBI_MALLOC & co.) into a caller-provided
 * scratch arena, which is rewound afterwards; results are copied into the destination arena. So
 * image work never touches the heap.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl::image
{

/// 8-bit RGBA pixel
struct Rgba
{
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;

  friend constexpr bool operator==(const Rgba& lhs, const Rgba& rhs)
  {
    return lhs.r == rhs.r and lhs.g == rhs.g and lhs.b == rhs.b and lhs.a == rhs.a;
  }
};

static_assert(sizeof(Rgba) == 4);

/// Row-major RGBA8 image; <code>pixels</code> is owned by whichever arena it was allocated from
struct Image
{
  int width = 0;
  int height = 0;
  Rgba* pixels = nullptr;

  [[nodiscard]] bool empty() const { return pixels == nullptr or width <= 0 or height <= 0; }
  [[nodiscard]] std::size_t size() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }
  [[nodiscard]] Rgba& at(int x, int y) { return pixels[static_cast<std::size_t>(y) * width + x]; }
  [[nodiscard]] const Rgba& at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
};

/// Allocates a <code>width</code> x <code>height</code> image filled with <code>fill</code>
Image create(memory::Arena& arena, int width, int height, Rgba fill = Rgba{0, 0, 0, 0});

/// What an encoded image needs, read from its header without decoding it
struct Info
{
  int width = 0;
  int height = 0;
  /// Channels stored in the file (decoding always produces 4)
  int channels = 0;
  bool sixteen_bit = false;
  /// Size of the encoded file
  std::size_t file_bytes = 0;
  /// Scratch memory <code>load()</code> needs for this image: the file plus stb's decode buffers (an
  /// upper bound)
  std::size_t scratch_bytes = 0;

  /// Destination memory for the decoded RGBA8 pixels
  [[nodiscard]] std::size_t pixel_bytes() const
  {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(Rgba);
  }
};

/// Reads the header of an encoded image held in memory (stb may use a little of <code>scratch</code>, released after)
Status info(const void* data, std::size_t size, memory::Arena& scratch, Info& out);

/// Reads the header of an image file (the start of the file is read into <code>scratch</code>, then released)
Status probe(const char* path, memory::Arena& scratch, Info& out);

/**
 * @brief Decodes an image file (PNG, JPEG, BMP, TGA, ...) into <code>dest</code> as RGBA8
 *
 * Fails (rather than asserting) if <code>scratch</code> can't hold the file and decode buffers, or
 * <code>dest</code> the pixels; see probe() for how much each needs.
 */
Status load(const char* path, memory::Arena& dest, memory::Arena& scratch, Image& out);

/**
 * @brief As load(), with one arena for both: the pixels are allocated from <code>arena</code> first,
 *        then the rest of it is lent to the decoder as scratch and given back
 */
Status load(const char* path, memory::Arena& arena, Image& out);

/// Decodes an encoded image held in memory into <code>dest</code> as RGBA8 (same checks as load())
Status decode(const void* data, std::size_t size, memory::Arena& dest, memory::Arena& scratch, Image& out);

/// Encodes <code>img</code> as PNG and writes it to <code>path</code> (fails if <code>scratch</code> is too small)
Status save_png(const char* path, const Image& img, memory::Arena& scratch);

/// Resamples <code>src</code> to <code>width</code> x <code>height</code> into <code>dest</code>
Image resize(const Image& src, int width, int height, memory::Arena& dest, memory::Arena& scratch);

/// Alpha-blends <code>src</code> over <code>dst</code> with an extra <code>opacity</code> in [0, 1]
constexpr Rgba blend(Rgba dst, Rgba src, float opacity = 1.0F)
{
  const float a = (static_cast<float>(src.a) / 255.0F) * opacity;
  const auto mix = [a](std::uint8_t d, std::uint8_t s) {
    return static_cast<std::uint8_t>(
      static_cast<float>(d) + (static_cast<float>(s) - static_cast<float>(d)) * a + 0.5F);
  };
  const float out_a = a + (static_cast<float>(dst.a) / 255.0F) * (1.0F - a);
  return Rgba{
    mix(dst.r, src.r), mix(dst.g, src.g), mix(dst.b, src.b), static_cast<std::uint8_t>(out_a * 255.0F + 0.5F)};
}

}  // namespace lvl::image
