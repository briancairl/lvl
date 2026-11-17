/**
 * @copyright 2026-present Brian Cairl
 *
 * @file style.cpp
 */

// C++ Standard Library
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

// LVL
#include "lvl/core/build.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/format.hpp"
#include "lvl/editor/style.hpp"

// ImGuiStyle fields serialized by name, by type
// clang-format off
#define LVL_STYLE_FLOATS(X)                                                                                            \
  X(Alpha) X(DisabledAlpha) X(WindowRounding) X(WindowBorderSize) X(WindowBorderHoverPadding) X(ChildRounding)        \
  X(ChildBorderSize) X(PopupRounding) X(PopupBorderSize) X(FrameRounding) X(FrameBorderSize) X(IndentSpacing)          \
  X(ColumnsMinSpacing) X(ScrollbarSize) X(ScrollbarRounding) X(GrabMinSize) X(GrabRounding) X(LogSliderDeadzone)      \
  X(ImageBorderSize) X(TabRounding) X(TabBorderSize) X(TabCloseButtonMinWidthSelected)                                 \
  X(TabCloseButtonMinWidthUnselected) X(TabBarBorderSize) X(TabBarOverlineSize) X(TableAngledHeadersAngle)            \
  X(SeparatorTextBorderSize) X(DockingSeparatorSize) X(MouseCursorScale) X(CurveTessellationTol)                       \
  X(CircleTessellationMaxError) X(HoverStationaryDelay) X(HoverDelayShort) X(HoverDelayNormal)

#define LVL_STYLE_VEC2S(X)                                                                                             \
  X(WindowPadding) X(WindowMinSize) X(WindowTitleAlign) X(FramePadding) X(ItemSpacing) X(ItemInnerSpacing)             \
  X(CellPadding) X(TouchExtraPadding) X(TableAngledHeadersTextAlign) X(ButtonTextAlign) X(SelectableTextAlign)         \
  X(SeparatorTextAlign) X(SeparatorTextPadding) X(DisplayWindowPadding) X(DisplaySafeAreaPadding)

#define LVL_STYLE_BOOLS(X) X(AntiAliasedLines) X(AntiAliasedLinesUseTex) X(AntiAliasedFill)

#define LVL_STYLE_DIRS(X) X(WindowMenuButtonPosition) X(ColorButtonPosition)
// clang-format on

