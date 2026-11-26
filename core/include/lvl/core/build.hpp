/**
 * @copyright 2026-present Brian Cairl
 *
 * @file build.hpp
 */
#pragma once

namespace lvl
{

/**
 * @brief Editor version, written into every file so loaders can migrate old ones
 *
 * 0.3 is the version tile groups came in: a tileset carries named groups of tiles that the editor
 * stamps as one thing. In binary they are appended to the tileset block, so a 0.1 or 0.2 file
 * simply has none -- which the loaders tell from the version in the header (binary::Version).
 *
 * 0.2 is the version projects came in. A level file now says whether it carries the catalogs,
 * seasons, tilesets and rules its cells reference (level::LevelScope) or leaves them to its
 * project's manifest, so a 0.2 level can be one a 0.1 build would read as having no tilesets at
 * all. The loaders refuse a file written by a newer editor, which is what keeps that from
 * happening quietly. Every 0.1 file still loads: it carries them, as they all did.
 */
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 3;
inline constexpr int kVersionPatch = 0;

/// Packed version number: <code>major * 10000 + minor * 100 + patch</code>
inline constexpr int kVersion = kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch;

#if defined(__EMSCRIPTEN__)
inline constexpr bool kIsWeb = true;
#else
inline constexpr bool kIsWeb = false;
#endif  // defined(__EMSCRIPTEN__)

#if defined(NDEBUG)
inline constexpr bool kIsDebug = false;
#else
inline constexpr bool kIsDebug = true;
#endif  // defined(NDEBUG)

}  // namespace lvl
