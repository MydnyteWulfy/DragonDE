# DragonDE — Architecture Plan

Status: v0.3 — M0 and M1 complete
Language: C++23 · Build: CMake · Target: Arch Linux, Intel Arc (xe/Mesa) first

---

## 1. Goals and non-goals

**Goals**
- A complete Wayland desktop environment: compositor, shell (panel, launcher,
  notifications, lock screen, wallpaper), session startup, and settings.
- **No wlroots.** Like KWin (and Hyprland since its switch to Aquamarine), we own
  the whole stack above the kernel and Mesa: DRM/KMS, input, buffer import,
  rendering, and protocol handling.
- C++ end to end, with clean internal APIs so each layer can be tested alone.
- Developed **nested** (as a window inside the current KDE session) first, and
  run on bare metal from a TTY later.

**Non-goals (for now)**
- X11 window manager mode (XWayland is supported; native X11 sessions are not).
- Non-Linux platforms.
- Multi-GPU / hybrid graphics.

### Is "KDE-style, no wlroots" viable?

Yes. This is how KWin is built: its own DRM backend, libinput, EGL/GBM
renderer, and a C++ wrapper around `libwayland-server`. Hyprland made the same
move in 2024 (Aquamarine plus a C++ protocol scanner). The costs we accept:

| We own                         | Why it's hard                                         |
|--------------------------------|-------------------------------------------------------|
| KMS atomic modesetting         | Plane assignment, test-commits, hotplug, VRR, page-flip timing |
| Buffer import (shm + dmabuf)   | Format/modifier negotiation, Vulkan import, explicit sync |
| Session/seat (libseat)         | VT switching, device revoke, suspend/resume           |
| Protocol plumbing              | Dozens of protocols, each with edge cases clients rely on |
| XWayland window manager        | A full XWM over xcb (focus, stacking, ICCCM/EWMH)     |

The order of work below gets something usable early instead of after months
of backend work.

---

## 2. Process layout

```
                  ┌──────────────────────── dragon-session ────────────────────────┐
                  │ env setup · dbus · systemd --user target · crash restart       │
                  └───────┬─────────────────────────────────────────────┬──────────┘
                          │ spawns                                      │ spawns
                ┌─────────▼──────────┐   private Wayland protocol   ┌───▼────────────────┐
  libinput ───► │  dragon-compositor │ ◄──────────────────────────► │  dragon-shell      │
  DRM/KMS  ◄─── │  (KWin equivalent) │   + layer-shell / foreign-   │  panel · launcher  │
  libseat  ───► │                    │     toplevel / session-lock  │  notifications ·   │
                │  ┌──────────────┐  │                              │  OSD · wallpaper   │
                │  │  XWayland    │  │                              └────────────────────┘
                │  │  (child)     │  │   ┌───────────────┐  ┌──────────────────┐
                │  └──────────────┘  │   │ dragon-lock   │  │ dragon-settings  │
                └─────────▲──────────┘   └───────────────┘  └──────────────────┘
                          │ wayland                ┌───────────────────────────────┐
                    regular apps                   │ dragon-portal (xdg-desktop-   │
                                                   │ portal backend, PipeWire)     │
                                                   └───────────────────────────────┘
```

**The compositor stays small and must never crash.** Anything with complex UI
runs in a separate process, so a panel bug can't take the session down. This
matches Plasma (KWin plus plasmashell).

| Binary              | Role |
|---------------------|------|
| `dragon-compositor` | Display server: backends, renderer, protocols, window management, input, XWayland |
| `dragon-session`    | Session entry point: env, D-Bus, systemd user units, launches and supervises the rest |
| `dragon-shell`      | Desktop UI as Wayland clients (layer-shell surfaces) |
| `dragon-lock`       | Lock screen using `ext-session-lock-v1` and PAM |
| `dragon-settings`   | Settings app; talks to the compositor over D-Bus and output-management |
| `dragon-portal`     | `xdg-desktop-portal` backend: screenshot, screencast (PipeWire), global shortcuts |
| `dragonctl`         | CLI for scripting and debugging (reload config, list windows, dump scene) |

---

## 3. Compositor internals

