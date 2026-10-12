// End-to-end tests for dragon-scanner output: a real libwayland-client talks to
// generated server classes over a socketpair, all in one thread.

#include "core/Log.h"
#include "dragon-protocols/wayland.h"
#include "wayland/Global.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

using namespace dragon;

namespace {

class TestSurface : public proto::WlSurface {
public:
    using WlSurface::WlSurface;

    int scale = 1;
    proto::WlOutputTransform transform = proto::WlOutputTransform::Normal;
    int commits = 0;

protected:
    void handleSetBufferScale(std::int32_t s) override { scale = s; }
    void handleSetBufferTransform(proto::WlOutputTransform t) override { transform = t; }
    void handleCommit() override { ++commits; }
};

struct ProtocolFixture;

class TestCompositor : public proto::WlCompositor {
public:
    TestCompositor(wl_client* c, std::uint32_t v, std::uint32_t id, ProtocolFixture* f)
        : WlCompositor(c, v, id), fixture(f) {}

    ProtocolFixture* fixture;

protected:
    void handleCreateSurface(std::uint32_t id) override;
};

struct ProtocolFixture : ::testing::Test {
    wl_display* server = nullptr;
    wl_display* client = nullptr;
    std::unique_ptr<Global> compositorGlobal;

    std::vector<TestSurface*> surfaces;
    std::vector<Connection> connections;

    void SetUp() override
    {
        log::setLevel(LogLevel::Warn);
        server = wl_display_create();
        compositorGlobal = Global::create<proto::WlCompositor>(
            server, 6, [this](wl_client* c, std::uint32_t version, std::uint32_t id) {
                makeResource<TestCompositor>(c, version, id, this);
            });

        int fds[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        ASSERT_NE(wl_client_create(server, fds[0]), nullptr);
        client = wl_display_connect_to_fd(fds[1]);
        ASSERT_NE(client, nullptr);
    }

    void TearDown() override
    {
        if (client)
            disconnectClient();
        compositorGlobal.reset(); // before the display, which would free the wl_global itself
        wl_display_destroy_clients(server);
        wl_display_destroy(server);
    }

    void disconnectClient()
    {
        wl_display_disconnect(std::exchange(client, nullptr));
        wl_event_loop_dispatch(wl_display_get_event_loop(server), 0); // server notices the hangup
    }

    // Pumps both ends until the server has processed everything the client sent
    // and the client has received every reply. Returns false on a protocol error.
    bool roundtrip()
    {
        bool done = false;
        static const wl_callback_listener listener = {
            .done = [](void* data, wl_callback* cb, std::uint32_t) {
                *static_cast<bool*>(data) = true;
                wl_callback_destroy(cb);
            },
        };
        wl_callback_add_listener(wl_display_sync(client), &listener, &done);

        for (int i = 0; i < 1000 && !done; ++i) {
            wl_display_flush(client);
            wl_event_loop_dispatch(wl_display_get_event_loop(server), 0);
            wl_display_flush_clients(server);

            if (wl_display_prepare_read(client) == 0) {
                pollfd pfd{wl_display_get_fd(client), POLLIN, 0};
                if (poll(&pfd, 1, 1) > 0)
                    wl_display_read_events(client);
                else
                    wl_display_cancel_read(client);
            }
            if (wl_display_dispatch_pending(client) < 0)
                return false;
        }
        return done && wl_display_get_error(client) == 0;
    }

    wl_compositor* bindCompositor(std::uint32_t version)
    {
        struct Bind {
            std::uint32_t version;
            wl_compositor* compositor = nullptr;
        } bind{version};

        static const wl_registry_listener listener = {
            .global = [](void* data, wl_registry* reg, std::uint32_t name, const char* iface,
                         std::uint32_t) {
                auto* b = static_cast<Bind*>(data);
                if (std::string_view(iface) == "wl_compositor")
                    b->compositor = static_cast<wl_compositor*>(
                        wl_registry_bind(reg, name, &wl_compositor_interface, b->version));
            },
            .global_remove = [](void*, wl_registry*, std::uint32_t) {},
        };

        auto* registry = wl_display_get_registry(client);
        wl_registry_add_listener(registry, &listener, &bind);
        EXPECT_TRUE(roundtrip());
        wl_registry_destroy(registry);
        return bind.compositor;
    }
};

void TestCompositor::handleCreateSurface(std::uint32_t id)
{
    auto* surface = makeResource<TestSurface>(client(), version(), id);
    if (!surface)
        return;
    auto& list = fixture->surfaces;
    list.push_back(surface);
    fixture->connections.push_back(surface->destroyed.connect([&list, surface] { std::erase(list, surface); }));
}

} // namespace

TEST_F(ProtocolFixture, RequestsReachTypedHandlers)
{
    auto* compositor = bindCompositor(6);
    ASSERT_NE(compositor, nullptr);

    auto* surface = wl_compositor_create_surface(compositor);
    wl_surface_set_buffer_scale(surface, 2);
    wl_surface_set_buffer_transform(surface, WL_OUTPUT_TRANSFORM_90);
    wl_surface_commit(surface);
    ASSERT_TRUE(roundtrip());

    ASSERT_EQ(surfaces.size(), 1u);
    EXPECT_EQ(surfaces[0]->scale, 2);
    EXPECT_EQ(surfaces[0]->transform, proto::WlOutputTransform::_90); // int arg with enum= became our enum class
    EXPECT_EQ(surfaces[0]->commits, 1);
    EXPECT_EQ(surfaces[0]->version(), 6u);

    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
}

TEST_F(ProtocolFixture, DestructorRequestDeletesServerObject)
{
    auto* compositor = bindCompositor(6);
    auto* surface = wl_compositor_create_surface(compositor);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces.size(), 1u);

