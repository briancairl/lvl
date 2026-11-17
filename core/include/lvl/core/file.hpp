/**
 * @copyright 2026-present Brian Cairl
 *
 * @file file.hpp
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdio>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl::file
{

/// True if <code>path</code> names an existing regular file
[[nodiscard]] bool exists(const char* path);

/// Size of the file at <code>path</code>, in bytes (fails if it cannot be opened)
Status size(const char* path, std::size_t& size);

/**
 * @brief Reads a whole file into <code>arena</code>
 *
 * The data is null-terminated (one byte beyond <code>size</code>) for convenience with text.
 */
Status read(const char* path, memory::Arena& arena, char*& data, std::size_t& size);

/// Writes <code>size</code> bytes to <code>path</code>, via a temporary file renamed into place
Status write(const char* path, const void* data, std::size_t size);

/**
 * @brief Writes a file in pieces, atomically: data goes to "<path>.tmp", renamed over
 *        <code>path</code> by <code>commit()</code>. Destroying an uncommitted Writer removes the
 *        temporary file.
 */
class Writer
{
public:
  Writer() = default;
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  /// Opens the temporary file for <code>path</code>
  Status open(const char* path);

  /// Appends bytes (errors are reported by commit)
  void write(const void* data, std::size_t size);

  /// Appends text
  void write(std::string_view text) { write(text.data(), text.size()); }

  /// Closes the temporary file and renames it into place
  Status commit();

private:
  std::FILE* file_ = nullptr;
  bool failed_ = false;
  Path path_;
  Path tmp_;
};

/// Extension of <code>path</code> including the dot (e.g. ".json"), or "" if none
[[nodiscard]] const char* extension(const char* path);

}  // namespace lvl::file
