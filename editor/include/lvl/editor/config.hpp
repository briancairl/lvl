/**
 * @copyright 2026-present Brian Cairl
 *
 * @file config.hpp
 *
 * Where the editor keeps per-user settings files (style profile, file dialog bookmarks).
 */
#pragma once

// C++ Standard Library
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"

namespace lvl::editor
{

/**
 * @brief Path of a per-user settings file
 *
 * $XDG_CONFIG_HOME/lvl/<name>, else ~/.config/lvl/<name>; in the browser (or without a home
 * directory), lvl_<name> in the working directory.
 */
Path config_path(std::string_view name);

}  // namespace lvl::editor
