// xdg_wm_base / xdg_surface / xdg_toplevel / xdg_popup and wl_output.

#include "ServerFixture.h"

#include "protocols/Output.h"

using namespace dragon;
using dragon::test::ClientWindow;
using dragon::test::FakeOutput;
using dragon::test::ServerFixture;

namespace {
using XdgShellTest = ServerFixture;
using Error = std::pair<std::string, std::uint32_t>;
} // namespace

TEST_F(XdgShellTest, InitialCommitGetsAConfigure)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(w->configures, 1);
    EXPECT_EQ(w->width, 0) << "0x0: the client picks its size";
    EXPECT_EQ(w->height, 0);
    EXPECT_TRUE(w->states.empty());
    EXPECT_TRUE(w->sawCapabilities);
    ASSERT_EQ(toplevels.size(), 1u);
}

TEST_F(XdgShellTest, AckThenBufferMapsTheWindow)
{
    auto w = makeWindow();
    xdg_toplevel_set_title(w->toplevel, "Hello");
    xdg_toplevel_set_app_id(w->toplevel, "org.dragon.test");
    ASSERT_TRUE(roundtrip());
    XdgToplevel* tl = toplevels[0];
    int mapped = 0;
    auto c = tl->xdgSurface()->mapped.connect([&] { ++mapped; });

    mapWindow(*w, 300, 200);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(mapped, 1);
    EXPECT_TRUE(tl->xdgSurface()->isMapped());
    EXPECT_EQ(tl->title(), "Hello");
    EXPECT_EQ(tl->appId(), "org.dragon.test");
    EXPECT_EQ(tl->xdgSurface()->geometry(), (Rect{0, 0, 300, 200}));
}

TEST_F(XdgShellTest, WindowGeometryIsDoubleBuffered)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w, 300, 200);
    xdg_surface_set_window_geometry(w->xdg, 10, 10, 280, 180);
    ASSERT_TRUE(roundtrip());
    auto* xdg = toplevels[0]->xdgSurface();
    EXPECT_EQ(xdg->geometry(), (Rect{0, 0, 300, 200}));

    wl_surface_commit(w->surface);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(xdg->geometry(), (Rect{10, 10, 280, 180}));
}

TEST_F(XdgShellTest, BufferBeforeFirstAckIsAnError)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());

    wl_surface_attach(w->surface, makeBuffer(10, 10), 0, 0);
    wl_surface_commit(w->surface);

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (Error{"xdg_surface", XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER}));
}

TEST_F(XdgShellTest, AckingAnUnknownSerialIsAnError)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());

    xdg_surface_ack_configure(w->xdg, w->lastSerial + 1000);

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (Error{"xdg_surface", XDG_SURFACE_ERROR_INVALID_SERIAL}));
}

TEST_F(XdgShellTest, CompositorChangesAreCoalescedIntoOneConfigure)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w);
    ASSERT_TRUE(roundtrip());
    XdgToplevel* tl = toplevels[0];

    tl->setSize({800, 600});
    tl->setActivated(true);
    tl->setMaximized(true);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(w->configures, 2);
    EXPECT_EQ(w->width, 800);
    EXPECT_EQ(w->height, 600);
    EXPECT_TRUE(w->hasState(XDG_TOPLEVEL_STATE_ACTIVATED));
    EXPECT_TRUE(w->hasState(XDG_TOPLEVEL_STATE_MAXIMIZED));
    EXPECT_FALSE(tl->currentConfigure().activated) << "not acked yet";

    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_commit(w->surface);
    ASSERT_TRUE(roundtrip());
    EXPECT_TRUE(tl->currentConfigure().activated);
    EXPECT_EQ(tl->currentConfigure().size, (Size{800, 600}));
}

TEST_F(XdgShellTest, NullBufferUnmapsAndRestartsTheHandshake)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w);
    ASSERT_TRUE(roundtrip());
    auto* xdg = toplevels[0]->xdgSurface();
    int unmapped = 0;
    auto c = xdg->unmapped.connect([&] { ++unmapped; });

    wl_surface_attach(w->surface, nullptr, 0, 0);
    wl_surface_commit(w->surface);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(unmapped, 1);
    EXPECT_FALSE(xdg->isMapped());
    const int before = w->configures;

    wl_surface_commit(w->surface); // a new initial commit
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(w->configures, before + 1);
}

TEST_F(XdgShellTest, CloseRequestReachesClient)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());

    toplevels[0]->requestClose();
    ASSERT_TRUE(roundtrip());

    EXPECT_TRUE(w->closed);
}

// move/resize need a wl_seat (stage 4); they get tested with input.
TEST_F(XdgShellTest, MaximizeRequestIsForwarded)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    XdgToplevel* tl = toplevels[0];
    std::optional<bool> maximize;
    auto c = tl->maximizeRequested.connect([&](bool on) { maximize = on; });
    const int before = w->configures;

    xdg_toplevel_set_maximized(w->toplevel);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(maximize, true);
    EXPECT_EQ(w->configures, before + 1) << "set_maximized always gets a configure in reply";
}

