/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io.hpp
 *
 * Reading and writing a project's manifest (<code>project.json</code> in the root): the project's
 * name, the catalogs and seasons every level shares, the tileset and rule-set files it owns, and the
 * index of its member levels.
 *
 * <b>The manifest is always JSON</b>, with no binary translation, which is the one deliberate
 * exception to "binary is a translation of the same data". It is a few kilobytes of names that is
 * never on a hot path, and keeping its name fixed is what makes "is this folder a project?" a single
 * existence check. Levels, tilesets, rule sets and path graphs keep both formats.
 *
 * The manifest never stores the project's own location: <code>Project::root</code> is taken from
 * where the file was read, so moving or copying a project folder keeps it working.
 */
#pragma once

// C++ Standard Library
#include <string_view>

// LVL
#include "lvl/core/status.hpp"
#include "lvl/level/io.hpp"
#include "lvl/project/project.hpp"

namespace lvl::project
{

/// The manifest's "format" member
inline constexpr std::string_view kProjectFormat = "lvl-project";

/// Writes <code>project</code>'s manifest to <code>path</code>
Status save_project(const char* path, const Project& project, level::IoContext& ctx);

/**
 * @brief Loads a project manifest into <code>project</code> (which is reset first)
 *
 * <code>Project::root</code> is set to the folder <code>path</code> lives in, and every member
 * level's file is checked for existence (<code>LevelEntry::missing</code>).
 *
 * On failure <code>project</code> holds a partial manifest, so load into a spare and swap, or reset
 * it; the editor validates into scratch memory before touching the live project.
 */
Status load_project(const char* path, Project& project, level::IoContext& ctx);

/**
 * @brief Opens the project rooted at <code>root</code>, i.e. loads <code>root/project.json</code>
 *
 * Fails with a plain message when there is no manifest there.
 */
Status open_project(const char* root, Project& project, level::IoContext& ctx);

/**
 * @brief Creates a new project on disk: the folder tree and an empty manifest
 *
 * <code>project</code> is replaced by the new, empty project.
 */
Status create_project(const char* root, std::string_view name, Project& project, level::IoContext& ctx);

}  // namespace lvl::project
