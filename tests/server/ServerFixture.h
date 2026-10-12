#pragma once

// Test harness: our real Server + a real libwayland-client in one thread,
// connected through a socketpair. Pump both ends with roundtrip().

#include "Server.h"
#include "backend/Backend.h"
#include "core/EventLoop.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <cstring>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

#include "xdg-shell-client-protocol.h"

namespace dragon::test {

// An Output without a GPU, for wl_output tests.
class FakeOutput final : public Output {
public:
    explicit FakeOutput(std::string name, OutputMode mode = {1920, 1080, 60000}) : Output(std::move(name))
    {
        setMode(mode);
    }
    void resize(std::int32_t w, std::int32_t h) { setMode({w, h, mode().refreshMhz}); }
    void present(SwapchainSlot&) override {}
};

// Client-side view of an xdg_toplevel window.
struct ClientWindow {
    wl_surface* surface = nullptr;
    xdg_surface* xdg = nullptr;
    xdg_toplevel* toplevel = nullptr;

    std::uint32_t lastSerial = 0;
    int configures = 0;
    std::int32_t width = -1;
    std::int32_t height = -1;
    std::vector<std::uint32_t> states;
    bool sawCapabilities = false;
    bool closed = false;
    Size bounds;

    bool hasState(xdg_toplevel_state s) const { return std::ranges::find(states, std::uint32_t(s)) != states.end(); }
};

class ServerFixture : public ::testing::Test {
public:
    struct DisplayDeleter {
        void operator()(wl_display* d) const { wl_display_destroy(d); }
    };

    std::unique_ptr<wl_display, DisplayDeleter> display{wl_display_create()};
    std::unique_ptr<EventLoop> loop;
    std::unique_ptr<Server> server;

    wl_display* client = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;

    // Server-side objects as they're created (removed when destroyed).
    std::vector<Surface*> surfaces;
    std::vector<XdgToplevel*> toplevels;
    std::vector<XdgPopup*> popups;
    std::vector<Connection> connections;

    void SetUp() override
    {
        log::setLevel(LogLevel::Error);
        loop = std::make_unique<EventLoop>(wl_display_get_event_loop(display.get()));
        server = std::make_unique<Server>(display.get(), *loop);
        trackCreated(server->surfaceCreated, surfaces);
        trackCreated(server->toplevelCreated, toplevels);
        trackCreated(server->popupCreated, popups);

        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        ASSERT_NE(wl_client_create(display.get(), fds[0]), nullptr);
        client = wl_display_connect_to_fd(fds[1]);
        ASSERT_NE(client, nullptr);
        bindGlobals();
    }

    void TearDown() override
    {
        if (client)
            disconnect();
        connections.clear();
        wl_display_destroy_clients(display.get());
        server.reset();
        loop.reset();
    }

    // Frees every client proxy we still track (locally, no requests), closes
    // the connection and lets the server process the hangup.
    void disconnect()
    {
        for (auto it = m_proxies.rbegin(); it != m_proxies.rend(); ++it)
            wl_proxy_destroy(*it);
        m_proxies.clear();
        wl_display_disconnect(std::exchange(client, nullptr));
        loop->dispatch(0);
    }

    template <typename T>
    T* track(T* proxy)
    {
        m_proxies.push_back(reinterpret_cast<wl_proxy*>(proxy));
        return proxy;
    }
    // Call after destroying a tracked proxy through its protocol destructor.
    template <typename T>
    void forget(T* proxy)
    {
        std::erase(m_proxies, reinterpret_cast<wl_proxy*>(proxy));
    }

    // Pumps both ends until the server handled everything and the client got
    // every reply. False once the client was disconnected by a protocol error.
    bool roundtrip()
    {
        if (wl_display_get_error(client))
            return false;
        bool done = false;
        static const wl_callback_listener listener = {
            .done = [](void* data, wl_callback* cb, std::uint32_t) {
                *static_cast<bool*>(data) = true;
                wl_callback_destroy(cb);
            },
        };
        auto* sync = wl_display_sync(client);
        wl_callback_add_listener(sync, &listener, &done);

        for (int i = 0; i < 1000 && !done; ++i) {
            wl_display_flush(client);
            loop->dispatch(0); // runs idle work (configures) and flushes clients
            if (wl_display_prepare_read(client) == 0) {
                pollfd pfd{wl_display_get_fd(client), POLLIN, 0};
                if (poll(&pfd, 1, 1) > 0)
                    wl_display_read_events(client);
                else
                    wl_display_cancel_read(client);
            }
            if (wl_display_dispatch_pending(client) < 0)
                break;
        }
        if (!done)
            wl_callback_destroy(sync); // disconnected: it will never fire
        return done && wl_display_get_error(client) == 0;
    }

    // Protocol error the client was disconnected with: {interface name, code}.
    std::pair<std::string, std::uint32_t> protocolError() const
    {
        const wl_interface* iface = nullptr;
        std::uint32_t id = 0;
        const std::uint32_t code = wl_display_get_protocol_error(client, &iface, &id);
        return {iface ? iface->name : "", code};
    }

