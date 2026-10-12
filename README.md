# DragonDE

A Wayland desktop environment written from scratch in C++23: own compositor
(no wlroots), Vulkan renderer, Qt Quick shell. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and roadmap.

**Status:** M0 complete; M1 in progress (stages 1–3 of 5 done). Apps show up
and animate, including GPU-rendered ones like kitty (zero-copy dma-buf).
Keyboard and mouse input come next.

## Build

Arch Linux packages:

```sh
sudo pacman -S cmake ninja gcc pugixml gtest wayland wayland-protocols libdrm mesa pixman \
    vulkan-icd-loader vulkan-headers vulkan-validation-layers
```

```sh
cmake --preset debug          # Debug + ASan/UBSan; `release` for optimized builds
cmake --build --preset debug
ctest --preset debug
```

## Run

From inside a Wayland session (KDE, GNOME, Sway, ...):

```sh
./build/debug/compositor/dragon-compositor
```

A "DragonDE — NESTED-1" window appears. Run clients inside it by pointing
them at the socket it prints (usually `wayland-1`):

```sh
WAYLAND_DISPLAY=wayland-1 kitty
WAYLAND_DISPLAY=wayland-1 ./build/debug/tools/demo-client/dragon-demo-client
```

Close the window or press Ctrl+C to quit.

| Variable | Values | Default |
|----------|--------|---------|
| `DRAGON_BACKEND` | `nested`, `headless` | nested if a parent compositor exists, else headless |
| `DRAGON_LOG` | `trace` `debug` `info` `warn` `error` `off` | `info` |
| `DRAGON_VK_VALIDATION` | `0`, `1` | on in Debug builds |

## Layout

| Path | What |
|------|------|
| `compositor/core` | Event loop, signals, logging, geometry/regions |
| `compositor/wayland` | Protocol object model (`Resource`, `Global`) |
| `compositor/protocols` | Protocol implementations: surfaces, subsurfaces, shm/dma-buf buffers, outputs, xdg-shell |
| `compositor/render` | GBM allocation, dma-buf sync, swapchains, Vulkan renderer |
| `compositor/backend` | Nested (window in a parent compositor) and headless backends |
| `compositor/desktop` | Window placement, surface textures, per-output rendering |
| `tools/scanner` | `dragon-scanner`: protocol XML → C++ |
| `tools/demo-client` | Minimal animated wl_shm client for manual testing |
| `tests` | GoogleTest suite (GPU tests skip without a GPU) |
