# Web build

`bazel build //editor:web` produces `bazel-bin/editor/web/lvl.{html,js,wasm}`: the same editor as
`//editor:lvl`, compiled with Emscripten (the `emsdk` Bazel module) against Emscripten's GLFW port
and WebGL2. Serve the directory over HTTP and open `lvl.html` (`lvl.html?arg=--demo` opens the demo
level).

Differences from the native build:

- **Files** live in Emscripten's in-memory file system (working directory `/work`). *File > Upload
  files...* (or *Upload...* in the file dialog) copies files from your machine into it, and every
  save/export is also offered as a browser download.
- **No threads**: without pthreads (which need cross-origin isolation headers most static hosts can't
  set) background jobs run synchronously, so the page pauses during long saves or generation.
- **Memory**: the editor reserves 512 MiB by default (`?arg=--memory=N` in MiB); the wasm heap grows
  to fit.

`smoke_test.mjs` (Node + Playwright) opens the build in headless Chromium with the demo level and
fails on any page error; CI runs it on every change.
