#include "Server.h"
#include "backend/headless/HeadlessBackend.h"
#include "backend/nested/NestedBackend.h"
#include "config/Config.h"
#include "core/EventLoop.h"
#include "core/FileWatcher.h"
#include "core/Log.h"
#include "core/Process.h"
#include "core/SessionBus.h"
#include "desktop/Desktop.h"
#include "desktop/InputRouter.h"
#include "desktop/OutputRenderer.h"
#include "desktop/SurfaceTextures.h"
#include "desktop/Toplevel.h"
#include "protocols/LayerShell.h"
#include "protocols/Seat.h"
#include "protocols/XdgActivation.h"
#include "protocols/LinuxDmabuf.h"
#include "render/Gpu.h"
#include "xwayland/XwaylandServer.h"
#include "xwayland/XwaylandShell.h"
#include "xwayland/Xwm.h"
#include "render/vulkan/VulkanTexture.h"

#include <csignal>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <wayland-server-core.h>

using namespace dragon;

namespace {

struct DisplayDeleter {
    void operator()(wl_display* d) const
    {
        wl_display_destroy_clients(d);
        wl_display_destroy(d);
    }
};

std::string_view env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

bool validationEnabled()
{
    if (const auto v = env("DRAGON_VK_VALIDATION"); !v.empty())
        return v != "0";
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
}

// DRAGON_BACKEND=nested|headless. Default: nested when running inside another
// compositor, headless otherwise (DRM comes in M3).
std::unique_ptr<Backend> createBackend(EventLoop& loop)
{
    const auto choice = env("DRAGON_BACKEND");
    if (choice.empty() || choice == "nested") {
        if (auto nested = NestedBackend::connect(loop))
            return nested;
        if (choice == "nested") {
            log::error("core", "DRAGON_BACKEND=nested but no usable parent compositor");
            return nullptr;
        }
    }
    if (choice.empty() || choice == "headless") {
        if (choice.empty())
            log::info("core", "no parent compositor and no DRM backend yet: running headless");
        return std::make_unique<HeadlessBackend>(loop, HeadlessBackend::Config{});
    }
    log::error("core", "unknown DRAGON_BACKEND '{}' (expected nested or headless)", choice);
    return nullptr;
}

} // namespace

