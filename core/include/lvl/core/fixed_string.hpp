/**
 * @copyright 2026-present Brian Cairl
 *
 * @file fixed_string.hpp
 */
#pragma once

// C++ Standard Library
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ostream>
#include <string_view>

namespace lvl
{

/**
 * @brief Null-terminated string stored inline, with a fixed capacity
 *
 * Used for names and paths in level data so documents never touch the heap. Assigning a longer
 * string truncates it (and <code>assign</code> reports that). The raw buffer can be handed directly to
 * <code>ImGui::InputText</code>.
 */
template <std::size_t N> struct FixedString
{
  static_assert(N > 1, "FixedString needs room for at least one character and a terminator");

  /// Maximum number of characters (excluding the terminator)
  static constexpr std::size_t kMaxLength = N - 1;

  char data[N] = {};

  constexpr FixedString() = default;

  FixedString(std::string_view sv) { assign(sv); }  // NOLINT: implicit by design

  FixedString(const char* s) : FixedString{std::string_view{s}} {}  // NOLINT: implicit by design

  /// Replaces the contents; returns false if <code>sv</code> was truncated
  bool assign(std::string_view sv)
  {
    const std::size_t n = std::min(sv.size(), kMaxLength);
    std::memcpy(data, sv.data(), n);
    std::memset(data + n, 0, N - n);
    return n == sv.size();
  }

  void clear() { std::memset(data, 0, N); }

  [[nodiscard]] std::size_t size() const { return std::strlen(data); }
  [[nodiscard]] bool empty() const { return data[0] == '\0'; }
  [[nodiscard]] static constexpr std::size_t capacity() { return N; }

  [[nodiscard]] const char* c_str() const { return data; }
  [[nodiscard]] std::string_view view() const { return std::string_view{data}; }
  operator std::string_view() const { return view(); }  // NOLINT: implicit by design

  friend bool operator==(const FixedString& lhs, const FixedString& rhs) { return lhs.view() == rhs.view(); }
  friend bool operator==(const FixedString& lhs, std::string_view rhs) { return lhs.view() == rhs; }
  friend bool operator<(const FixedString& lhs, const FixedString& rhs) { return lhs.view() < rhs.view(); }
  friend std::ostream& operator<<(std::ostream& os, const FixedString& s) { return os << s.data; }
};

/// Names of things (layers, tiles, seasons, portals, ...)
using Name = FixedString<64>;

/// File system paths
using Path = FixedString<512>;

}  // namespace lvl
