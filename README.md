# DragonDE

A Wayland desktop environment written from scratch in C++23: own compositor
(no wlroots), Vulkan renderer, Qt Quick shell. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and roadmap.

**Status:** M0 (skeleton) complete. The compositor opens a window inside your
current Wayland session and renders into it with Vulkan; it doesn't accept
clients yet.

## Build

Arch Linux packages:

```sh
sudo pacman -S cmake ninja gcc pugixml gtest wayland wayland-protocols libdrm mesa \
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

A "DragonDE — NESTED-1" window appears, slowly cycling through colors. Close
it or press Ctrl+C to quit.

| Variable | Values | Default |
|----------|--------|---------|
| `DRAGON_BACKEND` | `nested`, `headless` | nested if a parent compositor exists, else headless |
| `DRAGON_LOG` | `trace` `debug` `info` `warn` `error` `off` | `info` |
| `DRAGON_VK_VALIDATION` | `0`, `1` | on in Debug builds |

## Layout

| Path | What |
|------|------|
| `compositor/core` | Event loop, signals, logging |
| `compositor/wayland` | Protocol object model (`Resource`, `Global`) |
| `compositor/render` | GBM allocation, dma-buf sync, swapchains, Vulkan renderer |
| `compositor/backend` | Nested (window in a parent compositor) and headless backends |
| `tools/scanner` | `dragon-scanner`: protocol XML → C++ |
| `tests` | GoogleTest suite (GPU tests skip without a GPU) |
