/**
 * @copyright 2026-present Brian Cairl
 *
 * @file build.hpp
 */
#pragma once

namespace lvl
{

/// Editor version, also written into every saved level/tileset so loaders can migrate old files
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
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
