#include "backend/headless/HeadlessBackend.h"
#include "backend/nested/NestedBackend.h"
#include "core/EventLoop.h"
#include "core/Log.h"
#include "render/Gpu.h"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>
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

// M0 placeholder content: a slow hue cycle, so a frozen frame loop is obvious.
std::array<float, 4> colorAt(std::chrono::duration<float> t)
{
    const float h = std::fmod(t.count() / 8.0f, 1.0f) * 6.0f;
    const float s = 0.6f, v = 0.5f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    const float m = v - c;
    const std::array<std::array<float, 3>, 6> sectors = {{
        {c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x},
    }};
    const auto& rgb = sectors[std::size_t(h) % 6];
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m, 1.0f};
}

void renderFrame(Gpu& gpu, Output& output, std::chrono::steady_clock::time_point start)
{
    Swapchain* swapchain = output.swapchain();
    if (!swapchain)
        return;
    SwapchainSlot* slot = swapchain->acquire();
    if (!slot) {
        log::debug("core", "{}: every buffer is still on screen, skipping a frame", output.name());
        return;
    }
    if (!gpu.renderer().clear(*slot->image, colorAt(std::chrono::steady_clock::now() - start))) {
        Swapchain::release(*slot);
        return;
    }
    output.present(*slot);
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
    auto flushClients = loop.beforeSleep.connect([&] { wl_display_flush_clients(display.get()); });

    // Declared before the backend so it's destroyed after it: outputs own GPU resources.
    std::unique_ptr<Gpu> gpu;
    auto backend = createBackend(loop);
    if (!backend)
        return EXIT_FAILURE;

    gpu = Gpu::create(backend->preferredDevice(), validationEnabled());
    if (!gpu) {
        log::error("core", "no usable GPU");
        return EXIT_FAILURE;
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<Connection> frameConnections;
    auto outputAdded = backend->outputAdded.connect([&](Output* output) {
        frameConnections.push_back(output->frame.connect([&gpu, output, start] { renderFrame(*gpu, *output, start); }));
    });
    auto backendQuit = backend->quitRequested.connect(quit);

    if (!backend->start(*gpu))
        return EXIT_FAILURE;

    const char* socket = wl_display_add_socket_auto(display.get());
    if (!socket) {
        log::error("core", "failed to create a Wayland socket");
        return EXIT_FAILURE;
    }
    log::info("core", "running: {} backend, {} output(s), WAYLAND_DISPLAY={}", backend->name(),
              backend->outputs().size(), socket);

    while (running)
        loop.dispatch(-1);

    for (const auto& output : backend->outputs())
        log::info("core", "{}: {} frames presented", output->name(), output->framesPresented());
    gpu->renderer().waitIdle();
    log::info("core", "exiting");
    return EXIT_SUCCESS;
}
