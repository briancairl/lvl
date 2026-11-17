/**
 * @copyright 2026-present Brian Cairl
 *
 * @file json.hpp
 *
 * JSON documents whose every allocation comes from an arena (as in gft): a thread installs a JSON
 * arena with <code>json::install</code>; each load/save opens a <code>json::Scope</code>, which rewinds
 * the arena when it ends. With the nlohmann patch in //third_party/patches, parsing and dumping do no
 * heap allocation at all. The state is thread-local, so parallel load/save workers each install
 * their own arena.
 *
 * Files are user input, so reading goes through <code>json::Reader</code>, which type-checks every
 * value and reports a Status rather than asserting (nlohmann's own type errors would abort, since
 * first-party code builds with -fno-exceptions).
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// JSON
#include <nlohmann/json.hpp>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl
{
namespace json
{

/// Makes <code>arena</code> the memory every Json on this thread allocates from
void install(memory::Arena& arena);

/// Stops using the installed arena (which stays owned by the caller). No Scope may be open.
void uninstall();

/// True if this thread has a JSON arena installed
[[nodiscard]] bool is_installed();

/// The installed arena; asserts that a Scope is open, since JSON memory is only valid inside one
[[nodiscard]] memory::Arena& active_arena();

/**
 * @brief A JSON load/save segment
 *
 * Everything JSON allocates while it is open is released when it ends. Scopes nest (strictly LIFO).
 * No Json created inside a Scope may outlive it.
 */
class Scope
{
public:
  Scope();
  ~Scope();

  Scope(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope& operator=(Scope&&) = delete;

private:
  std::size_t mark_;
  std::size_t depth_;
};

/**
 * @brief Installs a JSON arena on construction, uninstalls on destruction
 *
 * Used by worker threads, which each get their own JSON arena.
 */
class Installation
{
public:
  explicit Installation(memory::Arena& arena) { install(arena); }
  ~Installation() { uninstall(); }

  Installation(const Installation&) = delete;
  Installation& operator=(const Installation&) = delete;
};

/// Stateless allocator drawing from the thread's JSON arena
template <typename T> struct ArenaAllocator
{
  using value_type = T;

  constexpr ArenaAllocator() noexcept = default;
  template <typename U> constexpr ArenaAllocator([[maybe_unused]] const ArenaAllocator<U>& other) noexcept {}

  [[nodiscard]] T* allocate(std::size_t n) { return memory::allocate<T>(active_arena(), n); }

  /// Reclaims the block if it was the newest; everything else is reclaimed when the Scope ends
  void deallocate(T* p, std::size_t n) noexcept { memory::release_bytes(active_arena(), p, n * sizeof(T)); }

  template <typename U> constexpr bool operator==([[maybe_unused]] const ArenaAllocator<U>& other) const noexcept
  {
    return true;
  }
  template <typename U> constexpr bool operator!=([[maybe_unused]] const ArenaAllocator<U>& other) const noexcept
  {
    return false;
  }
};

}  // namespace json

/// String type of JSON values/keys (arena-allocated). Look keys up with a literal/string_view.
using JsonString = std::basic_string<char, std::char_traits<char>, json::ArenaAllocator<char>>;

/// nlohmann::json, with every allocation drawn from the thread's JSON arena
using Json = nlohmann::
  basic_json<std::map, std::vector, JsonString, bool, std::int64_t, std::uint64_t, double, json::ArenaAllocator>;

/**
 * @brief Names for the values of an enum, for (de)serializing it as a string
 *
 * Specialize with <code>static constexpr std::array<std::string_view, N> kNames</code>, indexed by the
 * enum's underlying value.
 */
template <typename EnumT> struct EnumNames;

template <typename EnumT, typename = void> struct has_enum_names : std::false_type
{};

template <typename EnumT> struct has_enum_names<EnumT, std::void_t<decltype(EnumNames<EnumT>::kNames)>> : std::true_type
{};

/// Name of an enum value (or "<invalid>")
template <typename EnumT> std::string_view enum_name(EnumT value)
{
  const auto i = static_cast<std::size_t>(value);
  return (i < EnumNames<EnumT>::kNames.size()) ? EnumNames<EnumT>::kNames[i] : std::string_view{"<invalid>"};
}

