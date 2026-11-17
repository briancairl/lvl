/**
 * @copyright 2026-present Brian Cairl
 *
 * @file web.hpp
 *
 * Browser integration for the WebAssembly build: files live in Emscripten's in-memory file system,
 * so users upload files into it (a file picker) and download what they save or export. All of this
 * is a no-op natively.
 */
#pragma once

namespace lvl::editor
{

struct App;

namespace web
{

#if defined(__EMSCRIPTEN__)
inline constexpr bool kAvailable = true;
#else
inline constexpr bool kAvailable = false;
#endif  // defined(__EMSCRIPTEN__)

/// Working directory in the browser's in-memory file system
inline constexpr const char* kWorkDirectory = "/work";

/// Opens the browser's file picker; chosen files are written into <code>directory</code>
void request_upload(const char* directory);

/// Hands the file at <code>path</code> to the browser as a download
void offer_download(const char* path);

/// Per-frame housekeeping (logs finished uploads)
void poll(App& app);

}  // namespace web
}  // namespace lvl::editor
