# DragonDE

A Wayland desktop environment written from scratch in C++23: own compositor
(no wlroots), Vulkan renderer, Qt Quick shell. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and roadmap.

**Status:** M0 and M1 complete; working toward everyday apps. Firefox and
VS Code (Electron, `--ozone-platform=wayland`) run inside the nested session
with GPU rendering, input, menus, copy/paste and middle-click paste, drag and
drop, cursors (named shapes and app-drawn images), focus activation and
portal file dialogs. X11 apps run through Xwayland (started on demand) and
are managed like any other window; copy/paste between X11 and Wayland apps
isn't bridged yet.

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
WAYLAND_DISPLAY=wayland-1 foot
WAYLAND_DISPLAY=wayland-1 ./build/debug/tools/demo-client/dragon-demo-client
```

Apps started inside DragonDE (e.g. from a terminal opened with `Mod+Enter`)
inherit its environment: Wayland socket, desktop name and private D-Bus
session, plus `DISPLAY` for X11 apps. Electron apps run natively on Wayland
with `--ozone-platform=wayland`, or through Xwayland without it.

Close the window or press Ctrl+C to quit.

### Shortcuts

`Mod` is **Super**, or **Alt** when running nested (the parent compositor
usually keeps Super shortcuts for itself). Override with `DRAGON_MOD`.

| Shortcut | Action |
|----------|--------|
| `Mod+Enter` | Open a terminal (`DRAGON_TERMINAL`, default `kitty`) |
| `Mod+Q` | Close the focused window |
| `Mod+Shift+Esc` | Quit DragonDE |
| `Mod`+left drag | Move a window |
| `Mod`+right drag | Resize a window from the nearest corner |
| Click | Focus and raise |

Apps that draw their own title bars (kitty, GTK apps) can also be moved,
resized and maximized from those.

| Variable | Values | Default |
|----------|--------|---------|
| `DRAGON_BACKEND` | `nested`, `headless` | nested if a parent compositor exists, else headless |
| `DRAGON_LOG` | `trace` `debug` `info` `warn` `error` `off` | `info` |
| `DRAGON_VK_VALIDATION` | `0`, `1` | on in Debug builds |
| `DRAGON_MOD` | `super`, `alt`, `ctrl` | `super`; `alt` when nested |
| `DRAGON_TERMINAL` | any shell command | `kitty` |
| `DRAGON_XWAYLAND` | `0`, `1` | `1`: X11 apps get our own `DISPLAY`, served by Xwayland on first use |
| `DRAGON_PRIVATE_BUS` | `0`, `1` | `1` when nested: apps get their own D-Bus session so portals (file dialogs) open inside DragonDE |
| `DRAGON_SCREENSHOT_DIR` | directory | `/tmp`; `kill -USR1 <pid>` saves each output as PNG |

## Layout

| Path | What |
|------|------|
| `compositor/core` | Event loop, signals, logging, geometry/regions |
| `compositor/wayland` | Protocol object model (`Resource`, `Global`) |
| `compositor/protocols` | Protocol implementations: surfaces, subsurfaces, shm/dma-buf buffers, outputs, xdg-shell, seat, clipboard |
| `compositor/render` | GBM allocation, dma-buf sync, swapchains, Vulkan renderer |
| `compositor/backend` | Nested (window in a parent compositor) and headless backends |
| `compositor/desktop` | Window placement and focus, input routing, surface textures, per-output rendering |
| `compositor/xwayland` | Xwayland process (lazy start, display/lock/sockets), X11 window manager, xwayland-shell |
| `tools/scanner` | `dragon-scanner`: protocol XML → C++ |
| `tools/demo-client` | Minimal animated wl_shm client for manual testing |
| `tests` | GoogleTest suite (GPU tests skip without a GPU) |