/// Parses an enum value from its name; returns false if there is no such name
template <typename EnumT> bool enum_from_name(std::string_view name, EnumT& value)
{
  for (std::size_t i = 0; i < EnumNames<EnumT>::kNames.size(); ++i)
  {
    if (EnumNames<EnumT>::kNames[i] == name)
    {
      value = static_cast<EnumT>(i);
      return true;
    }
  }
  return false;
}

namespace json
{

/// Parses <code>text</code> into <code>out</code> (inside an open Scope); fails on malformed input
Status parse(std::string_view text, Json& out);

/// Serializes <code>j</code> (inside an open Scope)
[[nodiscard]] JsonString dump(const Json& j, int indent = 2);

/// Reads and parses a JSON file, then calls <code>read(const Json&) -> Status</code>, all in one Scope
template <typename ReadFn> Status read_file(const char* path, ReadFn&& read);

/// Calls <code>write(Json&)</code> to build a document, then writes it to <code>path</code>, in one Scope
template <typename WriteFn> Status write_file(const char* path, WriteFn&& write, int indent = 2);

/**
 * @brief Type-checked reading of untrusted JSON
 *
 * The first failure is recorded in <code>status</code> (with the path of the offending value);
 * later reads become no-ops returning false, so loaders can read a whole structure and check once.
 *
 * Supported value types: bool, integers (range-checked), floating point, FixedString, enums with
 * EnumNames, std::array of supported types, and any type with an ADL
 * <code>bool load(json::Reader&, const Json&, T&)</code>.
 */
class Reader
{
public:
  Status status;

  [[nodiscard]] bool ok() const { return status.ok; }

  /// Records a failure (if none has been recorded yet); always returns false
  template <typename... Args> bool fail(const char* fmt_str, Args&&... args)
  {
    if (status.ok)
    {
      status = Status::error(fmt_str, std::forward<Args>(args)...);
    }
    return false;
  }

  /// Requires <code>j</code> to be an object
  bool object(const Json& j, std::string_view what)
  {
    return ok() and (j.is_object() or fail("'%.*s' must be an object", static_cast<int>(what.size()), what.data()));
  }

  /// Requires <code>j</code> to be an array
  bool array(const Json& j, std::string_view what)
  {
    return ok() and (j.is_array() or fail("'%.*s' must be an array", static_cast<int>(what.size()), what.data()));
  }

  /// Reads required member <code>key</code> of object <code>j</code>
  template <typename T> bool get(const Json& j, std::string_view key, T& value)
  {
    if (!ok())
    {
      return false;
    }
    if (!j.is_object())
    {
      return fail("expected an object containing '%.*s'", static_cast<int>(key.size()), key.data());
    }
    const auto itr = j.find(key);
    if (itr == j.end())
    {
      return fail("missing required key '%.*s'", static_cast<int>(key.size()), key.data());
    }
    return convert(*itr, value, key);
  }

  /// Reads optional member <code>key</code>; leaves <code>value</code> unchanged if absent
  template <typename T> bool get_optional(const Json& j, std::string_view key, T& value)
  {
    if (!ok() or !j.is_object())
    {
      return ok();
    }
    const auto itr = j.find(key);
    return (itr == j.end()) or convert(*itr, value, key);
  }

