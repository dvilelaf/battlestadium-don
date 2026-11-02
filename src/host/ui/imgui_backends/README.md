# Dear ImGui SDL3 backends (copied, unmodified)

These four files are the SDL3 platform and renderer backends of **Dear ImGui 1.92.8**,
copied verbatim from the upstream repository at tag `v1.92.8`:

- `imgui_impl_sdl3.h` / `imgui_impl_sdl3.cpp` — `backends/` (platform: SDL3 events, keyboard, gamepad)
- `imgui_impl_sdlrenderer3.h` / `imgui_impl_sdlrenderer3.cpp` — `backends/` (renderer: `SDL_Renderer`)

Upstream: <https://github.com/ocornut/imgui> · tag `v1.92.8` · MIT licence (`LICENSE.txt`,
Copyright (c) 2014-2026 Omar Cornut).

They pair with the Dear ImGui core that PCSX2 vendors at
`third_party/pcsx2/3rdparty/imgui` (also 1.92.8, MIT); the core is compiled from there and is
not duplicated here. The two backends are copied into this folder (rather than taken from PCSX2,
which does not vendor them) so the start screen can build them unchanged, exactly as upstream
ships them.
