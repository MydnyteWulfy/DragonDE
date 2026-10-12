#include "Server.h"
#include "backend/headless/HeadlessBackend.h"
#include "backend/nested/NestedBackend.h"
#include "core/EventLoop.h"
#include "core/Log.h"
#include "core/Process.h"
#include "desktop/Desktop.h"
#include "desktop/InputRouter.h"
#include "desktop/OutputRenderer.h"
#include "desktop/SurfaceTextures.h"
#include "protocols/LinuxDmabuf.h"
#include "render/Gpu.h"
#include "render/vulkan/VulkanTexture.h"

#include <csignal>
#include <cstdlib>
#include <format>
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

    // One output for now: new windows go in the middle of the first one.
    Connection areaFollowsMode;
    auto outputAdded = backend->outputAdded.connect([&](Output* output) {
        server.addOutput(*output);
        renderers.emplace(output, std::make_unique<OutputRenderer>(server, *gpu, desktop, *textures, *output));
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
    // Shortcut modifier: Super, except nested where the parent compositor
    // usually keeps Super shortcuts (like Super+drag) for itself.
    WindowManagerConfig wm;
    const auto mod = env("DRAGON_MOD");
    const bool nested = std::string_view(backend->name()) == "nested";
    if (mod == "alt" || (mod.empty() && nested))
        wm.modifier = "Mod1", wm.modifierLabel = "Alt";
    else if (mod == "ctrl")
        wm.modifier = "Control", wm.modifierLabel = "Ctrl";
    if (const auto terminal = env("DRAGON_TERMINAL"); !terminal.empty())
        wm.terminal = terminal;
    InputRouter input(*backend, desktop, server.seat(), wm,
                      {.quit = quit, .spawn = [](const std::string& cmd) { process::spawn(cmd); }});
    auto reap = loop.addSignal(SIGCHLD, [] { process::reapChildren(); });

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
    // Everything we launch connects to us. (The nested backend is already
    // connected to its parent, so changing our own environment is safe now.)
    setenv("WAYLAND_DISPLAY", socket, 1);
    unsetenv("WAYLAND_SOCKET");
    // No XWayland yet: without this, X11-capable apps would open on the
    // parent session's X server instead of failing or using Wayland.
    unsetenv("DISPLAY");

    while (running)
        loop.dispatch(-1);

    for (const auto& output : backend->outputs())
        log::info("core", "{}: {} frames presented", output->name(), output->framesPresented());
    gpu->renderer().waitIdle();
    backend.reset(); // outputs go first, while the server can still drop their globals
    renderers.clear();
    textures.reset();
    log::info("core", "exiting");
    return EXIT_SUCCESS;
}