  /// Converts <code>j</code> itself into <code>value</code>; <code>what</code> names it in errors
  template <typename T> bool convert(const Json& j, T& value, std::string_view what);

private:
  template <typename T> bool convert_impl(const Json& j, T& value, std::string_view what);
};

/// Writes <code>value</code> into <code>j</code> (supports the same types as Reader)
template <typename T> void write(Json& j, const T& value);

/// Writes <code>value</code> as member <code>key</code> of object <code>j</code>
template <typename T> void write(Json& j, std::string_view key, const T& value)
{
  Json element;
  write(element, value);
  j[JsonString{key}] = std::move(element);
}

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

template <typename T> struct is_fixed_string : std::false_type
{};

template <std::size_t N> struct is_fixed_string<FixedString<N>> : std::true_type
{};

template <typename T> struct is_std_array : std::false_type
{};

template <typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type
{};

template <typename T> bool Reader::convert(const Json& j, T& value, std::string_view what)
{
  if (!ok())
  {
    return false;
  }
  if (!convert_impl(j, value, what))
  {
    // Prefix the failing value's key, to give a path to where the error was found
    if (!status.ok and what.size() > 0)
    {
      FixedString<512> message;
      message.assign(format<512UL>("%.*s: %s", static_cast<int>(what.size()), what.data(), status.message.c_str()));
      status.message = message;
    }
    return false;
  }
  return true;
}

template <typename T> bool Reader::convert_impl(const Json& j, T& value, [[maybe_unused]] std::string_view what)
{
  if constexpr (std::is_same_v<T, bool>)
  {
    if (!j.is_boolean())
    {
      return fail("expected a boolean");
    }
    value = j.template get<bool>();
    return true;
  }
  else if constexpr (std::is_integral_v<T>)
  {
    if (!j.is_number_integer())
    {
      return fail("expected an integer");
    }
    if (j.is_number_unsigned())
    {
      const auto v = j.template get<std::uint64_t>();
      if (v > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
      {
        return fail("integer %llu out of range", static_cast<unsigned long long>(v));
      }
      value = static_cast<T>(v);
      return true;
    }
    const auto v = j.template get<std::int64_t>();
    if (
      v < static_cast<std::int64_t>(std::numeric_limits<T>::lowest()) or
      (v > 0 and static_cast<std::uint64_t>(v) > static_cast<std::uint64_t>(std::numeric_limits<T>::max())))
    {
      return fail("integer %lld out of range", static_cast<long long>(v));
    }
    value = static_cast<T>(v);
    return true;
  }
  else if constexpr (std::is_floating_point_v<T>)
  {
    if (!j.is_number())
    {
      return fail("expected a number");
    }
    value = j.template get<T>();
    return true;
  }
  else if constexpr (is_fixed_string<T>::value)
  {
    if (!j.is_string())
    {
      return fail("expected a string");
    }
    const auto& s = j.template get_ref<const JsonString&>();
    if (!value.assign(std::string_view{s.data(), s.size()}))
    {
      return fail("string longer than %zu characters", T::kMaxLength);
    }
    return true;
  }
  else if constexpr (std::is_enum_v<T> and has_enum_names<T>::value)
  {
    if (!j.is_string())
    {
      return fail("expected a string");
    }
    const auto& s = j.template get_ref<const JsonString&>();
    if (!enum_from_name(std::string_view{s.data(), s.size()}, value))
    {
      return fail("unknown value '%s'", s.c_str());
    }
    return true;
  }
  else if constexpr (is_std_array<T>::value)
  {
    if (!j.is_array() or j.size() != value.size())
    {
      return fail("expected an array of %zu elements", value.size());
    }
    for (std::size_t i = 0; i < value.size(); ++i)
    {
      if (!convert(j[i], value[i], format<32UL>("[%zu]", i)))
      {
        return false;
      }
    }
    return true;
  }
  else
  {
    return load(*this, j, value);
  }
}

template <typename T> void write(Json& j, const T& value)
{
  if constexpr (std::is_same_v<T, bool> or std::is_arithmetic_v<T>)
  {
    j = value;
  }
  else if constexpr (is_fixed_string<T>::value)
  {
    j = JsonString{value.view()};
  }
  else if constexpr (std::is_enum_v<T> and has_enum_names<T>::value)
  {
    j = JsonString{enum_name(value)};
  }
  else if constexpr (is_std_array<T>::value)
  {
    j = Json::array();
    for (const auto& element : value)
    {
      Json e;
      write(e, element);
      j.push_back(std::move(e));
    }
  }
  else
  {
    save(j, value);
  }
}

namespace detail
{
/// Reads a whole file into the active JSON arena
Status read_text(const char* path, std::string_view& text);
/// Writes text to a file (via a temporary file + rename)
Status write_text(const char* path, std::string_view text);
}  // namespace detail

template <typename ReadFn> Status read_file(const char* path, ReadFn&& read)
{
  Scope scope;
  std::string_view text;
  LVL_RETURN_IF_ERROR(detail::read_text(path, text));
  Json j;
  if (Status s = parse(text, j); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  static_assert(
    std::is_same_v<std::invoke_result_t<ReadFn, const Json&>, Status>, "read_file callback must return a Status");
  Status s = read(static_cast<const Json&>(j));
  if (!s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  return s;
}

template <typename WriteFn> Status write_file(const char* path, WriteFn&& write, int indent)
{
  Scope scope;
  Json j;
  write(j);
  const JsonString text = dump(j, indent);
  return detail::write_text(path, std::string_view{text.data(), text.size()});
}

}  // namespace json
}  // namespace lvl