```
┌─────────────────────────────────────────────────────────────────────┐
│ Policy        WindowManager · Workspaces · Keybinds · Config        │
├─────────────────────────────────────────────────────────────────────┤
│ Protocols     wl_compositor, xdg_shell, dmabuf, layer-shell, …      │
│               (generated C++ bindings + hand-written logic)         │
├─────────────────────────────────────────────────────────────────────┤
│ Scene         Node tree · per-output damage · hit testing           │
├─────────────────────────────────────────────────────────────────────┤
│ Render        Vulkan 1.3 · dmabuf import · GBM-backed targets        │
├─────────────────────────────────────────────────────────────────────┤
│ Backend       IBackend → Nested(Wayland) | DRM | Headless           │
│               outputs · input devices · allocator · present timing  │
├─────────────────────────────────────────────────────────────────────┤
│ Core          event loop · logging · signals · fd/timer sources     │
└─────────────────────────────────────────────────────────────────────┘
```

Rule: each layer only calls downward. Upward communication goes through
signals/events. Policy never touches DRM; the backend never knows what a
window is.

### 3.1 Core
- **Event loop:** `wl_event_loop` is the single main loop. libinput, DRM page-flip
  events, the parent Wayland connection (nested), timers, signalfd, and D-Bus
  are all fd sources on it. Single-threaded by design, like KWin. Threads are
  used only for isolated jobs (shader compile, screenshot encode).
- **Signals:** a small typed `Signal<Args...>` with RAII `Connection` handles.
  `wl_signal` is not used directly in C++ code.
- **Logging:** `spdlog`-style categories (`backend.drm`, `proto.xdg`, …) with
  runtime level control via `dragonctl`.
- **Memory:** wrapper objects own their `wl_resource` via `unique_ptr`. The
  resource destroy handler is the only point where a wrapper is deleted, which
  avoids most use-after-free bugs in Wayland servers.

### 3.2 Backends (`IBackend`)

```cpp
class IBackend {
public:
    virtual bool start() = 0;
    virtual std::span<Output*> outputs() = 0;
    virtual Allocator& allocator() = 0;         // GBM or shm/dmabuf
    Signal<Output*> outputAdded, outputRemoved;
    Signal<InputDevice*> inputAdded, inputRemoved;
};
```

| Backend    | Purpose | Order |
|------------|---------|-------|
| **Nested** | Runs as an `xdg_toplevel` inside KDE. Each output is a window; input comes from the parent `wl_seat`. Fast iteration, no TTY switching. | 1st |
| **Headless** | No display. Renders offscreen; used for automated tests/CI. | 1st |
| **DRM**    | Real hardware: libseat session, KMS **atomic** API, GBM buffers, libinput, libdisplay-info for EDID, udev hotplug. | 3rd |

