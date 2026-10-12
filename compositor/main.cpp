#include "core/EventLoop.h"
#include "core/Log.h"

#include <csignal>
#include <cstdlib>
#include <memory>
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

    auto quit = [&] {
        log::info("core", "shutdown requested");
        wl_display_terminate(display.get());
    };
    auto sigint = loop.addSignal(SIGINT, quit);
    auto sigterm = loop.addSignal(SIGTERM, quit);

    const char* socket = wl_display_add_socket_auto(display.get());
    if (!socket) {
        log::error("core", "failed to create a Wayland socket");
        return EXIT_FAILURE;
    }
    log::info("core", "listening on WAYLAND_DISPLAY={}", socket);

    wl_display_run(display.get());

    log::info("core", "exiting");
    return EXIT_SUCCESS;
}
