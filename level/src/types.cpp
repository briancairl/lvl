/**
 * @copyright 2026-present Brian Cairl
 *
 * @file types.cpp
 */

// C++ Standard Library
#include <cstdio>

// LVL
#include "lvl/level/types.hpp"

namespace lvl::level
{
namespace
{

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

}  // namespace
}  // namespace lvl::level

namespace lvl::image
{

bool load(json::Reader& r, const Json& j, Rgba& c)
{
  // "#rrggbb" or "#rrggbbaa"
  FixedString<16> text;
  if (!r.convert(j, text, ""))
  {
    return false;
  }
  const auto s = text.view();
  if ((s.size() != 7 and s.size() != 9) or s[0] != '#')
  {
    return r.fail("expected a color \"#rrggbb\" or \"#rrggbbaa\", got \"%s\"", text.c_str());
  }
  std::uint8_t channels[4] = {0, 0, 0, 255};
  for (std::size_t i = 0; i < (s.size() - 1) / 2; ++i)
  {
    const int hi = level::hex_digit(s[1 + 2 * i]);
    const int lo = level::hex_digit(s[2 + 2 * i]);
    if (hi < 0 or lo < 0)
    {
      return r.fail("invalid color \"%s\"", text.c_str());
    }
    channels[i] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  c = Rgba{channels[0], channels[1], channels[2], channels[3]};
  return true;
}

void save(Json& j, const Rgba& c)
{
  char text[16];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
  j = JsonString{text};
}

}  // namespace lvl::image

namespace lvl::level
{

bool load(json::Reader& r, const Json& j, Extents& e)
{
  std::array<int, 2> wh = {};
  if (!r.convert(j, wh, ""))
  {
    return false;
  }
  if (wh[0] <= 0 or wh[1] <= 0 or wh[0] > 16384 or wh[1] > 16384)
  {
    return r.fail("grid size must be within [1, 16384], got %dx%d", wh[0], wh[1]);
  }
  e = Extents{wh[0], wh[1]};
  return true;
}

void save(Json& j, const Extents& e) { json::write(j, std::array<int, 2>{e.width, e.height}); }

bool load(json::Reader& r, const Json& j, SemanticType& v)
{
  r.object(j, "semantic");
  r.get(j, "name", v.name);
  r.get_optional(j, "color", v.color);
  return r.ok();
}

void save(Json& j, const SemanticType& v)
{
  j = Json::object();
  json::write(j, "name", v.name);
  json::write(j, "color", v.color);
}

bool load(json::Reader& r, const Json& j, Material& v)
{
  r.object(j, "material");
  r.get(j, "name", v.name);
  r.get_optional(j, "color", v.color);
  r.get_optional(j, "friction", v.friction);
  r.get_optional(j, "speed", v.speed);
  r.get_optional(j, "sound", v.sound);
  return r.ok();
}

void save(Json& j, const Material& v)
{
  j = Json::object();
  json::write(j, "name", v.name);
  json::write(j, "color", v.color);
  json::write(j, "friction", v.friction);
  json::write(j, "speed", v.speed);
  json::write(j, "sound", v.sound);
}

bool load(json::Reader& r, const Json& j, Season& v)
{
  r.object(j, "season");
  r.get(j, "name", v.name);
  r.get_optional(j, "duration_days", v.duration_days);
  r.get_optional(j, "temperature", v.temperature);
  r.get_optional(j, "precipitation", v.precipitation);
  r.get_optional(j, "humidity", v.humidity);
  r.get_optional(j, "wind", v.wind);
  r.get_optional(j, "snow_below", v.snow_below);
  r.get_optional(j, "tint", v.tint);
  return r.ok();
}

void save(Json& j, const Season& v)
{
  j = Json::object();
  json::write(j, "name", v.name);
  json::write(j, "duration_days", v.duration_days);
  json::write(j, "temperature", v.temperature);
  json::write(j, "precipitation", v.precipitation);
  json::write(j, "humidity", v.humidity);
  json::write(j, "wind", v.wind);
  json::write(j, "snow_below", v.snow_below);
  json::write(j, "tint", v.tint);
}

}  // namespace lvl::level
