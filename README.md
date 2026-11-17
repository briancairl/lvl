# lvl

A 2D tile-based level editor written in C++20 with Dear ImGui, building natively (GLFW/OpenGL3)
and for the web (Emscripten/WebGL2).

## Building

Requires [bazelisk](https://github.com/bazelbuild/bazelisk). On Linux, GLFW also needs the X11/GL
development headers:

```bash
sudo apt-get install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libgl-dev
```

```bash
bazel test //...
```

See [CLAUDE.md](CLAUDE.md) for the project's design tenets and layout.
