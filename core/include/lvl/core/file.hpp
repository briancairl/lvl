/**
 * @copyright 2026-present Brian Cairl
 *
 * @file file.hpp
 */
#pragma once

// C++ Standard Library
#include <cstddef>

// LVL
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

/// Extension of <code>path</code> including the dot (e.g. ".json"), or "" if none
[[nodiscard]] const char* extension(const char* path);

}  // namespace lvl::file
