// C++ Standard Library
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/image.hpp"

using namespace lvl;

class ImageTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(dest, 1 << 22, "images");
    memory::create(scratch, 1 << 22, "scratch");
  }
  void TearDown() override
  {
    memory::destroy(scratch);
    memory::destroy(dest);
  }
  memory::Arena dest;
  memory::Arena scratch;
};

TEST_F(ImageTest, PngRoundTrip)
{
  image::Image img = image::create(dest, 37, 21, image::Rgba{10, 20, 30, 255});
  img.at(5, 7) = image::Rgba{255, 0, 0, 128};
  const std::string path = std::string{::testing::TempDir()} + "img.png";
  ASSERT_TRUE(image::save_png(path.c_str(), img, scratch).ok);
  EXPECT_EQ(scratch.utilized, 0UL);

  image::Image loaded;
  const auto status = image::load(path.c_str(), dest, scratch, loaded);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_EQ(scratch.utilized, 0UL);
  ASSERT_EQ(loaded.width, 37);
  ASSERT_EQ(loaded.height, 21);
  EXPECT_EQ(loaded.at(0, 0), (image::Rgba{10, 20, 30, 255}));
  EXPECT_EQ(loaded.at(5, 7), (image::Rgba{255, 0, 0, 128}));
}

TEST_F(ImageTest, SingleArenaLoadKeepsOnlyThePixels)
{
  const image::Image img = image::create(scratch, 40, 30, image::Rgba{1, 2, 3, 255});
  const std::string path = std::string{::testing::TempDir()} + "one_arena.png";
  ASSERT_TRUE(image::save_png(path.c_str(), img, dest).ok);
  memory::clear(scratch);
  image::Image loaded;
  ASSERT_TRUE(image::load(path.c_str(), scratch, loaded).ok);
  EXPECT_EQ(loaded.at(39, 29), (image::Rgba{1, 2, 3, 255}));
  EXPECT_LE(scratch.utilized, 40UL * 30UL * 4UL + 128UL);
  // Failure leaves the arena as it was
  const auto before = scratch.utilized;
  EXPECT_FALSE(image::load("/nonexistent.png", scratch, loaded).ok);
  EXPECT_EQ(scratch.utilized, before);
}

TEST_F(ImageTest, LoadMissingOrInvalidFails)
{
  image::Image img;
  EXPECT_FALSE(image::load("/nonexistent.png", dest, scratch, img).ok);
  const char garbage[] = "not an image";
  EXPECT_FALSE(image::decode(garbage, sizeof(garbage), dest, scratch, img).ok);
}

TEST_F(ImageTest, ResizeKeepsFlatColor)
{
  const image::Image img = image::create(dest, 64, 32, image::Rgba{100, 150, 200, 255});
  const image::Image small = image::resize(img, 16, 8, dest, scratch);
  ASSERT_EQ(small.width, 16);
  const auto px = small.at(8, 4);
  EXPECT_NEAR(px.r, 100, 1);
  EXPECT_NEAR(px.g, 150, 1);
  EXPECT_NEAR(px.b, 200, 1);
}

namespace
{

/// A large image that compresses poorly (so its PNG is big too)
image::Image noisy(memory::Arena& arena, int width, int height)
{
  image::Image img = image::create(arena, width, height);
  std::uint32_t state = 12345;
  for (std::size_t i = 0; i < img.size(); ++i)
  {
    state = state * 1664525U + 1013904223U;
    img.pixels[i] = image::Rgba{
      static_cast<std::uint8_t>(state >> 24),
      static_cast<std::uint8_t>(state >> 16),
      static_cast<std::uint8_t>(state >> 8),
      255};
  }
  return img;
}

}  // namespace

TEST(ImageMemory, TooLittleScratchIsAnErrorNotAnAbort)
{
  memory::Arena big;
  memory::create(big, 256UL << 20, "big");
  memory::Arena dest;
  memory::Arena scratch;
  memory::push(big, dest, 64UL << 20, "dest");
  memory::push(big, scratch, 96UL << 20, "scratch");
  const image::Image img = noisy(dest, 2048, 1700);
  const std::string path = std::string{::testing::TempDir()} + "big.png";
  ASSERT_TRUE(image::save_png(path.c_str(), img, scratch).ok);
  memory::clear(dest);

  // The case from the field: ~17.8 MiB of scratch for a 13 MiB image
  memory::Arena small;
  memory::push(big, small, 18622080UL, "small-scratch");
  image::Image loaded;
  const auto status = image::load(path.c_str(), dest, small, loaded);
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.view().find("scratch"), std::string_view::npos) << status;
  EXPECT_EQ(small.utilized, 0UL);
  EXPECT_EQ(dest.utilized, 0UL);

  // With enough scratch it decodes, within the estimate
  memory::pop(big, small);
  image::Info info;
  ASSERT_TRUE(image::probe(path.c_str(), scratch, info).ok);
  EXPECT_EQ(info.width, 2048);
  EXPECT_EQ(info.height, 1700);
  EXPECT_LE(info.scratch_bytes, memory::available(scratch));
  scratch.peak = 0;
  const auto ok = image::load(path.c_str(), dest, scratch, loaded);
  ASSERT_TRUE(ok.ok) << ok;
  // The estimate bounds the real decode, without being wildly pessimistic
  EXPECT_LE(scratch.peak, info.scratch_bytes);
  EXPECT_GE(scratch.peak, info.scratch_bytes * 3 / 4);
  EXPECT_EQ(loaded.width, 2048);
  EXPECT_EQ(loaded.at(100, 100), img.at(100, 100));

  // A destination too small for the pixels is an error too
  memory::clear(dest);
  memory::Arena tiny_dest;
  memory::push(dest, tiny_dest, 1 << 20, "tiny-dest");
  const auto no_room = image::load(path.c_str(), tiny_dest, scratch, loaded);
  EXPECT_FALSE(no_room.ok);
  memory::pop(dest, tiny_dest);
  memory::destroy(big);
}

TEST(ImageMemory, EncodingWithTooLittleScratchIsAnError)
{
  memory::Arena big;
  memory::create(big, 64UL << 20, "big");
  memory::Arena dest;
  memory::Arena scratch;
  memory::push(big, dest, 32UL << 20, "dest");
  memory::push(big, scratch, 1UL << 20, "scratch");
  const image::Image img = noisy(dest, 1024, 1024);
  const std::string path = std::string{::testing::TempDir()} + "enc.png";
  EXPECT_FALSE(image::save_png(path.c_str(), img, scratch).ok);
  memory::destroy(big);
}

TEST(ImageBlend, OpaqueAndTransparent)
{
  const image::Rgba dst{0, 0, 0, 255};
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 255}), (image::Rgba{200, 100, 50, 255}));
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 0}), dst);
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 255}, 0.5F).r, 100);
}
