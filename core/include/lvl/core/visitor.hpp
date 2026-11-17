/**
 * @copyright 2026-present Brian Cairl
 *
 * @file visitor.hpp
 */
#pragma once

// C++ Standard Library
#include <type_traits>
#include <utility>
#include <variant>

namespace lvl
{

/// Lambda-set visitor: <code>std::visit(overloaded{[](A&){...}, [](B&){...}}, v)</code>
template <class... Ts> struct overloaded : Ts...
{
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

/// Visits <code>value</code> with <code>fn</code> only for alternatives <code>fn</code> accepts
template <typename F, typename V> void visit_with_default(F&& fn, V&& value)
{
  std::visit(
    [&fn](auto&& alternative) {
      if constexpr (std::is_invocable_v<F, decltype(alternative)>)
      {
        fn(std::forward<decltype(alternative)>(alternative));
      }
    },
    std::forward<V>(value));
}

/// Index of type <code>T</code> within <code>std::variant<Ts...></code>
template <typename T, typename VariantT> struct variant_index;

template <typename T, typename... Ts> struct variant_index<T, std::variant<Ts...>>
{
  static constexpr std::size_t value = [] {
    constexpr bool matches[] = {std::is_same_v<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i)
    {
      if (matches[i])
      {
        return i;
      }
    }
    return sizeof...(Ts);
  }();
};

template <typename T, typename VariantT>
inline constexpr std::size_t variant_index_v = variant_index<T, VariantT>::value;

}  // namespace lvl
