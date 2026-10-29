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
    vulkan-icd-loader vulkan-headers vulkan-validation-layers libxkbcommon lua pango cairo gdk-pixbuf2
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

### Configuration

Settings and shortcuts live in `~/.config/dragon/config.lua` (Lua). Start from
the documented example, which lists every setting with its default:

```bash
mkdir -p ~/.config/dragon && cp data/config/config.lua ~/.config/dragon/
```

Saving the file applies it immediately (so does `Mod+Shift+R`). If it has an
error, the log says where and the previous configuration stays in effect.

### Window styling

Every window gets the same look from the `windows` section: rounded corners,
a border (solid or gradient), a drop shadow, a colored glow, opacity
(focused, unfocused, fullscreen), dimming of unfocused windows and blur
behind anything translucent (dual Kawase, as in Hyprland). A `wallpaper`
goes behind everything. Apps that let the compositor draw their title
bar (kitty, foot, Qt and X11 apps) get DragonDE's, with minimize, maximize
and close buttons; apps that insist on their own (GTK, Firefox) keep it and
are still rounded, bordered and shaded. Per-app overrides:

```lua
dragon.rule({ app_id = "kitty" }, { opacity = { active = 0.95, inactive = 0.85 } })
dragon.rule({ app_id = "mpv" }, { titlebar = { enabled = false }, border = { width = 0 } })
dragon.layer_rule({ namespace = "waybar" }, { blur = true, ignore_alpha = 0.3 })
```

Title bar: drag to move, double-click to maximize. Drag a window's edge or
corner to resize. Fullscreen windows are drawn untouched.

### Bars, launchers and wallpapers

Layer-shell clients work: bars like `waybar` (windows keep clear of the
space they reserve), launchers like `wofi --show drun` (centered, above
everything, with the keyboard while open) and wallpaper tools like `swaybg`.


### Shortcuts

`Mod` is **Super**, or **Alt** when running nested (the parent compositor
usually keeps Super shortcuts for itself); set `modifier` in the config to
choose. All of these can be changed with `dragon.bind` / `dragon.unbind`.

| Shortcut | Action |
|----------|--------|
| `Mod+Return` | Open a terminal (`terminal`, default `kitty`) |
| `Mod+Q` | Close the focused window |
| `Mod+M` / `Mod+Shift+M` | Minimize / bring back the last minimized window |
| `Mod+Up` | Toggle maximized |
| `Mod+F` | Toggle fullscreen |
| `Mod+Shift+R` | Reload the config |
| `Mod+Shift+Escape` | Quit DragonDE |
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
| `DRAGON_CONFIG` | path | `$XDG_CONFIG_HOME/dragon/config.lua` |
| `DRAGON_MOD` | `super`, `alt`, `ctrl` | what `modifier = "auto"` means: `super`; `alt` when nested |
| `DRAGON_XWAYLAND` | `0`, `1` | `1`: X11 apps get our own `DISPLAY`, served by Xwayland on first use |
| `DRAGON_PRIVATE_BUS` | `0`, `1` | `1` when nested: apps get their own D-Bus session so portals (file dialogs) open inside DragonDE |
| `DRAGON_SCREENSHOT_DIR` | directory | `/tmp`; `kill -USR1 <pid>` saves each output as PNG |

## Layout

| Path | What |
|------|------|
| `compositor/core` | Event loop, signals, logging, geometry/regions |
| `compositor/config` | `config.lua`: settings and shortcuts (Lua 5.4) |
| `compositor/wayland` | Protocol object model (`Resource`, `Global`) |
| `compositor/protocols` | Protocol implementations: surfaces, subsurfaces, shm/dma-buf buffers, outputs, xdg-shell, layer-shell, seat, clipboard |
| `protocols` | Protocol XML not shipped in wayland-protocols (wlr-layer-shell) |
| `compositor/render` | GBM allocation, dma-buf sync, swapchains, Vulkan renderer |
| `compositor/backend` | Nested (window in a parent compositor) and headless backends |
| `compositor/desktop` | Window placement and focus, frames and title bars, input routing, surface textures, per-output rendering |
| `compositor/xwayland` | Xwayland process (lazy start, display/lock/sockets), X11 window manager, xwayland-shell |
| `tools/scanner` | `dragon-scanner`: protocol XML → C++ |
| `tools/demo-client` | Minimal animated wl_shm client for manual testing |
| `tests` | GoogleTest suite (GPU tests skip without a GPU) |
