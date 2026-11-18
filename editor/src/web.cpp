/**
 * @copyright 2026-present Brian Cairl
 *
 * @file web.cpp
 */

// LVL
#include "lvl/editor/web.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/editor/app.hpp"

#if defined(__EMSCRIPTEN__)

// Emscripten
#include <emscripten.h>

// clang-format off
EM_JS(void, lvl_web_request_upload, (const char* directory), {
  const dir = UTF8ToString(directory);
  const input = document.createElement('input');
  input.type = 'file';
  input.multiple = true;
  input.onchange = async () => {
    for (const file of input.files) {
      const data = new Uint8Array(await file.arrayBuffer());
      try { FS.mkdirTree(dir); } catch (e) {}
      FS.writeFile(dir + '/' + file.name, data);
      Module.lvlUploaded = (Module.lvlUploaded || 0) + 1;
      console.log('[lvl] uploaded ' + dir + '/' + file.name + ' (' + data.length + ' bytes)');
    }
  };
  input.click();
});

EM_JS(void, lvl_web_offer_download, (const char* path), {
  const p = UTF8ToString(path);
  const data = FS.readFile(p);
  const blob = new Blob([data], {type: 'application/octet-stream'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = p.split('/').pop();
  document.body.appendChild(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
});

EM_JS(int, lvl_web_uploaded, (), { return Module.lvlUploaded || 0; });

EM_JS(void, lvl_web_set_unsaved, (int unsaved), {
  window.onbeforeunload = unsaved ? (e) => { e.preventDefault(); e.returnValue = ''; return ''; } : null;
});
// clang-format on

#endif  // defined(__EMSCRIPTEN__)

namespace lvl::editor::web
{

#if defined(__EMSCRIPTEN__)

void request_upload(const char* directory) { lvl_web_request_upload(directory); }

void offer_download(const char* path) { lvl_web_offer_download(path); }

void set_unsaved_changes(bool unsaved)
{
  // Only touch the page when the state changes
  static int armed = -1;
  if (armed != static_cast<int>(unsaved))
  {
    armed = static_cast<int>(unsaved);
    lvl_web_set_unsaved(armed);
  }
}

void poll([[maybe_unused]] App& app)
{
  static int seen = 0;
  if (const int uploaded = lvl_web_uploaded(); uploaded != seen)
  {
    LVL_LOG_INFO() << (uploaded - seen) << " file(s) uploaded to " << kWorkDirectory;
    seen = uploaded;
  }
}

#else

void request_upload([[maybe_unused]] const char* directory) {}

void offer_download([[maybe_unused]] const char* path) {}

void set_unsaved_changes([[maybe_unused]] bool unsaved) {}

void poll([[maybe_unused]] App& app) {}

#endif  // defined(__EMSCRIPTEN__)

}  // namespace lvl::editor::web
