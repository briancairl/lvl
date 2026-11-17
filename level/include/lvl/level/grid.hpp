/**
 * @copyright 2026-present Brian Cairl
 *
 * @file grid.hpp
 */
#pragma once

// C++ Standard Library
#include <algorithm>
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/core/memory.hpp"

namespace lvl::level
{

/// Integer cell coordinate
struct Cell
{
  int x = 0;
  int y = 0;

  friend constexpr bool operator==(const Cell& lhs, const Cell& rhs) { return lhs.x == rhs.x and lhs.y == rhs.y; }
  friend constexpr bool operator!=(const Cell& lhs, const Cell& rhs) { return !(lhs == rhs); }
};

/// Grid dimensions, in cells
struct Extents
{
  int width = 0;
  int height = 0;

  [[nodiscard]] constexpr std::size_t count() const
  {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  }

  [[nodiscard]] constexpr bool contains(int x, int y) const { return x >= 0 and y >= 0 and x < width and y < height; }
  [[nodiscard]] constexpr bool contains(Cell c) const { return contains(c.x, c.y); }

  /// Row-major index of a cell
  [[nodiscard]] constexpr std::size_t index(int x, int y) const
  {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
  }
  [[nodiscard]] constexpr std::size_t index(Cell c) const { return index(c.x, c.y); }

  [[nodiscard]] constexpr Cell cell(std::size_t index) const
  {
    return Cell{
      static_cast<int>(index % static_cast<std::size_t>(width)),
      static_cast<int>(index / static_cast<std::size_t>(width))};
  }

  friend constexpr bool operator==(const Extents& lhs, const Extents& rhs)
  {
    return lhs.width == rhs.width and lhs.height == rhs.height;
  }
  friend constexpr bool operator!=(const Extents& lhs, const Extents& rhs) { return !(lhs == rhs); }
};

/// The four grid directions, in the order WFC adjacency rules are stored
enum class Direction : std::uint8_t
{
  kNorth,
  kEast,
  kSouth,
  kWest,
};

inline constexpr Direction kDirections[] = {Direction::kNorth, Direction::kEast, Direction::kSouth, Direction::kWest};

constexpr Direction opposite(Direction d) { return static_cast<Direction>((static_cast<int>(d) + 2) % 4); }

constexpr Cell step(Cell c, Direction d)
{
  switch (d)
  {
  case Direction::kNorth:
    return Cell{c.x, c.y - 1};
  case Direction::kEast:
    return Cell{c.x + 1, c.y};
  case Direction::kSouth:
    return Cell{c.x, c.y + 1};
  case Direction::kWest:
    return Cell{c.x - 1, c.y};
  }
  return c;
}

/**
 * @brief A dense, row-major plane of per-cell values (one data plane of a layer)
 *
 * Storage is an arena-backed vector (the level document's arena).
 */
template <typename T> struct Grid
{
  using value_type = T;

  Extents extents;
  memory::Vector<T> values;

  Grid() = default;
  explicit Grid(memory::Arena& arena) : values{memory::ArenaAllocator<T>{arena}} {}

  [[nodiscard]] int width() const { return extents.width; }
  [[nodiscard]] int height() const { return extents.height; }
  [[nodiscard]] std::size_t size() const { return values.size(); }

  [[nodiscard]] T& at(int x, int y)
  {
    LVL_DEBUG_ASSERT(extents.contains(x, y));
    return values[extents.index(x, y)];
  }
  [[nodiscard]] const T& at(int x, int y) const
  {
    LVL_DEBUG_ASSERT(extents.contains(x, y));
    return values[extents.index(x, y)];
  }
  [[nodiscard]] T& at(Cell c) { return at(c.x, c.y); }
  [[nodiscard]] const T& at(Cell c) const { return at(c.x, c.y); }

  /// Value at (x, y), or <code>fallback</code> outside the grid
  [[nodiscard]] T get(int x, int y, T fallback = T{}) const
  {
    return extents.contains(x, y) ? values[extents.index(x, y)] : fallback;
  }

  /// (Re)initializes to <code>e</code>, every cell <code>fill</code>
  void assign(Extents e, const T& fill)
  {
    extents = e;
    values.assign(e.count(), fill);
  }

  void fill(const T& value) { std::fill(values.begin(), values.end(), value); }

  /// Resizes to <code>e</code>, keeping overlapping cells (anchored at the top-left) and filling the rest
  void resize(Extents e, const T& fill_value)
  {
    if (e == extents)
    {
      return;
    }
    memory::Vector<T> next(e.count(), fill_value, values.get_allocator());
    const int w = std::min(e.width, extents.width);
    const int h = std::min(e.height, extents.height);
    for (int y = 0; y < h; ++y)
    {
      for (int x = 0; x < w; ++x)
      {
        next[e.index(x, y)] = values[extents.index(x, y)];
      }
    }
    values = std::move(next);
    extents = e;
  }
};

}  // namespace lvl::level
