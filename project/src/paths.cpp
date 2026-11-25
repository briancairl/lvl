/**
 * @copyright 2026-present Brian Cairl
 *
 * @file paths.cpp
 *
 * Path composition is done by hand on FixedString, so none of it touches the heap. The one place
 * that needs <code>std::filesystem</code> is <code>ensure_tree</code>, for create_directories, as in
 * editor/src/config.cpp; copying goes through file::read/file::write and an arena.
 */

// C++ Standard Library
#include <array>
#include <cstring>
#include <filesystem>
#include <system_error>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/format.hpp"
#include "lvl/project/paths.hpp"

namespace lvl::project
{
namespace
{

constexpr std::array<std::string_view, kFolderCount> kFolderPaths =
  {"", "levels", "tilesets", "rules", "paths", "assets/atlases", "assets/images", "artifacts"};

constexpr std::array<std::string_view, kFolderCount> kFolderLabels =
  {"Project", "Levels", "Tilesets", "Rules", "Path graphs", "Atlases", "Images", "Artifacts"};

constexpr bool is_separator(char c) { return c == '/' or c == '\\'; }

/// <code>path</code> without trailing separators ("a/b/" -> "a/b"; "/" stays "/")
std::string_view without_trailing_separator(std::string_view path)
{
  while (path.size() > 1 and is_separator(path.back()))
  {
    path.remove_suffix(1);
  }
  return path;
}

/// Copies <code>sv</code> into <code>out</code>, failing rather than truncating
Status assign_checked(std::string_view sv, Path& out)
{
  if (sv.size() > Path::kMaxLength)
  {
    return Status::error("path is too long (%zu characters, at most %zu)", sv.size(), Path::kMaxLength);
  }
  out.assign(sv);
  return Status::okay();
}

/// True if the two files hold the same bytes (false when either cannot be read)
bool same_contents(const char* a, const char* b, memory::Arena& scratch)
{
  std::size_t size_a = 0;
  std::size_t size_b = 0;
  if (!file::size(a, size_a).ok or !file::size(b, size_b).ok or size_a != size_b)
  {
    return false;
  }
  memory::Scope scope{scratch};
  char* data_a = nullptr;
  char* data_b = nullptr;
  std::size_t read_a = 0;
  std::size_t read_b = 0;
  if (!file::read(a, scratch, data_a, read_a).ok or !file::read(b, scratch, data_b, read_b).ok)
  {
    return false;
  }
  return read_a == read_b and std::memcmp(data_a, data_b, read_a) == 0;
}

}  // namespace

std::string_view folder_path(Folder folder)
{
  const auto i = static_cast<std::size_t>(folder);
  return (i < kFolderCount) ? kFolderPaths[i] : std::string_view{};
}

std::string_view folder_label(Folder folder)
{
  const auto i = static_cast<std::size_t>(folder);
  return (i < kFolderCount) ? kFolderLabels[i] : std::string_view{"?"};
}

bool is_absolute(std::string_view path)
{
  if (path.empty())
  {
    return false;
  }
  if (is_separator(path.front()))
  {
    return true;
  }
  // "C:\..." or "C:/..."
  return path.size() > 2 and path[1] == ':' and is_separator(path[2]);
}

std::string_view file_name(std::string_view path)
{
  for (std::size_t i = path.size(); i > 0; --i)
  {
    if (is_separator(path[i - 1]))
    {
      return path.substr(i);
    }
  }
  return path;
}

std::string_view extension(std::string_view path)
{
  const std::string_view name = file_name(path);
  const std::size_t dot = name.rfind('.');
  // A leading dot is part of the name (".gitignore"), not an extension
  return (dot == std::string_view::npos or dot == 0) ? std::string_view{} : name.substr(dot);
}

std::string_view stem(std::string_view path)
{
  const std::string_view name = file_name(path);
  return name.substr(0, name.size() - extension(path).size());
}

std::string_view parent_path(std::string_view path)
{
  const std::string_view trimmed = without_trailing_separator(path);
  for (std::size_t i = trimmed.size(); i > 0; --i)
  {
    if (is_separator(trimmed[i - 1]))
    {
      return trimmed.substr(0, i - 1);
    }
  }
  return std::string_view{};
}

Status join(std::string_view base, std::string_view tail, Path& out)
{
  const std::string_view head = without_trailing_separator(base);
  while (!tail.empty() and is_separator(tail.front()))
  {
    tail.remove_prefix(1);
  }
  if (head.empty())
  {
    return assign_checked(tail, out);
  }
  if (tail.empty())
  {
    return assign_checked(head, out);
  }
  // +1 for the separator
  if (head.size() + 1 + tail.size() > Path::kMaxLength)
  {
    return Status::error(
      "path '%.*s/%.*s' is too long",
      static_cast<int>(head.size()),
      head.data(),
      static_cast<int>(tail.size()),
      tail.data());
  }
  out.assign(format<Path::capacity()>(
    "%.*s/%.*s", static_cast<int>(head.size()), head.data(), static_cast<int>(tail.size()), tail.data()));
  return Status::okay();
}

Status subfolder(std::string_view root, Folder folder, Path& out) { return join(root, folder_path(folder), out); }

namespace
{

/**
 * @brief <code>path</code> made absolute, against the working directory if it is relative
 *
 * What lets "is this inside the project?" be asked at all: the source comes from the user and the
 * root from wherever the manifest was written, and either may be relative, so neither can be
 * compared with the other until both are in the same frame of reference. The working directory is
 * that frame, because a relative path means the same thing to the process either way. The one place
 * here besides ensure_tree that reaches for std::filesystem.
 */
Status absolute_path(std::string_view path, Path& out)
{
  if (is_absolute(path))
  {
    return assign_checked(path, out);
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (ec)
  {
    return Status::error("cannot find the working directory: %s", ec.message().c_str());
  }
  return join(cwd.string(), path, out);
}

}  // namespace

Status resolve(std::string_view root, std::string_view relative, Path& out)
{
  return is_absolute(relative) ? assign_checked(relative, out) : join(root, relative, out);
}

bool is_inside(std::string_view root, std::string_view path)
{
  const std::string_view base = without_trailing_separator(root);
  if (base.empty() or path.size() <= base.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < base.size(); ++i)
  {
    const bool both_separators = is_separator(base[i]) and is_separator(path[i]);
    if (!both_separators and base[i] != path[i])
    {
      return false;
    }
  }
  return is_separator(path[base.size()]);
}

Status relativize(std::string_view root, std::string_view path, Path& out)
{
  std::string_view tail = path;
  if (is_inside(root, path))
  {
    tail = path.substr(without_trailing_separator(root).size() + 1);
  }
  LVL_RETURN_IF_ERROR(assign_checked(tail, out));
  // Project files always use '/', whatever the platform wrote them
  for (std::size_t i = 0; i < Path::kMaxLength and out.data[i] != '\0'; ++i)
  {
    if (out.data[i] == '\\')
    {
      out.data[i] = '/';
    }
  }
  return Status::okay();
}

Status project_file(Folder folder, std::string_view name, std::string_view suffix, Path& out)
{
  // The name is the user's, so the file name is built a character at a time: anything a file name
  // should not hold becomes '-', a run of them collapses, and a leading or trailing one is dropped,
  // so "a / b" gives "a-b" rather than "a---b-"
  char slug[Name::capacity()] = {};
  std::size_t n = 0;
  for (const char c : name)
  {
    if (n + 1 >= sizeof(slug))
    {
      break;
    }
    const bool keep =
      (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or c == '-' or c == '_' or c == '.';
    if (keep)
    {
      slug[n++] = c;
    }
    else if (n > 0 and slug[n - 1] != '-')
    {
      slug[n++] = '-';
    }
  }
  while (n > 0 and slug[n - 1] == '-')
  {
    --n;
  }
  const std::string_view stem = (n > 0) ? std::string_view{slug, n} : std::string_view{"unnamed"};
  if (stem.size() + suffix.size() > Path::kMaxLength)
  {
    return Status::error(
      "'%.*s' names a file too long for a project's tree", static_cast<int>(name.size()), name.data());
  }
  const Path tail = format<Path::capacity()>(
    "%.*s%.*s", static_cast<int>(stem.size()), stem.data(), static_cast<int>(suffix.size()), suffix.data());
  return join(folder_path(folder), tail.view(), out);
}

Status ensure_tree(std::string_view root)
{
  if (root.empty())
  {
    return Status::error("the project has no folder");
  }
  for (std::size_t i = 0; i < kFolderCount; ++i)
  {
    Path dir;
    LVL_RETURN_IF_ERROR(subfolder(root, static_cast<Folder>(i), dir));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{dir.c_str()}, ec);
    // create_directories reports "already there" as no error with an empty ec, so ec means failure
    if (ec)
    {
      return Status::error("could not create '%s': %s", dir.c_str(), ec.message().c_str());
    }
  }
  return Status::okay();
}

bool is_project_root(std::string_view root)
{
  Path manifest;
  return join(root, kManifestName, manifest).ok and file::exists(manifest.c_str());
}

Status plan_copy(std::string_view root, Folder folder, std::string_view source, memory::Arena& scratch, CopyPlan& out)
{
  out = CopyPlan{};
  if (source.empty())
  {
    return Status::error("no file was chosen");
  }
  // Both made absolute against the working directory before anything is compared. The source is
  // the user's -- a file dialog gives an absolute path, a script may give a relative one -- and the
  // root is whatever the manifest was written to, which may itself be relative. Resolving the
  // source against the *root* instead, as this once did, reads "forest.png" as
  // "<root>/forest.png": an outside file then looks like one already inside, and is stored by a
  // path that names nothing.
  Path absolute_root;
  Path absolute_source;
  LVL_RETURN_IF_ERROR(absolute_path(root, absolute_root));
  LVL_RETURN_IF_ERROR(absolute_path(source, absolute_source));
  if (is_inside(absolute_root.view(), absolute_source.view()))
  {
    out.inside = true;
    return relativize(absolute_root.view(), absolute_source.view(), out.destination);
  }

  const std::string_view name = file_name(absolute_source.view());
  const std::string_view base = stem(name);
  const std::string_view ext = extension(name);
  Path folder_absolute;
  LVL_RETURN_IF_ERROR(subfolder(absolute_root.view(), folder, folder_absolute));

  // The first free name: "a.png", then "a-2.png", "a-3.png", ... ; an identical file is reused
  for (int attempt = 1; attempt <= 1000; ++attempt)
  {
    FixedString<192> candidate;
    if (attempt == 1)
    {
      candidate.assign(name);
    }
    else
    {
      candidate.assign(format<192>(
        "%.*s-%d%.*s", static_cast<int>(base.size()), base.data(), attempt, static_cast<int>(ext.size()), ext.data()));
    }
    Path absolute_destination;
    LVL_RETURN_IF_ERROR(join(folder_absolute.view(), candidate.view(), absolute_destination));
    if (!file::exists(absolute_destination.c_str()))
    {
      out.renamed = attempt > 1;
      return relativize(absolute_root.view(), absolute_destination.view(), out.destination);
    }
    if (same_contents(absolute_source.c_str(), absolute_destination.c_str(), scratch))
    {
      out.identical = true;
      return relativize(absolute_root.view(), absolute_destination.view(), out.destination);
    }
  }
  return Status::error(
    "'%.*s' is already in the project under a thousand names", static_cast<int>(name.size()), name.data());
}

Status apply_copy(std::string_view root, const CopyPlan& plan, std::string_view source, memory::Arena& scratch)
{
  if (plan.inside or plan.identical)
  {
    return Status::okay();
  }
  // The source as the user gave it (absolute, or relative to the working directory); the
  // destination as the plan left it, which is relative to the root
  Path absolute_root;
  Path absolute_source;
  Path absolute_destination;
  LVL_RETURN_IF_ERROR(absolute_path(root, absolute_root));
  LVL_RETURN_IF_ERROR(absolute_path(source, absolute_source));
  LVL_RETURN_IF_ERROR(resolve(absolute_root.view(), plan.destination.view(), absolute_destination));
  std::error_code ec;
  std::filesystem::create_directories(
    std::filesystem::path{Path{parent_path(absolute_destination.view())}.c_str()}, ec);
  if (ec)
  {
    return Status::error(
      "could not create the folder for '%s': %s", absolute_destination.c_str(), ec.message().c_str());
  }
  memory::Scope scope{scratch};
  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(absolute_source.c_str(), scratch, data, size));
  return file::write(absolute_destination.c_str(), data, size);
}

}  // namespace lvl::project