    wl_surface_destroy(surface);
    ASSERT_TRUE(roundtrip());

    EXPECT_TRUE(surfaces.empty()); // `destroyed` fired, object deleted (ASan checks the rest)
    wl_compositor_destroy(compositor);
}

TEST_F(ProtocolFixture, ClientDisconnectDeletesAllItsObjects)
{
    auto* compositor = bindCompositor(6);
    auto* a = wl_compositor_create_surface(compositor);
    auto* b = wl_compositor_create_surface(compositor);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces.size(), 2u);

    // Free client-side proxies without sending anything, as if the client crashed.
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(a));
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(b));
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(compositor));
    disconnectClient();

    EXPECT_TRUE(surfaces.empty());
}

TEST_F(ProtocolFixture, EventsReachClient)
{
    auto* compositor = bindCompositor(6);
    auto* surface = wl_compositor_create_surface(compositor);

    int receivedScale = 0;
    static const wl_surface_listener listener = {
        .enter = [](void*, wl_surface*, wl_output*) {},
        .leave = [](void*, wl_surface*, wl_output*) {},
        .preferred_buffer_scale = [](void* data, wl_surface*, std::int32_t f) { *static_cast<int*>(data) = f; },
        .preferred_buffer_transform = [](void*, wl_surface*, std::uint32_t) {},
    };
    wl_surface_add_listener(surface, &listener, &receivedScale);
    ASSERT_TRUE(roundtrip());

    surfaces[0]->sendPreferredBufferScale(3);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(receivedScale, 3);
    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
}

TEST_F(ProtocolFixture, EventsNewerThanBoundVersionAreDropped)
{
    auto* compositor = bindCompositor(5); // preferred_buffer_scale is since v6
    auto* surface = wl_compositor_create_surface(compositor);
    ASSERT_TRUE(roundtrip());
    ASSERT_EQ(surfaces[0]->version(), 5u);

    surfaces[0]->sendPreferredBufferScale(3);

    // Sending a v6 event to a v5 object would be a fatal client-side error.
    EXPECT_TRUE(roundtrip());
    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
}

TEST_F(ProtocolFixture, FromResourceChecksType)
{
    auto* compositor = bindCompositor(6);
    auto* surface = wl_compositor_create_surface(compositor);
    ASSERT_TRUE(roundtrip());

    wl_resource* res = surfaces[0]->resource();
    EXPECT_EQ(proto::WlSurface::fromResource(res), surfaces[0]);
    EXPECT_EQ(proto::WlCompositor::fromResource(res), nullptr);
    EXPECT_EQ(proto::WlSurface::fromResource(nullptr), nullptr);

    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
}

TEST_F(ProtocolFixture, ServerSideDestroyDeletesObject)
{
    auto* compositor = bindCompositor(6);
    auto* surface = wl_compositor_create_surface(compositor);
    ASSERT_TRUE(roundtrip());

    surfaces[0]->destroy();
    EXPECT_TRUE(surfaces.empty());

    // The client must not use the object after the server destroyed it (the
    // protocol only allows this for objects like wl_callback), so just drop
    // the proxy locally.
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(surface));
    EXPECT_TRUE(roundtrip());
    wl_compositor_destroy(compositor);
}

TEST(Bitfield, OperatorsWorkOnGeneratedBitfields)
{
    using Cap = proto::WlSeatCapability;
    constexpr auto caps = Cap::Pointer | Cap::Keyboard;

    static_assert(proto::hasFlags(caps, Cap::Pointer));
    static_assert(!proto::hasFlags(caps, Cap::Touch));
    static_assert(std::uint32_t(caps) == 3);
}
