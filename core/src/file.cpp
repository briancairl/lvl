/**
 * @copyright 2026-present Brian Cairl
 *
 * @file file.cpp
 */

// C++ Standard Library
#include <cerrno>
#include <cstdio>
#include <cstring>

// POSIX
#include <sys/stat.h>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/logging.hpp"

namespace lvl::file
{

bool exists(const char* path)
{
  struct stat st;
  return (::stat(path, &st) == 0) and S_ISREG(st.st_mode);
}

Status size(const char* path, std::size_t& size)
{
  struct stat st;
  if (::stat(path, &st) != 0 or !S_ISREG(st.st_mode))
  {
    return Status::error("cannot open '%s'", path);
  }
  size = static_cast<std::size_t>(st.st_size);
  return Status::okay();
}

Status read(const char* path, memory::Arena& arena, char*& data, std::size_t& size)
{
  LVL_RETURN_IF_ERROR(file::size(path, size));
  if (size + 1 > memory::available(arena))
  {
    return Status::error(
      "'%s' (%zu bytes) does not fit in arena '%s' (%zu bytes available)",
      path,
      size,
      arena.label,
      memory::available(arena));
  }
  std::FILE* const f = std::fopen(path, "rb");
  if (f == nullptr)
  {
    return Status::error("cannot open '%s': %s", path, std::strerror(errno));
  }
  data = memory::allocate<char>(arena, size + 1);
  const std::size_t read_size = std::fread(data, 1, size, f);
  std::fclose(f);
  data[size] = '\0';
  if (read_size != size)
  {
    return Status::error("failed to read '%s'", path);
  }
  return Status::okay();
}

Status write(const char* path, const void* data, std::size_t size)
{
  Path tmp;
  if (!tmp.assign(format<Path::capacity()>("%s.tmp", path)))
  {
    return Status::error("path too long: '%s'", path);
  }
  std::FILE* const f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr)
  {
    return Status::error("cannot open '%s' for writing: %s", tmp.c_str(), std::strerror(errno));
  }
  const std::size_t written = (size == 0) ? 0 : std::fwrite(data, 1, size, f);
  const bool closed = (std::fclose(f) == 0);
  if (written != size or !closed)
  {
    std::remove(tmp.c_str());
    return Status::error("failed to write '%s'", path);
  }
  if (std::rename(tmp.c_str(), path) != 0)
  {
    std::remove(tmp.c_str());
    return Status::error("cannot replace '%s': %s", path, std::strerror(errno));
  }
  return Status::okay();
}

const char* extension(const char* path)
{
  const char* const dot = std::strrchr(path, '.');
  const char* const slash = std::strrchr(path, '/');
  if (dot == nullptr or (slash != nullptr and dot < slash))
  {
    return "";
  }
  return dot;
}

}  // namespace lvl::file
