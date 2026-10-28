// End to end: a real client maps shm windows, the compositor composites them
// onto a headless output, and we read the presented frame back.

#include "DesktopFixture.h"

using namespace dragon;

namespace {
using DesktopRenderTest = test::DesktopFixture;

constexpr std::uint32_t kRed = 0xffff0000;
constexpr std::uint32_t kGreen = 0xff00ff00;
constexpr std::uint32_t kBlue = 0xff0000ff;

} // namespace

TEST_F(DesktopRenderTest, WindowAppearsCenteredOnScreen)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, makeBuffer(50, 40, kRed), 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, 50, 40);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    ASSERT_EQ(desktop->windows().size(), 1u);
    EXPECT_EQ(desktop->windows()[0]->position, (Point{135, 100})); // (320-50)/2, (240-40)/2
    EXPECT_EQ(pixel(135, 100), 0xff0000u);
    EXPECT_EQ(pixel(184, 139), 0xff0000u);
    EXPECT_EQ(pixel(134, 100), background());
    EXPECT_EQ(pixel(185, 139), background());
    EXPECT_EQ(pixel(5, 5), background());
}

TEST_F(DesktopRenderTest, ClientGetsFrameCallbacksAndItsBufferBack)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);

    auto* buffer = makeBuffer(20, 20, kRed);
    bool released = false;
    static const wl_buffer_listener bufferListener = {
        .release = [](void* d, wl_buffer*) { *static_cast<bool*>(d) = true; },
    };
    wl_buffer_add_listener(buffer, &bufferListener, &released);

    bool frameDone = false;
    static const wl_callback_listener frameListener = {
        .done = [](void* d, wl_callback* cb, std::uint32_t) {
            *static_cast<bool*>(d) = true;
            wl_callback_destroy(cb);
        },
    };
    wl_callback_add_listener(wl_surface_frame(w->surface), &frameListener, &frameDone);
    wl_surface_attach(w->surface, buffer, 0, 0);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    EXPECT_TRUE(frameDone) << "drawn surfaces get their frame callback";
    EXPECT_TRUE(released) << "shm is copied on commit, so the buffer is released while still on screen";
}

TEST_F(DesktopRenderTest, NewBufferReplacesContent)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w, 40, 40); // default fill: 0xff336699
    pumpFrames(2);
    EXPECT_EQ(pixel(160, 120), 0x336699u);

    wl_surface_attach(w->surface, makeBuffer(40, 40, kBlue), 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, 40, 40);
    wl_surface_commit(w->surface);
    pumpFrames(2);
    EXPECT_EQ(pixel(160, 120), 0x0000ffu);
}

TEST_F(DesktopRenderTest, SubsurfaceDrawsAboveItsParent)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    auto* child = makeSurface();
    auto* sub = track(wl_subcompositor_get_subsurface(subcompositor, child, w->surface));
    wl_subsurface_set_position(sub, 5, 5);
    wl_surface_attach(child, makeBuffer(10, 10, kGreen), 0, 0);
    wl_surface_commit(child); // synchronized: applied with the parent's commit
    mapWindow(*w, 50, 40);
    pumpFrames(2);

    const Point origin = desktop->windows()[0]->position;
    EXPECT_EQ(pixel(origin.x + 6, origin.y + 6), 0x00ff00u);
    EXPECT_EQ(pixel(origin.x + 1, origin.y + 1), 0x336699u);
}

TEST_F(DesktopRenderTest, LaterWindowsStackOnTop)
{
    auto a = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(a->xdg, a->lastSerial);
    wl_surface_attach(a->surface, makeBuffer(100, 100, kRed), 0, 0);
    wl_surface_commit(a->surface);
    auto b = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(b->xdg, b->lastSerial);
    wl_surface_attach(b->surface, makeBuffer(100, 100, kBlue), 0, 0);
    wl_surface_commit(b->surface);
    pumpFrames(2);

    // b cascades 32px down-right of a; their overlap shows b.
    const Point pa = desktop->windows()[0]->position;
    EXPECT_EQ(pixel(pa.x + 50, pa.y + 50), 0x0000ffu);
    EXPECT_EQ(pixel(pa.x + 5, pa.y + 5), 0xff0000u);
}

TEST_F(DesktopRenderTest, UnmappedWindowDisappears)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w, 40, 40);
    pumpFrames(2);
    ASSERT_NE(pixel(160, 120), background());

    wl_surface_attach(w->surface, nullptr, 0, 0);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    EXPECT_TRUE(desktop->windows().empty());
    EXPECT_EQ(pixel(160, 120), background());
}

TEST_F(DesktopRenderTest, OverlayIsDrawnAboveWindows)
{
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    mapWindow(*w, 100, 100); // at (110, 70), default fill
    auto* cursor = makeSurface();
    wl_surface_attach(cursor, makeBuffer(8, 8, kGreen), 0, 0);
    wl_surface_commit(cursor);
    ASSERT_TRUE(roundtrip());
    Surface* serverCursor = surfaces.back();

    renderer->addOverlay([serverCursor] { return Desktop::RenderItem{serverCursor, {150, 100}}; });
    pumpFrames(2);

    EXPECT_EQ(pixel(152, 102), 0x00ff00u) << "on top of the window";
    EXPECT_EQ(pixel(160, 120), 0x336699u);
}
