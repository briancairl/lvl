/**
 * @copyright 2026-present Brian Cairl
 *
 * @file paths.hpp
 *
 * A project is a folder with a fixed sub-folder tree, and everything a project needs lives inside
 * it, so that the whole folder can be moved, copied or committed as one thing:
 *
 * <pre>
 *   shoreline/                 the project root; its name is the project's default name
 *     project.json             the manifest (always JSON; see io.hpp)
 *     levels/                  one file per level ("lvl-level")
 *     tilesets/                one file per tileset ("lvl-tileset")
 *     rules/                   one file per rule set ("lvl-wfc": WFC + auto-tiling + height steps)
 *     paths/                   exported/imported path graphs ("lvl-paths")
 *     assets/
 *       atlases/               tileset atlas images, copied in on import
 *       images/                heightmap seed images and layer image imports
 *     artifacts/               PNG exports: the full map, the mini-preview, layer images
 * </pre>
 *
 * Every path stored in a project file is relative to the root and uses '/' separators, whatever the
 * platform, so the files are portable. A path that points outside the tree is kept absolute (see
 * <code>relativize</code>), which is how a deliberately external reference is expressed.
 *
 * Composing a path can overflow <code>Path</code>, and the inputs come from the user, so the
 * composing functions report a <code>Status</code> rather than truncating silently: a truncated path
 * would name a different file.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>
#include <string_view>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl::project
{

/// File name of a project's manifest, in its root folder
inline constexpr std::string_view kManifestName = "project.json";

/// What a project names its own tileset and rule-set files, after the tileset or rule set itself
inline constexpr std::string_view kTileSetSuffix = ".tileset.json";
inline constexpr std::string_view kRuleSetSuffix = ".rules.json";
inline constexpr std::string_view kScatterSuffix = ".scatter.json";

/// One of the folders a project owns
enum class Folder : std::uint8_t
{
  /// The project root itself
  kRoot,
  kLevels,
  kTileSets,
  kRules,
  /// Scatter profiles: one file per profile ("lvl-scatter")
  kScatter,
  kPaths,
  kAtlases,
  kImages,
  kArtifacts,
};

inline constexpr std::size_t kFolderCount = 9;

/// Path of <code>folder</code> relative to the root ("levels", "assets/atlases", ... ; "" for kRoot)
[[nodiscard]] std::string_view folder_path(Folder folder);

/// Human-readable name of <code>folder</code>, for places panes and messages ("Levels", "Atlases", ...)
[[nodiscard]] std::string_view folder_label(Folder folder);

/// True if <code>path</code> is absolute (POSIX "/..." or Windows "C:\...")
[[nodiscard]] bool is_absolute(std::string_view path);

/// The part of <code>path</code> after the last separator ("assets/atlases/a.png" -> "a.png")
[[nodiscard]] std::string_view file_name(std::string_view path);

/// <code>path</code> without its last extension ("a.tileset.json" -> "a.tileset")
[[nodiscard]] std::string_view stem(std::string_view path);

/// The last extension of <code>path</code>, including the dot ("a.png" -> ".png"; "" if there is none)
[[nodiscard]] std::string_view extension(std::string_view path);

/// <code>path</code> up to the last separator ("" when it has none)
[[nodiscard]] std::string_view parent_path(std::string_view path);

/**
 * @brief Joins <code>base</code> and <code>tail</code> with a single '/'
 *
 * An empty part is skipped, so joining with "" gives the other part back. Fails if the result would
 * not fit a Path.
 */
Status join(std::string_view base, std::string_view tail, Path& out);

/// Absolute path of <code>folder</code> within the project rooted at <code>root</code>
Status subfolder(std::string_view root, Folder folder, Path& out);

/// Absolute path of a root-relative <code>relative</code> (returned unchanged when already absolute)
Status resolve(std::string_view root, std::string_view relative, Path& out);

/// True if <code>path</code> names something inside <code>root</code>
[[nodiscard]] bool is_inside(std::string_view root, std::string_view path);

/**
 * @brief <code>path</code> relative to <code>root</code>, or unchanged when it points outside
 *
 * Separators are normalized to '/'. This is what a project file stores, so a reference that stays
 * absolute is one the user chose to keep outside the project.
 */
Status relativize(std::string_view root, std::string_view path, Path& out);

/**
 * @brief A root-relative path in <code>folder</code> for a part of the project named <code>name</code>
 *
 * "forest" in kTileSets with ".tileset.json" gives "tilesets/forest.tileset.json". A name is the
 * user's, so anything a file name should not hold becomes '-' and an empty one becomes "unnamed":
 * the path a project stores for one of its own parts is always a legal file name, whatever the part
 * is called.
 */
Status project_file(Folder folder, std::string_view name, std::string_view suffix, Path& out);

/// Creates the root and every folder of the tree that is not there yet (idempotent)
Status ensure_tree(std::string_view root);

/// True if <code>root</code> holds a project manifest
[[nodiscard]] bool is_project_root(std::string_view root);

/**
 * @brief Where a copy of <code>source</code> would land in the project, and what copying would do
 *
 * Built before the user is asked, so the confirmation dialog can say exactly what will happen.
 */
struct CopyPlan
{
  /// Where the copy goes, relative to the root (what the project file will store)
  Path destination;
  /// A different file of that name was already there, so the copy was given another name
  bool renamed = false;
  /// The identical bytes are already in the project: copying is a no-op and reuses that file
  bool identical = false;
  /// The source is already inside the project: nothing to copy, destination is just its relative path
  bool inside = false;
};

/**
 * @brief Plans a copy of <code>source</code> into <code>folder</code> of the project at <code>root</code>
 *
 * Uses <code>scratch</code> to compare bytes when a file of that name is already there; it is
 * rewound before returning.
 */
Status plan_copy(std::string_view root, Folder folder, std::string_view source, memory::Arena& scratch, CopyPlan& out);

/**
 * @brief Carries out <code>plan</code>, copying <code>source</code> into the project
 *
 * A no-op when the plan says the bytes are already there or the source is already inside the tree.
 * Uses <code>scratch</code> for the file's bytes; it is rewound before returning.
 */
Status apply_copy(std::string_view root, const CopyPlan& plan, std::string_view source, memory::Arena& scratch);

}  // namespace lvl::project
