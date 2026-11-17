// C++ Standard Library
#include <type_traits>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/build.hpp"

TEST(Build, VersionIsPacked)
{
  EXPECT_EQ(lvl::kVersion, lvl::kVersionMajor * 10000 + lvl::kVersionMinor * 100 + lvl::kVersionPatch);
  EXPECT_FALSE(lvl::kIsWeb);
}