TEST_F(XdgShellTest, DestroyingRoleObjectUnmaps)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w);
    ASSERT_TRUE(roundtrip());
    auto* xdg = toplevels[0]->xdgSurface();
    int unmapped = 0;
    auto c = xdg->unmapped.connect([&] { ++unmapped; });

    xdg_toplevel_destroy(w->toplevel);
    forget(w->toplevel);
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(unmapped, 1);
    EXPECT_TRUE(toplevels.empty());
}

TEST_F(XdgShellTest, DestroyingWlSurfaceFirstDoesNotCrash)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w);
    ASSERT_TRUE(roundtrip());

    wl_surface_destroy(w->surface);
    forget(w->surface);
    ASSERT_TRUE(roundtrip());
    xdg_toplevel_set_title(w->toplevel, "still alive"); // inert, ignored
    EXPECT_TRUE(roundtrip());
}

TEST_F(XdgShellTest, PopupIsPlacedByItsPositioner)
{
    auto parent = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*parent);
    ASSERT_TRUE(roundtrip());

    auto* positioner = track(xdg_wm_base_create_positioner(wmBase));
    xdg_positioner_set_size(positioner, 100, 50);
    xdg_positioner_set_anchor_rect(positioner, 10, 10, 20, 20);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(positioner, 2, 3);

    auto* surface = makeSurface();
    auto* xdg = track(xdg_wm_base_get_xdg_surface(wmBase, surface));
    auto* popup = track(xdg_surface_get_popup(xdg, parent->xdg, positioner));

    struct PopupState {
        std::int32_t x = -1, y = -1, w = -1, h = -1;
        std::uint32_t serial = 0;
    } state;
    static const xdg_popup_listener popupListener = {
        .configure = [](void* d, xdg_popup*, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) {
            auto* s = static_cast<PopupState*>(d);
            s->x = x, s->y = y, s->w = w, s->h = h;
        },
        .popup_done = [](void*, xdg_popup*) {},
        .repositioned = [](void*, xdg_popup*, std::uint32_t) {},
    };
    static const xdg_surface_listener surfaceListener = {
        .configure = [](void* d, xdg_surface*, std::uint32_t serial) { static_cast<PopupState*>(d)->serial = serial; },
    };
    xdg_popup_add_listener(popup, &popupListener, &state);
    xdg_surface_add_listener(xdg, &surfaceListener, &state);
    wl_surface_commit(surface);
    ASSERT_TRUE(roundtrip());

    // Anchor point (30, 30), extending right/down, plus the offset.
    EXPECT_EQ(state.x, 32);
    EXPECT_EQ(state.y, 33);
    EXPECT_EQ(state.w, 100);
    EXPECT_EQ(state.h, 50);
    ASSERT_EQ(popups.size(), 1u);
    EXPECT_EQ(popups[0]->parent(), toplevels[0]->xdgSurface());
}

TEST_F(XdgShellTest, IncompletePositionerIsAnError)
{
    auto parent = makeWindow();
    ASSERT_TRUE(roundtrip());
    auto* positioner = track(xdg_wm_base_create_positioner(wmBase));
    xdg_positioner_set_size(positioner, 100, 50); // no anchor rect
    auto* surface = makeSurface();
    auto* xdg = track(xdg_wm_base_get_xdg_surface(wmBase, surface));
    track(xdg_surface_get_popup(xdg, parent->xdg, positioner));

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (Error{"xdg_positioner", XDG_POSITIONER_ERROR_INVALID_INPUT}));
}

TEST_F(XdgShellTest, OutputAdvertisesModeAndUpdates)
{
    FakeOutput output("TEST-1");
    server->addOutput(output);

    struct OutputState {
        ServerFixture* fixture;
        std::int32_t width = 0, height = 0, refresh = 0;
        std::string name;
        int done = 0;
    } state{this};
    static const wl_output_listener listener = {
        .geometry = [](void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                       const char*, const char*, std::int32_t) {},
        .mode = [](void* d, wl_output*, std::uint32_t, std::int32_t w, std::int32_t h, std::int32_t r) {
            auto* s = static_cast<OutputState*>(d);
            s->width = w, s->height = h, s->refresh = r;
        },
        .done = [](void* d, wl_output*) { ++static_cast<OutputState*>(d)->done; },
        .scale = [](void*, wl_output*, std::int32_t) {},
        .name = [](void* d, wl_output*, const char* n) { static_cast<OutputState*>(d)->name = n; },
        .description = [](void*, wl_output*, const char*) {},
    };

    static const wl_registry_listener registryListener = {
        .global =
            [](void* d, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
                if (std::string_view(iface) == "wl_output") {
                    auto* out = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
                    static_cast<OutputState*>(d)->fixture->track(out);
                    wl_output_add_listener(out, &listener, d);
                }
            },
        .global_remove = [](void*, wl_registry*, std::uint32_t) {},
    };
    auto* registry = track(wl_display_get_registry(client));
    wl_registry_add_listener(registry, &registryListener, &state);
    ASSERT_TRUE(roundtrip());
    ASSERT_TRUE(roundtrip());

    EXPECT_EQ(state.width, 1920);
    EXPECT_EQ(state.height, 1080);
    EXPECT_EQ(state.refresh, 60000);
    EXPECT_EQ(state.name, "TEST-1");
    EXPECT_EQ(state.done, 1);

    output.resize(1280, 720);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(state.width, 1280);
    EXPECT_EQ(state.done, 2);

    server->removeOutput(output);
}