namespace lvl::editor::style
{
namespace
{

constexpr std::string_view kFormat = "lvl-style";

int hex_digit(char c)
{
  if (c >= '0' and c <= '9')
  {
    return c - '0';
  }
  if (c >= 'a' and c <= 'f')
  {
    return 10 + c - 'a';
  }
  if (c >= 'A' and c <= 'F')
  {
    return 10 + c - 'A';
  }
  return -1;
}

std::uint8_t to_byte(float v)
{
  const float c = (v < 0.0F) ? 0.0F : (v > 1.0F ? 1.0F : v);
  return static_cast<std::uint8_t>(c * 255.0F + 0.5F);
}

void save_color(Json& j, const ImVec4& c)
{
  char text[16];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", to_byte(c.x), to_byte(c.y), to_byte(c.z), to_byte(c.w));
  j = JsonString{text};
}

bool load_color(json::Reader& r, const Json& j, std::string_view what, ImVec4& c)
{
  FixedString<16> text;
  if (!r.convert(j, text, what))
  {
    return false;
  }
  const auto s = text.view();
  if ((s.size() != 7 and s.size() != 9) or s[0] != '#')
  {
    return r.fail("%.*s: expected \"#rrggbb\" or \"#rrggbbaa\"", static_cast<int>(what.size()), what.data());
  }
  float channels[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  for (std::size_t i = 0; i < (s.size() - 1) / 2; ++i)
  {
    const int hi = hex_digit(s[1 + 2 * i]);
    const int lo = hex_digit(s[2 + 2 * i]);
    if (hi < 0 or lo < 0)
    {
      return r.fail("%.*s: invalid color \"%s\"", static_cast<int>(what.size()), what.data(), text.c_str());
    }
    channels[i] = static_cast<float>(hi * 16 + lo) / 255.0F;
  }
  c = ImVec4{channels[0], channels[1], channels[2], channels[3]};
  return true;
}

const char* dir_name(ImGuiDir d)
{
  switch (d)
  {
  case ImGuiDir_Left:
    return "left";
  case ImGuiDir_Right:
    return "right";
  case ImGuiDir_Up:
    return "up";
  case ImGuiDir_Down:
    return "down";
  default:
    return "none";
  }
}

bool load_dir(json::Reader& r, const Json& j, std::string_view what, ImGuiDir& d)
{
  Name name;
  if (!r.convert(j, name, what))
  {
    return false;
  }
  for (const ImGuiDir candidate : {ImGuiDir_None, ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down})
  {
    if (name == std::string_view{dir_name(candidate)})
    {
      d = candidate;
      return true;
    }
  }
  return r.fail("%.*s: unknown direction '%s'", static_cast<int>(what.size()), what.data(), name.c_str());
}

}  // namespace

void apply(Preset preset, Profile& profile)
{
  profile.style = ImGuiStyle{};
  profile.font_scale = 1.0F;
  switch (preset)
  {
  case Preset::kLvl:
    ImGui::StyleColorsDark(&profile.style);
    profile.style.WindowRounding = 3.0F;
    profile.style.FrameRounding = 2.0F;
    profile.style.GrabRounding = 2.0F;
    break;
  case Preset::kDark:
    ImGui::StyleColorsDark(&profile.style);
    break;
  case Preset::kLight:
    ImGui::StyleColorsLight(&profile.style);
    break;
  case Preset::kClassic:
    ImGui::StyleColorsClassic(&profile.style);
    break;
  }
}

void activate(const Profile& profile)
{
  ImGui::GetStyle() = profile.style;
  ImGui::GetIO().FontGlobalScale = profile.font_scale;
}

Profile current() { return Profile{.style = ImGui::GetStyle(), .font_scale = ImGui::GetIO().FontGlobalScale}; }

void save(Json& j, const Profile& profile)
{
  const ImGuiStyle& s = profile.style;
  j = Json::object();
  json::write(j, "format", Name{kFormat});
  json::write(j, "version", kVersion);
  json::write(j, "font_scale", profile.font_scale);
  Json vars = Json::object();
#define LVL_SAVE_FLOAT(name) json::write(vars, #name, s.name);
#define LVL_SAVE_VEC2(name) json::write(vars, #name, std::array<float, 2>{s.name.x, s.name.y});
#define LVL_SAVE_BOOL(name) json::write(vars, #name, s.name);
#define LVL_SAVE_DIR(name) vars[#name] = JsonString{dir_name(s.name)};
  LVL_STYLE_FLOATS(LVL_SAVE_FLOAT)
  LVL_STYLE_VEC2S(LVL_SAVE_VEC2)
  LVL_STYLE_BOOLS(LVL_SAVE_BOOL)
  LVL_STYLE_DIRS(LVL_SAVE_DIR)
#undef LVL_SAVE_FLOAT
#undef LVL_SAVE_VEC2
#undef LVL_SAVE_BOOL
#undef LVL_SAVE_DIR
  j["vars"] = std::move(vars);
  Json colors = Json::object();
  for (int i = 0; i < ImGuiCol_COUNT; ++i)
  {
    save_color(colors[JsonString{ImGui::GetStyleColorName(i)}], s.Colors[i]);
  }
  j["colors"] = std::move(colors);
}

bool load(json::Reader& r, const Json& j, Profile& profile)
{
  Profile p = profile;
  ImGuiStyle& s = p.style;
  Name format;
  int version = 0;
  r.object(j, "style");
  r.get(j, "format", format);
  r.get_optional(j, "version", version);
  if (r.ok() and format != kFormat)
  {
    return r.fail("file holds '%s', expected 'lvl-style'", format.c_str());
  }
  r.get_optional(j, "font_scale", p.font_scale);
  if (const auto vars = j.find("vars"); r.ok() and vars != j.end() and r.object(*vars, "vars"))
  {
    std::array<float, 2> v2 = {};
#define LVL_LOAD_FLOAT(name) r.get_optional(*vars, #name, s.name);
#define LVL_LOAD_VEC2(name)                                                                                            \
  v2 = {s.name.x, s.name.y};                                                                                           \
  if (r.get_optional(*vars, #name, v2))                                                                                \
  {                                                                                                                    \
    s.name = ImVec2{v2[0], v2[1]};                                                                                     \
  }
#define LVL_LOAD_BOOL(name) r.get_optional(*vars, #name, s.name);
#define LVL_LOAD_DIR(name)                                                                                             \
  if (const auto itr = vars->find(#name); itr != vars->end())                                                          \
  {                                                                                                                    \
    load_dir(r, *itr, #name, s.name);                                                                                  \
  }
    LVL_STYLE_FLOATS(LVL_LOAD_FLOAT)
    LVL_STYLE_VEC2S(LVL_LOAD_VEC2)
    LVL_STYLE_BOOLS(LVL_LOAD_BOOL)
    LVL_STYLE_DIRS(LVL_LOAD_DIR)
#undef LVL_LOAD_FLOAT
#undef LVL_LOAD_VEC2
#undef LVL_LOAD_BOOL
#undef LVL_LOAD_DIR
  }
  if (const auto colors = j.find("colors"); r.ok() and colors != j.end() and r.object(*colors, "colors"))
  {
    for (int i = 0; i < ImGuiCol_COUNT and r.ok(); ++i)
    {
      const std::string_view name{ImGui::GetStyleColorName(i)};
      if (const auto itr = colors->find(name); itr != colors->end())
      {
        load_color(r, *itr, name, s.Colors[i]);
      }
    }
  }
  if (r.ok() and (p.font_scale < 0.25F or p.font_scale > 4.0F))
  {
    return r.fail("font_scale %g out of range [0.25, 4]", static_cast<double>(p.font_scale));
  }
  if (!r.ok())
  {
    return false;
  }
  profile = p;
  return true;
}

Status save_file(const char* path, const Profile& profile)
{
  // Make sure the profile's directory exists (e.g. ~/.config/lvl)
  std::error_code ec;
  if (const auto dir = std::filesystem::path{path}.parent_path(); !dir.empty())
  {
    std::filesystem::create_directories(dir, ec);
  }
  return json::write_file(path, [&profile](Json& j) { save(j, profile); });
}

Status load_file(const char* path, Profile& profile)
{
  return json::read_file(path, [&profile](const Json& j) {
    json::Reader r;
    load(r, j, profile);
    return r.status;
  });
}

const char* default_path()
{
  static Path path;
  if (!path.empty())
  {
    return path.c_str();
  }
  const char* xdg = kIsWeb ? nullptr : std::getenv("XDG_CONFIG_HOME");
  const char* home = kIsWeb ? nullptr : std::getenv("HOME");
  if (xdg != nullptr and xdg[0] != '\0')
  {
    path.assign(format<Path::capacity()>("%s/lvl/style.json", xdg));
  }
  else if (home != nullptr and home[0] != '\0')
  {
    path.assign(format<Path::capacity()>("%s/.config/lvl/style.json", home));
  }
  else
  {
    path.assign("lvl_style.json");
  }
  return path.c_str();
}

}  // namespace lvl::editor::style
