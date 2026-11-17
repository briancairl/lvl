/**
 * @copyright 2026-present Brian Cairl
 *
 * @file format.hpp
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdio>
#include <utility>

namespace lvl
{

/// C-string returned when <code>format</code> fails
inline constexpr const char* kFormatErrorString = "::lvl::format failed (formatted_len >= BufferLen)";

/**
 * @brief Formats a string into a thread-local static buffer and returns a pointer to it
 *
 * @param fmt_str  printf-style format specification string
 * @param fmt_args...  args used to replace format placeholders in \c fmt_str
 *
 * @retval fmt  formatted c-string buffer
 * @retval err  <code>kFormatErrorString</code> if the result would not fit
 *
 * @warning the returned buffer is overwritten by the next call (per thread and buffer size); copy
 *          it if it needs to be kept
 */
template <std::size_t BufferLen = 256UL, typename... FormatArgTs>
const char* format(const char* fmt_str, FormatArgTs&&... fmt_args)
{
  thread_local static char STATIC__fmt_buffer[BufferLen];
  int formatted_len = 0;
  if constexpr (sizeof...(FormatArgTs) == 0)
  {
    // No arguments: copy verbatim (a '%' in the text is not a format directive)
    formatted_len = std::snprintf(STATIC__fmt_buffer, sizeof(STATIC__fmt_buffer), "%s", fmt_str);
  }
  else
  {
    formatted_len =
      std::snprintf(STATIC__fmt_buffer, sizeof(STATIC__fmt_buffer), fmt_str, std::forward<FormatArgTs>(fmt_args)...);
  }
  if (formatted_len >= 0 and static_cast<std::size_t>(formatted_len) < BufferLen)
  {
    return STATIC__fmt_buffer;
  }
  return kFormatErrorString;
}

}  // namespace lvl
