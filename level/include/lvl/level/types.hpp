/**
 * @copyright 2026-present Brian Cairl
 *
 * @file types.hpp
 *
 * Small value types shared by every part of a level: tile references, collision masks and the
 * catalogs (semantic types, materials, seasons) that per-cell ids refer to.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/image.hpp"
#include "lvl/core/json.hpp"
#include "lvl/level/grid.hpp"

namespace lvl::image
{

/// JSON: "#rrggbbaa" (or "#rrggbb", opaque)
bool load(json::Reader& r, const Json& j, Rgba& c);
void save(Json& j, const Rgba& c);

template <typename S> void serialize(S& s, Rgba& c)
{
  s.value1b(c.r);
  s.value1b(c.g);
  s.value1b(c.b);
  s.value1b(c.a);
}

}  // namespace lvl::image

namespace lvl::level
{

using Color = image::Rgba;

/// Index of a tileset within a level
using TileSetId = std::uint16_t;

/// Index of a tile within a tileset
using TileIndex = std::uint16_t;

/// Index into a level's semantic-type catalog (0 = none)
using SemanticId = std::uint16_t;

/// Index into a level's material catalog (0 = none)
using MaterialId = std::uint16_t;

/// Index into a level's season list
using SeasonId = std::uint8_t;

inline constexpr TileSetId kNoTileSet = std::numeric_limits<TileSetId>::max();
inline constexpr SemanticId kNoSemantic = 0;
inline constexpr MaterialId kNoMaterial = 0;

/// A tile from one of a level's tilesets (or empty)
struct TileRef
{
  TileSetId tileset = kNoTileSet;
  TileIndex tile = 0;

  [[nodiscard]] constexpr bool empty() const { return tileset == kNoTileSet; }

  friend constexpr bool operator==(const TileRef& lhs, const TileRef& rhs)
  {
    return lhs.tileset == rhs.tileset and lhs.tile == rhs.tile;
  }
  friend constexpr bool operator!=(const TileRef& lhs, const TileRef& rhs) { return !(lhs == rhs); }
};

inline constexpr TileRef kEmptyTile = TileRef{};

/// Per-cell collision flags. The low four bits mark solid quadrants of a cell.
namespace collision
{
inline constexpr std::uint8_t kNone = 0;
inline constexpr std::uint8_t kNorthWest = 1U << 0U;
inline constexpr std::uint8_t kNorthEast = 1U << 1U;
inline constexpr std::uint8_t kSouthWest = 1U << 2U;
inline constexpr std::uint8_t kSouthEast = 1U << 3U;
inline constexpr std::uint8_t kSolid = kNorthWest | kNorthEast | kSouthWest | kSouthEast;
/// Passable from below/one side only
inline constexpr std::uint8_t kPlatform = 1U << 4U;
/// Not solid, but raises an event when entered
inline constexpr std::uint8_t kTrigger = 1U << 5U;
/// Blocks projectiles/sight only
inline constexpr std::uint8_t kSightBlocker = 1U << 6U;

inline constexpr std::array<std::string_view, 7> kFlagNames =
  {"nw", "ne", "sw", "se", "platform", "trigger", "sight_blocker"};
}  // namespace collision

/// An entry of the semantic-type catalog ("grass", "road", "cliff", ...)
struct SemanticType
{
  Name name;
  Color color = {200, 200, 200, 255};
};

/// An entry of the material catalog ("dirt", "stone", "snow", ...)
struct Material
{
  Name name;
  Color color = {150, 120, 90, 255};
  /// Movement friction multiplier
  float friction = 1.0F;
  /// Movement speed multiplier
  float speed = 1.0F;
  /// Footstep / impact sound key for the game
  Name sound;
};

/**
 * @brief One season of a level's year, and its climate
 *
 * A level has one or more seasons, cycled in order. Tilesets can provide per-season alternate tiles
 * (and materials) keyed by season name.
 */
struct Season
{
  Name name;
  /// Length of the season, in game days
  std::uint32_t duration_days = 30;
  /// Mean temperature, degrees C
  float temperature = 15.0F;
  /// Mean precipitation, mm/day
  float precipitation = 2.0F;
  /// Relative humidity [0, 1]
  float humidity = 0.5F;
  /// Mean wind speed, m/s
  float wind = 3.0F;
  /// Precipitation falls as snow below this temperature
  float snow_below = 0.0F;
  /// Color tint multiplied over the map when previewing this season
  Color tint = {255, 255, 255, 255};
};

// JSON (de)serialization of the above (see lvl/core/json.hpp for the conventions)
bool load(json::Reader& r, const Json& j, Extents& e);
void save(Json& j, const Extents& e);
bool load(json::Reader& r, const Json& j, SemanticType& v);
void save(Json& j, const SemanticType& v);
bool load(json::Reader& r, const Json& j, Material& v);
void save(Json& j, const Material& v);
bool load(json::Reader& r, const Json& j, Season& v);
void save(Json& j, const Season& v);

// Binary serialization
template <typename S> void serialize(S& s, Extents& e)
{
  s.value4b(e.width);
  s.value4b(e.height);
}

template <typename S> void serialize(S& s, TileRef& t)
{
  s.value2b(t.tileset);
  s.value2b(t.tile);
}

template <typename S> void serialize(S& s, SemanticType& v)
{
  s.object(v.name);
  s.object(v.color);
}

template <typename S> void serialize(S& s, Material& v)
{
  s.object(v.name);
  s.object(v.color);
  s.value4b(v.friction);
  s.value4b(v.speed);
  s.object(v.sound);
}

template <typename S> void serialize(S& s, Season& v)
{
  s.object(v.name);
  s.value4b(v.duration_days);
  s.value4b(v.temperature);
  s.value4b(v.precipitation);
  s.value4b(v.humidity);
  s.value4b(v.wind);
  s.value4b(v.snow_below);
  s.object(v.tint);
}

}  // namespace lvl::level

template <> struct lvl::EnumNames<lvl::level::Direction>
{
  static constexpr std::array<std::string_view, 4> kNames = {"north", "east", "south", "west"};
};