int main()
{
    log::init();
    log::info("core", "DragonDE compositor starting");

    std::unique_ptr<wl_display, DisplayDeleter> display(wl_display_create());
    if (!display) {
        log::error("core", "wl_display_create failed");
        return EXIT_FAILURE;
    }
    EventLoop loop(wl_display_get_event_loop(display.get()));

    bool running = true;
    auto quit = [&] {
        if (running)
            log::info("core", "shutdown requested");
        running = false;
    };
    auto sigint = loop.addSignal(SIGINT, quit);
    auto sigterm = loop.addSignal(SIGTERM, quit);

    Server server(display.get(), loop);
    Desktop desktop(server);

    // Declaration order is teardown order (reversed): renderers and textures
    // before the GPU, the backend (outputs) before everything that renders to it.
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<SurfaceTextures> textures;
    std::map<Output*, std::unique_ptr<OutputRenderer>> renderers;
    auto backend = createBackend(loop);
    if (!backend)
        return EXIT_FAILURE;

    gpu = Gpu::create(backend->preferredDevice(), validationEnabled());
    if (!gpu) {
        log::error("core", "no usable GPU");
        return EXIT_FAILURE;
    }

    textures = std::make_unique<SurfaceTextures>(server, *gpu);

    // Clients render on our GPU and share buffers as dma-bufs (zero-copy).
    DmabufSupport dmabuf{gpu->device(), {}, [&gpu](const DmaBuf& buf) {
                             return gpu->renderer().importTexture(buf) != nullptr; // trial import
                         }};
    for (auto [format, modifier] : gpu->sampleFormats())
        dmabuf.formats.push_back({format, modifier});
    server.enableDmabuf(std::move(dmabuf));

    // The user's config.lua: settings and shortcuts. Reloaded when it's saved
    // (or Mod+Shift+R); a broken file keeps the previous configuration.
    const auto configPath = env("DRAGON_CONFIG").empty() ? Config::defaultPath()
                                                         : std::filesystem::path(env("DRAGON_CONFIG"));
    std::function<void()> reloadConfig;
    ConfigActions actions{
        .spawn =
            [&server](const std::string& cmd) {
                // A launch token lets the app take focus once it shows up, however slowly.
                const auto token = server.activation().createCompositorToken();
                process::spawn(cmd, {"XDG_ACTIVATION_TOKEN=" + token, "DESKTOP_STARTUP_ID=" + token});
            },
        .quit =
            [&] {
                log::info("core", "quit shortcut");
                quit();
            },
        // Never reload from inside the running config: its Lua state would go away.
        .reload = [&] { loop.addIdle([&] { reloadConfig(); }); },
    };
    addWindowActions(actions, desktop);
    Config config(std::move(actions));
    // Modifier "auto": Super, except nested where the parent compositor
    // usually keeps Super shortcuts (like Super+drag) for itself.
    const bool nested = std::string_view(backend->name()) == "nested";
    if (const auto mod = env("DRAGON_MOD"); !mod.empty())
        config.setAutoModifier(std::string(mod));
    else if (nested)
        config.setAutoModifier("alt");
    reloadConfig = [&] {
        if (config.loadFile(configPath))
            log::info("core", "config loaded from {} (modifier {}, terminal {})", configPath.string(),
                      config.modifier(), config.settings().terminal);
    };
    reloadConfig();
    FileWatcher configWatcher(loop, configPath);
    auto configEdited = configWatcher.changed.connect([&] { reloadConfig(); });
    auto applyRenderSettings = [&] {
        for (auto& [output, renderer] : renderers) {
            renderer->setBackground(config.settings().background.rgba());
            renderer->setWallpaper(config.settings().wallpaper);
        }
        desktop.restyle();
    };
    // Every window's look: the config's `windows` style plus matching rules.
    desktop.setStyleResolver(
        [&config](const Toplevel& toplevel) { return config.styleFor(toplevel.appId(), toplevel.title()); });
    desktop.setLayerStyleResolver([&config](const LayerSurface& s) { return config.layerStyleFor(s.nameSpace()); });
    auto configChanged = config.changed.connect(applyRenderSettings);
    applyRenderSettings();

    // Client cursor images are drawn by us, above everything (set up once input exists).
    OutputRenderer::OverlayProvider cursorOverlay;
    OutputRenderer::OverlayProvider dragIconOverlay;

    // One output for now: new windows go in the middle of the first one.
    Connection areaFollowsMode;
    auto outputAdded = backend->outputAdded.connect([&](Output* output) {
        server.addOutput(*output);
        auto& renderer =
            renderers.emplace(output, std::make_unique<OutputRenderer>(server, *gpu, desktop, *textures, *output))
                .first->second;
        renderer->addOverlay([&dragIconOverlay] { return dragIconOverlay ? dragIconOverlay() : std::nullopt; });
        renderer->addOverlay([&cursorOverlay] { return cursorOverlay ? cursorOverlay() : std::nullopt; });
        renderer->setBackground(config.settings().background.rgba());
        renderer->setWallpaper(config.settings().wallpaper);
        if (renderers.size() == 1) {
            auto updateArea = [&desktop, output] {
                desktop.setArea({0, 0, output->mode().width, output->mode().height});
            };
            updateArea();
            areaFollowsMode = output->modeChanged.connect(updateArea);
        }
    });
    auto outputRemoved = backend->outputRemoved.connect([&](Output* output) {
        renderers.erase(output);
        server.removeOutput(*output);
    });
    auto backendQuit = backend->quitRequested.connect(quit);
    auto cursor = server.seat().cursorShapeChanged.connect([&](std::uint32_t shape) { backend->setCursorShape(shape); });
    InputRouter input(*backend, desktop, server.seat(), config);
    auto reap = loop.addSignal(SIGCHLD, [] { process::reapChildren(); });
    cursorOverlay = [&]() -> std::optional<Desktop::RenderItem> {
        Surface* surface = server.seat().cursorSurface();
        if (!surface || !input.pointerInside())
            return std::nullopt;
        const PointF p = input.pointerPosition();
        const Point at{std::int32_t(std::lround(p.x)), std::int32_t(std::lround(p.y))};
        return Desktop::RenderItem{surface, at - server.seat().cursorHotspot()};
    };
    dragIconOverlay = [&]() -> std::optional<Desktop::RenderItem> {
        Surface* icon = server.seat().dragIcon();
        if (!icon || !input.pointerInside())
            return std::nullopt;
        const PointF p = input.pointerPosition();
        return Desktop::RenderItem{icon, Point{std::int32_t(std::lround(p.x)), std::int32_t(std::lround(p.y))} +
                                             server.seat().dragIconOffset()};
    };

    // `kill -USR1 <pid>`: save the next frame of every output as a PNG.
    int screenshots = 0;
    auto screenshot = loop.addSignal(SIGUSR1, [&] {
        const auto dir = env("DRAGON_SCREENSHOT_DIR").empty() ? std::string("/tmp") : std::string(env("DRAGON_SCREENSHOT_DIR"));
        ++screenshots;
        for (auto& [output, renderer] : renderers)
            renderer->screenshot(std::format("{}/dragon-{}-{}.png", dir, output->name(), screenshots));
    });

    if (!backend->start(*gpu))
        return EXIT_FAILURE;

    const char* socket = wl_display_add_socket_auto(display.get());
    if (!socket) {
        log::error("core", "failed to create a Wayland socket");
        return EXIT_FAILURE;
    }
    log::info("core", "running: {} backend, {} output(s), WAYLAND_DISPLAY={}", backend->name(),
              backend->outputs().size(), socket);
    // X11 apps get our own X display, served by Xwayland once one connects.
    std::unique_ptr<XwaylandShell> xwaylandShell;
    std::unique_ptr<XwaylandServer> xwayland;
    std::unique_ptr<Xwm> xwm;
    Connection xwaylandStarted, xwaylandReady, xwaylandStopped;
    if (env("DRAGON_XWAYLAND") != "0") {
        xwaylandShell = std::make_unique<XwaylandShell>(server);
        xwayland = std::make_unique<XwaylandServer>(server, loop);
        xwaylandStarted = xwayland->started.connect([&](wl_client* c) { xwaylandShell->setXwaylandClient(c); });
        xwaylandReady = xwayland->ready.connect([&](int wmFd) {
            xwm = std::make_unique<Xwm>(server, loop, desktop, *xwaylandShell, wmFd);
        });
        xwaylandStopped = xwayland->stopped.connect([&] {
            xwm.reset();
            xwaylandShell->setXwaylandClient(nullptr);
        });
        if (!xwayland->listen())
            xwayland.reset();
    }

    // Everything we launch connects to us. (The nested backend is already
    // connected to its parent, so changing our own environment is safe now.)
    setenv("WAYLAND_DISPLAY", socket, 1);
    unsetenv("WAYLAND_SOCKET");
    // Our X display, or none: X11 apps must never reach the parent session's X server.
    if (xwayland)
        setenv("DISPLAY", xwayland->displayName().c_str(), 1);
    else
        unsetenv("DISPLAY");
    // Apps pick portal backends, themes and quirks by desktop name.
    setenv("XDG_CURRENT_DESKTOP", "DragonDE", 1);
    setenv("XDG_SESSION_DESKTOP", "DragonDE", 1);
    const std::string configDirs = env("XDG_CONFIG_DIRS").empty() ? "/etc/xdg" : std::string(env("XDG_CONFIG_DIRS"));
    setenv("XDG_CONFIG_DIRS", (std::string(DRAGON_DATA_DIR) + ":" + configDirs).c_str(), 1); // dragonde-portals.conf

    // Nested, the parent's session bus would start portals in the parent
    // (file choosers opening on the outer desktop). Give our apps their own
    // bus, started now so activated services inherit the environment above.
    // On real hardware dragon-session provides the bus.
    std::unique_ptr<SessionBus> sessionBus;
    const auto privateBus = env("DRAGON_PRIVATE_BUS");
    if (privateBus == "1" || (privateBus.empty() && nested)) {
        sessionBus = SessionBus::start();
        if (sessionBus)
            setenv("DBUS_SESSION_BUS_ADDRESS", sessionBus->address().c_str(), 1);
    }

    while (running)
        loop.dispatch(-1);

    for (const auto& output : backend->outputs())
        log::info("core", "{}: {} frames presented", output->name(), output->framesPresented());
    gpu->renderer().waitIdle();
    // Disconnect every client while everything that tracks their objects
    // (desktop, Xwayland shell, textures) is still alive.
    xwm.reset();
    wl_display_destroy_clients(display.get());
    desktop.setStyleResolver(nullptr); // the config goes before the desktop
    desktop.setLayerStyleResolver(nullptr);
    backend.reset(); // outputs go first, while the server can still drop their globals
    renderers.clear();
    textures.reset();
    log::info("core", "exiting");
    return EXIT_SUCCESS;
}