Startup order (implemented in M0): create the backend → ask it for its
preferred GPU (nested: the parent's dmabuf-feedback main device) → create the
`Gpu` (render node + GBM + Vulkan) on that device → `backend.start(gpu)`
creates outputs, each owning a `Swapchain` of GBM buffers. Outputs emit
`frame` when it's time to draw; the render loop acquires a slot, renders,
and calls `present()`.

DRM details planned up front:
- Atomic commits only (no legacy API); `TEST_ONLY` before every real commit.
- Primary plane first; cursor plane next; overlay-plane direct scanout of
  fullscreen client buffers later (big win for games and video).
- Page-flip driven frame scheduling: render as late as possible before vblank.
- VRR (`VRR_ENABLED`) and tearing (`wp_tearing_control_v1`) for games.

### 3.3 Renderer (Vulkan)
- **Vulkan 1.3 from the start** (decided). Mesa's ANV driver on the Arc B580
  supports everything below. We use dynamic rendering and synchronization2,
  so no render-pass objects and no `VkFramebuffer` boilerplate.
- **One buffer path for every backend.** We never use a `VkSwapchainKHR`.
  Render targets are GBM buffer objects imported into Vulkan as dmabufs
  (`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`):
  - DRM backend: the same dmabuf becomes a KMS framebuffer and is scanned out.
  - Nested backend: the dmabuf is handed to the parent compositor (KDE)
    through *its* `linux-dmabuf-v1`, attached to our window's surface.
  - Headless: the dmabuf is read back for tests and screenshots.
  So the nested dev loop exercises exactly the code the real hardware runs.
- Client buffer import:
  - `wl_shm` → staging buffer → `VkImage` copy (only damaged rects).
  - `linux-dmabuf-v1` → `VkImage` bound to imported memory, zero-copy.
    Format/modifier tables come from `vkGetPhysicalDeviceFormatProperties2`
    with `VkDrmFormatModifierPropertiesListEXT`; feedback (v4+) tells clients
    which modifiers can be scanned out.
- **Synchronization** is the hard part with Vulkan, so it's designed up front:
  - Implicit-sync clients: export the dmabuf's fence with
    `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and import it as a semaphore
    (`VK_KHR_external_semaphore_fd`, `SYNC_FD`) before sampling.
  - Explicit-sync clients: `linux-drm-syncobj-v1` timeline points imported
    the same way.
  - Our output: export a sync_file from the render's completion and give it to
    KMS as `IN_FENCE_FD`, or attach it to the dmabuf for the parent compositor.
  - Queue family ownership transfers use `VK_QUEUE_FAMILY_FOREIGN_EXT` for
    every imported/exported image.
- Shaders: GLSL compiled to SPIR-V at build time with `glslc`, then embedded
  in the binary. A handful of pipelines: textured quad (with per-format variants),
  solid rect, and later blur/shadow.
- Validation layers are on in debug builds. Every Vulkan object is named via
  `VK_EXT_debug_utils` so validation messages are readable.
- Effects (blur, shadows, rounded corners, animations) are render passes on
  scene nodes. They are designed for but not built until after M4.

### 3.4 Scene graph
- Tree of nodes: `TreeNode`, `SurfaceNode` (client buffer), `RectNode`,
  `TextureNode` (decorations/effects).
- Layers from bottom to top: `background · bottom · windows · top · overlay ·
  lock`, matching layer-shell and session-lock.
- Per-output damage accumulation with buffer age (we own the target ring, so age is known), so
  only changed regions are repainted.
- Hit-testing for input uses the same tree, so what you see is what you click.

### 3.5 Wayland protocol layer
- **`dragon-scanner`** (`tools/scanner`): generates C++ classes from protocol
  XML, layered on top of `wayland-scanner`'s C output. The C side keeps doing
  message encoding (implementation structs, `*_send_*`, `wl_interface` tables);
  we generate one class per interface deriving from `dragon::Resource`:
  `virtual handleX(...)` per request (typed: enums, `double` for fixed,
  `string_view` for strings), `sendX(...)` per event (dropped if newer than the
  bound version), `fromResource()`, and `enum class` types with bitfield
  operators. Object lifetime is tied to the `wl_resource`. Wired up with
  `dragon_add_protocol()` in `cmake/DragonProtocols.cmake`.
- Protocol XML comes from the system `wayland-protocols` (1.49 installed), plus
  vendored wlr-protocols for the de-facto standards.

**Protocol tiers**

| Tier | Needed for | Protocols |
|------|-----------|-----------|
| 0 | Any client shows | `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_wm_base`, `wl_data_device_manager` |
| 1 | Real apps behave | `linux-dmabuf-v1`, `xdg-decoration`, `viewporter`, `fractional-scale-v1`, `xdg-output`, `presentation-time`, `cursor-shape-v1`, `primary-selection`, `xdg-activation`, `single-pixel-buffer`, `linux-drm-syncobj-v1` |
| 1g | Games/engines (LunarEngine!) | `pointer-constraints`, `relative-pointer`, `tearing-control`, `content-type`, `idle-inhibit`, `keyboard-shortcuts-inhibit` |
| 2 | Our own shell | `wlr-layer-shell`, `ext-session-lock-v1`, `ext-foreign-toplevel-list` + `wlr-foreign-toplevel-management`, `ext-idle-notify`, `ext-image-copy-capture`, `wlr-output-management`, `dragon-shell-v1` (private) |
| 3 | Polish | `text-input-v3` / `input-method-v2`, `xdg-foreign`, `xdg-dialog`, `security-context`, `tablet-v2`, `pointer-gestures`, `xdg-toplevel-icon` |

### 3.6 Window management
- `Window` is an abstract type with two implementations: `XdgWindow` and
  `X11Window` (XWayland). Policy code only sees `Window`.
- Configure/ack serial tracking is done once and done right. Most "window
  flickers on resize" bugs come from getting this wrong.
- **Floating by default with optional tiling per workspace** (open decision,
  see §7). Workspaces per output; interactive move/resize; snapping.
- Decorations: server-side by default (we draw them as scene nodes), and
  client-side when the client insists.
- Focus: click-to-focus, `xdg-activation` for focus stealing prevention.

### 3.7 Input
- **libinput** (DRM backend) or the parent seat (nested), normalized into one
  `InputDevice` abstraction.
- **xkbcommon** for keymaps, layouts, and compose.
- Keybinding engine: compositor bindings are checked before clients;
  `keyboard-shortcuts-inhibit` lets games and VMs grab everything.
- Pointer: acceleration settings, constraints/locks, relative motion,
  XCursor themes plus `cursor-shape-v1`. The cursor goes on a hardware plane
  when possible.
- Touchpad gestures map to workspace switching and overview.

### 3.8 Outputs
- Mode setting, scale (integer and fractional), transform, position, VRR,
  and HDR-ready color pipeline later. HDR goes after v1; the Arc B580 can do
  it, but it's a project on its own.
- Hotplug with layout restore keyed by EDID (make/model/serial).

### 3.9 XWayland
- Lazily started on first X11 client (like KWin/Sway). Reduces idle cost.
- Own XWM over **xcb**: ICCCM/EWMH, override-redirect windows, selections
  bridging (clipboard and DnD between X and Wayland). Planned as M5; it's
  big enough to need its own milestone. Steam and many games need it.

### 3.10 Configuration and IPC
- Config: **TOML** (`toml++`) at `~/.config/dragon/dragon.toml`, hot-reloaded
  via inotify. Settings app writes the same file.
- IPC:
  - **D-Bus** (`sdbus-c++`): desktop-facing services (`org.dragon.Compositor`,
    notifications, global shortcuts, screenshots).
  - **`dragon-shell-v1`**: private Wayland protocol giving the shell
    privileged info (workspaces, window thumbnails, OSD triggers).
    Restricted to clients spawned by `dragon-session`.
  - Unix socket + JSON for `dragonctl`, for easy scripting.

---

## 4. Shell and desktop components

**Toolkit: Qt 6 / Qt Quick** (decided).
- It's what Plasma uses, it's C++, and it's already installed (6.11). Needs
  `qt6-wayland` and `layer-shell-qt` added.
- Qt Quick picks its own graphics backend; it can run on Vulkan too, but it
  doesn't need to match the compositor.

| Component      | Protocols/APIs |
|----------------|----------------|
| Panel/taskbar  | layer-shell, foreign-toplevel, `dragon-shell-v1`, StatusNotifierItem (tray) |
| Launcher       | layer-shell; `.desktop` parsing per XDG spec; fuzzy search |
| Notifications  | `org.freedesktop.Notifications` D-Bus server, layer-shell popups |
| OSD            | volume/brightness via PipeWire / logind |
| Wallpaper      | layer-shell `background` layer |
| Lock screen    | `ext-session-lock-v1`, PAM, idle via `ext-idle-notify` |
| Polkit agent   | `org.freedesktop.PolicyKit1.AuthenticationAgent` |
| Portal backend | `xdg-desktop-portal` impl: Screenshot, ScreenCast (PipeWire + image-copy-capture), GlobalShortcuts, Settings (dark mode); FileChooser delegated to the KDE/GTK portal |

---

## 5. Repository layout

```
DragonDE/
├── CMakeLists.txt
├── cmake/                     # toolchain helpers, protocol-gen functions
├── docs/
├── protocols/                 # vendored XML (wlr-*, dragon-shell-v1)
├── tools/
│   └── scanner/               # dragon-scanner (XML → C++)
├── compositor/
│   ├── core/                  # event loop, signals, logging
│   ├── backend/{nested,drm,headless}/
│   ├── render/vulkan/  # + shaders/ (GLSL → SPIR-V)
│   ├── scene/
│   ├── protocols/             # handwritten logic per protocol
│   ├── wm/                    # windows, workspaces, focus, decorations
│   ├── input/
│   ├── xwayland/
│   └── main.cpp
├── session/                   # dragon-session, .desktop for SDDM, systemd units
├── shell/                     # Qt Quick panel/launcher/notifications
├── lock/
├── settings/
├── portal/
├── ctl/                       # dragonctl
└── tests/                     # headless-backend integration tests + unit tests
```

---

## 6. Milestones

| # | Milestone | Done when |
|---|-----------|-----------|
| **M0** ✅ | Skeleton | CMake builds; `dragon-scanner` generates bindings; core loop; Vulkan device + GBM-backed render targets with validation clean; nested backend opens a window and shows a Vulkan-cleared dmabuf; headless backend renders offscreen. |
| **M1** ✅ | First pixels from clients | Tier 0 protocols + linux-dmabuf. **kitty** (the default terminal; GPU-rendered, so it needs dmabuf) runs inside the nested compositor. Keyboard/pointer focus, move, close, Super+Enter spawns the terminal. Stages: (1) ✅ surfaces, subsurfaces, shm, wl_output, xdg_shell; (2) ✅ textured-quad renderer + shm upload; (3) ✅ linux-dmabuf v4 import (kitty renders zero-copy); (4) ✅ wl_seat input + data device (typing in kitty/foot, clipboard); (5) ✅ window management (shortcuts, move/resize, maximize/fullscreen, spawning). |
| **M2** | Real apps | Tier 1 + 1g. dmabuf zero-copy; damage tracking; SSD; fractional scale; multiple nested outputs. Qt, GTK4, Firefox, mpv, and a LunarEngine build all run correctly. |
| **M3** | Bare metal | DRM backend with libseat; launch from TTY; VT switching; hotplug; cursor plane; frame scheduling; VRR. SDDM session entry. |
| **M4** | Desktop shell | Tier 2. Panel, launcher, notifications, wallpaper, lock screen, OSD. First "daily drive for an evening" test. |
| **M5** | XWayland | XWM, clipboard/DnD bridging, Steam + Proton games work. |
| **M6** | Integration | Portals (screencast in OBS/Discord), polkit, settings app, config hot-reload, `dragonctl`. |
| **M7** | Polish | Animations, blur/shadows, tiling, gestures, overview, direct scanout, HDR investigation. |

Testing throughout:
- Headless backend plus a small test client library for protocol-level
  integration tests (create surface → commit buffer → assert scene).
- An app compatibility checklist run per milestone: kitty, foot, gtk4-demo,
  Qt widgets/QML, Firefox, Chromium (Ozone Wayland), mpv, LunarEngine, Steam.
- ASan/UBSan builds by default in debug.

---

## 7. Open decisions

**Decided (2026-10-11)**

| Decision | Choice |
|----------|--------|
| Shell toolkit | Qt 6 / Qt Quick |
| Window paradigm | Floating by default, tiling toggled per workspace |
| Renderer | Vulkan 1.3 from the start (no GLES path) |
| Scanner | Our own `dragon-scanner` |

**Still open**

| Decision | Recommendation | Alternatives |
|----------|----------------|--------------|
| Config format | TOML | Custom DSL · Lua scripting |
| Dependency policy | System packages via pkg-config | Vendored submodules / CPM |

## 8. Dependencies

Installed: `wayland 1.26`, `wayland-protocols 1.49`, `libinput`, `libxkbcommon`,
`pixman`, `mesa`, `libdrm`, `seatd`, `libdisplay-info`, `hwdata`, `libxcb`,
`xorg-xwayland`, `pipewire`, `qt6-base`, `qt6-declarative`, `foot`, `cmake`, GCC 16,
`vulkan-icd-loader`, `vulkan-intel`, `vulkan-tools`, `shaderc` (`glslc`),
`glslang`, `spirv-tools`, `pugixml` (scanner), `gtest`.

To add: `vulkan-headers` + `vulkan-validation-layers` (M0), `tomlplusplus`
(M2), `qt6-wayland` + `layer-shell-qt` (M4), `xcb-util-wm` +
`xcb-util-errors` (M5), `sdbus-cpp` (M6), `weston` (for `weston-simple-*`
test clients, optional).

## 9. References worth reading
- KWin source: `src/backends/drm`, `src/wayland`, `src/scene`. The closest
  model to this project.
- Hyprland's Aquamarine and hyprwayland-scanner: a modern C++ example of
  leaving wlroots.
- Weston: the reference compositor; small, readable protocol implementations.
- *The Wayland Protocol* book (Drew DeVault) and `wayland.app` protocol explorer.
- Kernel docs: KMS atomic API, `drm-kms.rst`; Vulkan spec chapters on external memory, DRM format modifiers, and sync_file.
