/**
 * @copyright 2026-present Brian Cairl
 *
 * @file config.cpp
 */

// C++ Standard Library
#include <cstdlib>

// LVL
#include "lvl/core/build.hpp"
#include "lvl/core/format.hpp"
#include "lvl/editor/config.hpp"

namespace lvl::editor
{

Path config_path(std::string_view name)
{
  const char* xdg = kIsWeb ? nullptr : std::getenv("XDG_CONFIG_HOME");
  const char* home = kIsWeb ? nullptr : std::getenv("HOME");
  const int n = static_cast<int>(name.size());
  if (xdg != nullptr and xdg[0] != '\0')
  {
    return Path{format<Path::capacity()>("%s/lvl/%.*s", xdg, n, name.data())};
  }
  if (home != nullptr and home[0] != '\0')
  {
    return Path{format<Path::capacity()>("%s/.config/lvl/%.*s", home, n, name.data())};
  }
  return Path{format<Path::capacity()>("lvl_%.*s", n, name.data())};
}

}  // namespace lvl::editor