    // A width x height XRGB8888 buffer filled with `pixel`.
    wl_buffer* makeBuffer(std::int32_t width, std::int32_t height, std::uint32_t pixel = 0xff336699)
    {
        const std::int32_t stride = width * 4;
        const std::int32_t size = stride * height;
        const int fd = memfd_create("dragon-test-shm", MFD_CLOEXEC);
        EXPECT_GE(fd, 0);
        EXPECT_EQ(ftruncate(fd, size), 0);
        auto* data = static_cast<std::uint32_t*>(mmap(nullptr, std::size_t(size), PROT_WRITE, MAP_SHARED, fd, 0));
        std::fill_n(data, width * height, pixel);
        munmap(data, std::size_t(size));

        auto* pool = wl_shm_create_pool(shm, fd, size);
        auto* buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return track(buffer);
    }

    wl_surface* makeSurface() { return track(wl_compositor_create_surface(compositor)); }

    // Creates an xdg_toplevel and does the initial commit; call roundtrip() to
    // receive the first configure.
    std::unique_ptr<ClientWindow> makeWindow()
    {
        auto w = std::make_unique<ClientWindow>();
        w->surface = makeSurface();
        w->xdg = track(xdg_wm_base_get_xdg_surface(wmBase, w->surface));
        w->toplevel = track(xdg_surface_get_toplevel(w->xdg));
        xdg_surface_add_listener(w->xdg, &kXdgSurfaceListener, w.get());
        xdg_toplevel_add_listener(w->toplevel, &kToplevelListener, w.get());
        wl_surface_commit(w->surface);
        return w;
    }

    // Acks the last configure and commits a buffer: the window is mapped.
    void mapWindow(ClientWindow& w, std::int32_t width = 200, std::int32_t height = 100)
    {
        xdg_surface_ack_configure(w.xdg, w.lastSerial);
        wl_surface_attach(w.surface, makeBuffer(width, height), 0, 0);
        wl_surface_damage_buffer(w.surface, 0, 0, width, height);
        wl_surface_commit(w.surface);
    }

private:
    template <typename T>
    void trackCreated(Signal<T*>& created, std::vector<T*>& list)
    {
        connections.push_back(created.connect([this, &list](T* obj) {
            list.push_back(obj);
            connections.push_back(obj->destroyed.connect([&list, obj] { std::erase(list, obj); }));
        }));
    }

    void bindGlobals()
    {
        static const wl_registry_listener listener = {
            .global =
                [](void* data, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t version) {
                    auto* self = static_cast<ServerFixture*>(data);
                    const std::string_view i(iface);
                    if (i == "wl_compositor")
                        self->compositor = self->track(static_cast<wl_compositor*>(
                            wl_registry_bind(reg, name, &wl_compositor_interface, version)));
                    else if (i == "wl_subcompositor")
                        self->subcompositor = self->track(static_cast<wl_subcompositor*>(
                            wl_registry_bind(reg, name, &wl_subcompositor_interface, 1)));
                    else if (i == "wl_shm")
                        self->shm = self->track(static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1)));
                    else if (i == "xdg_wm_base") {
                        self->wmBase = self->track(static_cast<xdg_wm_base*>(
                            wl_registry_bind(reg, name, &xdg_wm_base_interface, version)));
                        xdg_wm_base_add_listener(self->wmBase, &kWmBaseListener, nullptr);
                    }
                },
            .global_remove = [](void*, wl_registry*, std::uint32_t) {},
        };
        auto* registry = wl_display_get_registry(client);
        wl_registry_add_listener(registry, &listener, this);
        ASSERT_TRUE(roundtrip());
        wl_registry_destroy(registry);
        ASSERT_TRUE(compositor && subcompositor && shm && wmBase);
    }

    static constexpr xdg_wm_base_listener kWmBaseListener = {
        .ping = [](void*, xdg_wm_base* base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); },
    };
    static constexpr xdg_surface_listener kXdgSurfaceListener = {
        .configure = [](void* data, xdg_surface*, std::uint32_t serial) {
            auto* w = static_cast<ClientWindow*>(data);
            w->lastSerial = serial;
            ++w->configures;
        },
    };
    static constexpr xdg_toplevel_listener kToplevelListener = {
        .configure =
            [](void* data, xdg_toplevel*, std::int32_t width, std::int32_t height, wl_array* states) {
                auto* w = static_cast<ClientWindow*>(data);
                w->width = width;
                w->height = height;
                const auto* s = static_cast<const std::uint32_t*>(states->data);
                w->states.assign(s, s + states->size / sizeof(std::uint32_t));
            },
        .close = [](void* data, xdg_toplevel*) { static_cast<ClientWindow*>(data)->closed = true; },
        .configure_bounds = [](void* data, xdg_toplevel*, std::int32_t w, std::int32_t h) {
            static_cast<ClientWindow*>(data)->bounds = {w, h};
        },
        .wm_capabilities = [](void* data, xdg_toplevel*, wl_array*) {
            static_cast<ClientWindow*>(data)->sawCapabilities = true;
        },
    };

    std::vector<wl_proxy*> m_proxies;
};

} // namespace dragon::test
