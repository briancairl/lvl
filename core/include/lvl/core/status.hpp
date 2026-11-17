/**
 * @copyright 2026-present Brian Cairl
 *
 * @file status.hpp
 */
#pragma once

// C++ Standard Library
#include <ostream>
#include <utility>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/format.hpp"

namespace lvl
{

/**
 * @brief Outcome of an operation which can fail for reasons outside the program's control
 *
 * A missing or malformed file picked by the user is not a bug, so it is reported with a Status
 * (logged and shown in the UI) rather than an assert. Invariant violations still use LVL_ASSERT*.
 */
struct [[nodiscard]] Status
{
  /// True on success
  bool ok = true;
  /// Human-readable description of the failure (empty on success)
  FixedString<512> message = {};

  static Status okay() { return Status{}; }

  template <typename... FormatArgTs> static Status error(const char* fmt_str, FormatArgTs&&... fmt_args)
  {
    Status s;
    s.ok = false;
    s.message.assign(format<512UL>(fmt_str, std::forward<FormatArgTs>(fmt_args)...));
    return s;
  }

  constexpr explicit operator bool() const { return ok; }

  friend std::ostream& operator<<(std::ostream& os, const Status& s)
  {
    return s.ok ? (os << "OK") : (os << "ERROR: " << s.message);
  }
};

/// Returns from the calling function if <code>expr</code> produced a failed Status
#define LVL_RETURN_IF_ERROR(expr)                                                                                      \
  if (::lvl::Status __lvl_status = (expr); !__lvl_status.ok)                                                           \
  {                                                                                                                    \
    return __lvl_status;                                                                                               \
  }

}  // namespace lvl
