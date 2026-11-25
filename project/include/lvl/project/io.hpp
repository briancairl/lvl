/**
 * @copyright 2026-present Brian Cairl
 *
 * @file io.hpp
 *
 * Reading and writing a project's manifest (<code>project.json</code> in the root): the project's
 * name, the catalogs and seasons every level shares, the tileset and rule-set files it owns, and the
 * index of its member levels.
 *
 * The tilesets and rule sets are not <i>in</i> the manifest: it names their files, and saving or
 * loading a project writes or reads each one in the standalone <code>lvl-tileset</code> and
 * <code>lvl-wfc</code> formats the editor already imports and exports. That is what those formats
 * existing buys: per-tileset and per-rule-set diffs stay small, and a project is a folder of files
 * a person can read rather than one large document.
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
#include <cstddef>
#include <string_view>

// LVL
#include "lvl/core/status.hpp"
#include "lvl/level/io.hpp"
#include "lvl/project/paths.hpp"
#include "lvl/project/project.hpp"

namespace lvl::project
{

/// The manifest's "format" member
inline constexpr std::string_view kProjectFormat = "lvl-project";

/**
 * @brief Writes <code>project</code>: its manifest at <code>path</code>, and a file per part
 *
 * Each tileset and rule set is written to the file recorded for it, or, when it has none yet, to one
 * made from its name under <code>tilesets/</code> or <code>rules/</code>. Those choices are
 * recorded in <code>project</code> -- which is why this does not take it by const reference -- so
 * the manifest and the project agree on where every part lives, and a part keeps the same file the
 * next time around.
 *
 * The folder tree is created if it is not there, so saving a project that was built in memory works
 * without a separate <code>ensure_tree</code>.
 */
Status save_project(const char* path, Project& project, level::IoContext& ctx);

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
 * @brief Where <code>project</code> would write its tileset <code>id</code>, root-relative
 *
 * The file already recorded for it, or one made from its name. Exposed because the editor shows it
 * and the tests check it.
 */
Status tileset_file(const Project& project, level::TileSetId id, Path& out);

/// Where <code>project</code> would write rule set <code>index</code>, root-relative
Status rule_set_file(const Project& project, std::size_t index, Path& out);

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
