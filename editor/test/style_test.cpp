// C++ Standard Library
#include <string>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/editor/style.hpp"

using namespace lvl;
using namespace lvl::editor;

class Style : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ImGui::CreateContext();
    memory::create(arena, 4 << 20, "json");
    json::install(arena);
  }
  void TearDown() override
  {
    json::uninstall();
    memory::destroy(arena);
    ImGui::DestroyContext();
  }
  static std::string temp(const char* name) { return std::string{::testing::TempDir()} + name; }
  memory::Arena arena;
};

TEST_F(Style, RoundTripThroughFile)
{
  style::Profile original;
  style::apply(style::Preset::kLight, original);
  original.font_scale = 1.25F;
  original.style.WindowRounding = 7.0F;
  original.style.ItemSpacing = ImVec2{3.0F, 9.0F};
  original.style.AntiAliasedFill = false;
  original.style.WindowMenuButtonPosition = ImGuiDir_Right;
  original.style.Colors[ImGuiCol_Button] = ImVec4{1.0F, 0.0F, 0.2F, 0.6F};
  const auto path = temp("style.json");
  ASSERT_TRUE(style::save_file(path.c_str(), original).ok);

  style::Profile loaded;
  style::apply(style::Preset::kDark, loaded);
  const auto status = style::load_file(path.c_str(), loaded);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_FLOAT_EQ(loaded.font_scale, 1.25F);
  EXPECT_FLOAT_EQ(loaded.style.WindowRounding, 7.0F);
  EXPECT_FLOAT_EQ(loaded.style.ItemSpacing.y, 9.0F);
  EXPECT_FALSE(loaded.style.AntiAliasedFill);
  EXPECT_EQ(loaded.style.WindowMenuButtonPosition, ImGuiDir_Right);
  // Colors are stored as 8-bit channels
  for (int i = 0; i < ImGuiCol_COUNT; ++i)
  {
    EXPECT_NEAR(loaded.style.Colors[i].x, original.style.Colors[i].x, 1.0F / 255.0F) << ImGui::GetStyleColorName(i);
    EXPECT_NEAR(loaded.style.Colors[i].w, original.style.Colors[i].w, 1.0F / 255.0F) << ImGui::GetStyleColorName(i);
  }
}

TEST_F(Style, PartialProfilesKeepOtherValues)
{
  const auto path = temp("partial.json");
  const char text[] = R"({"format": "lvl-style", "vars": {"FrameRounding": 6}, "colors": {"Text": "#ff0000"}})";
  ASSERT_TRUE(file::write(path.c_str(), text, sizeof(text) - 1).ok);
  style::Profile p;
  style::apply(style::Preset::kLvl, p);
  const float window_rounding = p.style.WindowRounding;
  ASSERT_TRUE(style::load_file(path.c_str(), p).ok);
  EXPECT_FLOAT_EQ(p.style.FrameRounding, 6.0F);
  EXPECT_FLOAT_EQ(p.style.WindowRounding, window_rounding);
  EXPECT_FLOAT_EQ(p.style.Colors[ImGuiCol_Text].x, 1.0F);
  EXPECT_FLOAT_EQ(p.style.Colors[ImGuiCol_Text].w, 1.0F);
}

TEST_F(Style, BadProfilesAreRejectedAndLeaveTheStyleAlone)
{
  const auto path = temp("bad.json");
  const char text[] = R"({"format": "lvl-style", "vars": {"FrameRounding": "round"}})";
  ASSERT_TRUE(file::write(path.c_str(), text, sizeof(text) - 1).ok);
  style::Profile p;
  style::apply(style::Preset::kLvl, p);
  const float rounding = p.style.FrameRounding;
  const auto status = style::load_file(path.c_str(), p);
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.view().find("FrameRounding"), std::string_view::npos) << status;
  EXPECT_FLOAT_EQ(p.style.FrameRounding, rounding);
  EXPECT_FALSE(style::load_file("/nonexistent/style.json", p).ok);
}
