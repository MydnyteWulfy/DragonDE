// dragon-demo-client: a minimal xdg_toplevel drawn with wl_shm.
//
// Animates a gradient driven by frame callbacks, follows configure sizes and
// quits on close. Useful to see the compositor do something without a full
// toolkit. Usage: WAYLAND_DISPLAY=wayland-1 dragon-demo-client [title]

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

namespace {

volatile std::sig_atomic_t g_quit = 0;

struct Buffer {
    wl_buffer* buffer = nullptr;
    std::uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    bool busy = false;
};

struct App {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;

    int width = 480, height = 320;
    int pendingWidth = 0, pendingHeight = 0;
    bool configured = false;
    bool running = true;
    std::uint32_t frames = 0;
    Buffer buffers[2];
    wl_callback* frameCallback = nullptr; // in flight between commit and done
};

void destroyBuffer(Buffer& b)
{
    if (b.buffer) {
        wl_buffer_destroy(b.buffer);
        munmap(b.pixels, std::size_t(b.width) * std::size_t(b.height) * 4);
    }
    b = {};
}

const wl_buffer_listener kBufferListener = {
    .release = [](void* data, wl_buffer*) { static_cast<Buffer*>(data)->busy = false; },
};

Buffer* freeBuffer(App& app)
{
    for (auto& b : app.buffers) {
        if (b.busy)
            continue;
        if (b.width != app.width || b.height != app.height) {
            destroyBuffer(b);
            const int stride = app.width * 4, size = stride * app.height;
            const int fd = memfd_create("dragon-demo", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, size) != 0)
                return nullptr;
            b.pixels = static_cast<std::uint32_t*>(mmap(nullptr, std::size_t(size), PROT_READ | PROT_WRITE,
                                                        MAP_SHARED, fd, 0));
            auto* pool = wl_shm_create_pool(app.shm, fd, size);
            b.buffer = wl_shm_pool_create_buffer(pool, 0, app.width, app.height, stride, WL_SHM_FORMAT_XRGB8888);
            wl_shm_pool_destroy(pool);
            close(fd);
            b.width = app.width;
            b.height = app.height;
            wl_buffer_add_listener(b.buffer, &kBufferListener, &b);
        }
        return &b;
    }
    return nullptr;
}

void draw(App& app);

const wl_callback_listener kFrameListener = {
    .done = [](void* data, wl_callback* cb, std::uint32_t) {
        auto& app = *static_cast<App*>(data);
        wl_callback_destroy(cb);
        app.frameCallback = nullptr;
        draw(app);
    },
};

void draw(App& app)
{
    Buffer* b = freeBuffer(app);
    if (!b) {
        std::fprintf(stderr, "demo: no free buffer\n");
        return;
    }
    const float t = float(app.frames++) / 60.0f;
    for (int y = 0; y < b->height; ++y) {
        for (int x = 0; x < b->width; ++x) {
            const float fx = float(x) / float(b->width), fy = float(y) / float(b->height);
            const auto r = std::uint32_t(127.5f + 127.5f * std::sin(t + fx * 3.0f));
            const auto g = std::uint32_t(127.5f + 127.5f * std::sin(t * 1.3f + fy * 3.0f));
            const auto bl = std::uint32_t(127.5f + 127.5f * std::sin(t * 0.7f + (fx + fy) * 2.0f));
            b->pixels[y * b->width + x] = 0xff000000 | (r << 16) | (g << 8) | bl;
        }
    }
    // A white border makes the window edges obvious.
    for (int x = 0; x < b->width; ++x)
        b->pixels[x] = b->pixels[(b->height - 1) * b->width + x] = 0xffffffff;
    for (int y = 0; y < b->height; ++y)
        b->pixels[y * b->width] = b->pixels[y * b->width + b->width - 1] = 0xffffffff;

    wl_surface_attach(app.surface, b->buffer, 0, 0);
    wl_surface_damage_buffer(app.surface, 0, 0, b->width, b->height);
    app.frameCallback = wl_surface_frame(app.surface);
    wl_callback_add_listener(app.frameCallback, &kFrameListener, &app);
    wl_surface_commit(app.surface);
    b->busy = true;
}

const xdg_wm_base_listener kWmBaseListener = {
    .ping = [](void*, xdg_wm_base* base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); },
};

const xdg_surface_listener kXdgSurfaceListener = {
    .configure = [](void* data, xdg_surface* xdg, std::uint32_t serial) {
        auto& app = *static_cast<App*>(data);
        xdg_surface_ack_configure(xdg, serial);
        if (app.pendingWidth > 0 && app.pendingHeight > 0) {
            app.width = app.pendingWidth;
            app.height = app.pendingHeight;
        }
        if (!app.configured) {
            app.configured = true;
            draw(app); // later frames are driven by frame callbacks
        }
    },
};

const xdg_toplevel_listener kToplevelListener = {
    .configure = [](void* data, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array*) {
        auto& app = *static_cast<App*>(data);
        app.pendingWidth = w;
        app.pendingHeight = h;
    },
    .close = [](void* data, xdg_toplevel*) { static_cast<App*>(data)->running = false; },
    .configure_bounds = [](void*, xdg_toplevel*, std::int32_t, std::int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

const wl_registry_listener kRegistryListener = {
    .global =
        [](void* data, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
            auto& app = *static_cast<App*>(data);
            if (std::strcmp(iface, wl_compositor_interface.name) == 0)
                app.compositor = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
            else if (std::strcmp(iface, wl_shm_interface.name) == 0)
                app.shm = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
            else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
                app.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
                xdg_wm_base_add_listener(app.wmBase, &kWmBaseListener, nullptr);
            }
        },
    .global_remove = [](void*, wl_registry*, std::uint32_t) {},
};

} // namespace

int main(int argc, char** argv)
{
    App app;
    app.display = wl_display_connect(nullptr);
    if (!app.display) {
        std::fprintf(stderr, "demo: cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)\n");
        return 1;
    }
    auto* registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(registry, &kRegistryListener, &app);
    wl_display_roundtrip(app.display);
    if (!app.compositor || !app.shm || !app.wmBase) {
        std::fprintf(stderr, "demo: compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        return 1;
    }

    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdg = xdg_wm_base_get_xdg_surface(app.wmBase, app.surface);
    xdg_surface_add_listener(app.xdg, &kXdgSurfaceListener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg);
    xdg_toplevel_add_listener(app.toplevel, &kToplevelListener, &app);
    const std::string title = argc > 1 ? argv[1] : "DragonDE demo";
    xdg_toplevel_set_title(app.toplevel, title.c_str());
    xdg_toplevel_set_app_id(app.toplevel, "org.dragonde.demo");
    wl_surface_commit(app.surface);

    // No SA_RESTART: a signal interrupts the blocking dispatch so we exit cleanly.
    struct sigaction sa {};
    sa.sa_handler = [](int) { g_quit = 1; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    while (app.running && !g_quit && wl_display_dispatch(app.display) >= 0) {
    }
    std::printf("demo: drew %u frames\n", app.frames);

    if (app.frameCallback)
        wl_callback_destroy(app.frameCallback);
    for (auto& b : app.buffers)
        destroyBuffer(b);
    xdg_toplevel_destroy(app.toplevel);
    xdg_surface_destroy(app.xdg);
    wl_surface_destroy(app.surface);
    xdg_wm_base_destroy(app.wmBase);
    wl_shm_destroy(app.shm);
    wl_compositor_destroy(app.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(app.display);
    return 0;
}
