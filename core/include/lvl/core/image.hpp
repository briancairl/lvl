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

/// Decodes an image file (PNG, JPEG, BMP, TGA, ...) into <code>dest</code> as RGBA8
Status load(const char* path, memory::Arena& dest, memory::Arena& scratch, Image& out);

/// Decodes an encoded image held in memory into <code>dest</code> as RGBA8
Status decode(const void* data, std::size_t size, memory::Arena& dest, memory::Arena& scratch, Image& out);

/// Encodes <code>img</code> as PNG and writes it to <code>path</code>
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
