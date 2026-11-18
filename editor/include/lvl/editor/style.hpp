/**
 * @copyright 2026-present Brian Cairl
 *
 * @file style.hpp
 *
 * UI style profiles: an ImGuiStyle plus a global font scale, saved/loaded as JSON
 * ("format": "lvl-style"). Every style variable is written by its ImGuiStyle field name and every
 * color by its ImGuiCol name, as "#rrggbbaa". Loading is partial: keys absent from a file keep their
 * current values, so hand-written profiles may set just a few things.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <string_view>

// Dear ImGui
#include <imgui.h>

// LVL
#include "lvl/core/json.hpp"
#include "lvl/core/status.hpp"

namespace lvl::editor::style
{

/// Built-in starting points
enum class Preset : std::uint8_t
{
  /// ImGui dark colors with lvl's rounding
  kLvl,
  kDark,
  kLight,
  kClassic,
};

inline constexpr std::size_t kPresetCount = 4;

struct Profile
{
  ImGuiStyle style;
  /// ImGuiIO::FontGlobalScale
  float font_scale = 1.0F;
};

/// Resets <code>profile</code> to a preset
void apply(Preset preset, Profile& profile);

/// Makes <code>profile</code> the current ImGui style (requires an ImGui context)
void activate(const Profile& profile);

/// The current ImGui style (requires an ImGui context)
Profile current();

void save(Json& j, const Profile& profile);
bool load(json::Reader& r, const Json& j, Profile& profile);

/// Writes a style profile file
Status save_file(const char* path, const Profile& profile);

/// Reads a style profile file into <code>profile</code> (left unchanged on failure)
Status load_file(const char* path, Profile& profile);

/// Where the default profile lives: config_path("style.json") (see lvl/editor/config.hpp)
const char* default_path();

}  // namespace lvl::editor::style

template <> struct lvl::EnumNames<lvl::editor::style::Preset>
{
  static constexpr std::array<std::string_view, 4> kNames = {"lvl", "dark", "light", "classic"};
};
