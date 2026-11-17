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

TEST(ImageBlend, OpaqueAndTransparent)
{
  const image::Rgba dst{0, 0, 0, 255};
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 255}), (image::Rgba{200, 100, 50, 255}));
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 0}), dst);
  EXPECT_EQ(image::blend(dst, image::Rgba{200, 100, 50, 255}, 0.5F).r, 100);
}
