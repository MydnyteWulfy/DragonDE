// End to end: a real client maps shm windows, the compositor composites them
// onto a headless output, and we read the presented frame back.

#include "DesktopFixture.h"

#include "core/Png.h"
#include "desktop/Toplevel.h"

#include <filesystem>
#include <unistd.h>

#include "xdg-output-unstable-v1-client-protocol.h"

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

// ------------------------------------------------------------------ styles --

namespace {
std::int32_t channel(std::uint32_t rgb, int shift)
{
    return std::int32_t((rgb >> shift) & 0xff);
}
} // namespace

TEST_F(DesktopRenderTest, StyledWindowsGetRoundedCornersAndABorder)
{
    WindowStyle style = WindowStyle::plain();
    style.cornerRadius = 10;
    style.border.width = 3;
    style.border.active = {*Color::parse("#0000ff")};
    style.border.inactive = style.border.active;
    desktop->setStyleResolver([style](const Toplevel&) { return style; });

    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, makeBuffer(50, 40, kRed), 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, 50, 40);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    const Point p = desktop->windows()[0]->position;
    EXPECT_EQ(pixel(p.x + 25, p.y + 20), 0xff0000u) << "the middle is untouched";
    EXPECT_EQ(pixel(p.x, p.y + 20), 0xff0000u) << "straight edges aren't cut";
    EXPECT_NE(pixel(p.x, p.y), 0xff0000u) << "the corner is rounded off";
    EXPECT_EQ(pixel(p.x - 2, p.y + 20), 0x0000ffu) << "the border sits outside the window";
    EXPECT_EQ(pixel(p.x + 25, p.y + 41), 0x0000ffu);
    EXPECT_EQ(pixel(p.x - 5, p.y + 20), background()) << "and is 3px wide";
}

TEST_F(DesktopRenderTest, StyledWindowsHideClientShadowsAndFade)
{
    WindowStyle style = WindowStyle::plain();
    style.cornerRadius = 1; // styled, barely rounded
    style.activeOpacity = style.inactiveOpacity = 0.5;
    desktop->setStyleResolver([style](const Toplevel&) { return style; });

    // 10px of "shadow" (the whole buffer is red) around a 30x20 window.
    auto w = makeWindow();
    xdg_surface_set_window_geometry(w->xdg, 10, 10, 30, 20);
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, makeBuffer(50, 40, kRed), 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, 50, 40);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    const Point p = desktop->windows()[0]->position;
    EXPECT_EQ(pixel(p.x - 5, p.y + 10), background()) << "outside the window geometry";

    // Half red over the background.
    const std::uint32_t c = pixel(p.x + 15, p.y + 10), bg = background();
    EXPECT_NEAR(channel(c, 16), (255 + channel(bg, 16)) / 2, 2);
    EXPECT_NEAR(channel(c, 8), channel(bg, 8) / 2, 2);
}

TEST_F(DesktopRenderTest, XdgOutputDescribesTheOutput)
{
    struct Found {
        wl_output* output = nullptr;
        zxdg_output_manager_v1* manager = nullptr;
    } found;
    static const wl_registry_listener listener = {
        .global =
            [](void* d, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
                auto* f = static_cast<Found*>(d);
                if (std::string_view(iface) == "wl_output")
                    f->output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
                else if (std::string_view(iface) == "zxdg_output_manager_v1")
                    f->manager = static_cast<zxdg_output_manager_v1*>(
                        wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 3));
            },
        .global_remove = [](void*, wl_registry*, std::uint32_t) {},
    };
    auto* registry = wl_display_get_registry(client);
    wl_registry_add_listener(registry, &listener, &found);
    ASSERT_TRUE(roundtrip());
    wl_registry_destroy(registry);
    ASSERT_TRUE(found.output && found.manager);
    track(found.output);
    track(found.manager);

    struct Info {
        std::string name;
        std::int32_t width = 0, height = 0;
    } info;
    static const zxdg_output_v1_listener xdgListener = {
        .logical_position = [](void*, zxdg_output_v1*, std::int32_t, std::int32_t) {},
        .logical_size =
            [](void* d, zxdg_output_v1*, std::int32_t w, std::int32_t h) {
                static_cast<Info*>(d)->width = w;
                static_cast<Info*>(d)->height = h;
            },
        .done = [](void*, zxdg_output_v1*) {},
        .name = [](void* d, zxdg_output_v1*, const char* name) { static_cast<Info*>(d)->name = name; },
        .description = [](void*, zxdg_output_v1*, const char*) {},
    };
    auto* xdg = track(zxdg_output_manager_v1_get_xdg_output(found.manager, found.output));
    zxdg_output_v1_add_listener(xdg, &xdgListener, &info);
    ASSERT_TRUE(roundtrip());
    EXPECT_EQ(info.name, output->name());
    EXPECT_EQ(info.width, 320);
    EXPECT_EQ(info.height, 240);
}

// ----------------------------------------------------------------- effects --

namespace {
// Maps a solid w x h window and returns it (the desktop centers it).
std::unique_ptr<test::ClientWindow> solidWindow(test::DesktopFixture& f, std::int32_t w, std::int32_t h,
                                                std::uint32_t color)
{
    auto win = f.makeWindow();
    EXPECT_TRUE(f.roundtrip());
    xdg_surface_ack_configure(win->xdg, win->lastSerial);
    wl_surface_attach(win->surface, f.makeBuffer(w, h, color), 0, 0);
    wl_surface_damage_buffer(win->surface, 0, 0, w, h);
    wl_surface_commit(win->surface);
    EXPECT_TRUE(f.roundtrip());
    return win;
}
} // namespace

TEST_F(DesktopRenderTest, InactiveWindowsAreDimmedAndGlowsSurroundTheActiveOne)
{
    WindowStyle style = WindowStyle::plain();
    style.dim.strength = 0.5;
    style.glow.enabled = true;
    style.glow.size = 10;
    style.glow.active = {0, 1, 0, 1};
    desktop->setStyleResolver([style](const Toplevel&) { return style; });

    auto a = solidWindow(*this, 60, 40, kRed);
    desktop->moveWindow(*desktop->windows()[0], {20, 20});
    auto b = solidWindow(*this, 60, 40, kBlue); // focused
    desktop->moveWindow(*desktop->windows()[1], {200, 150});
    pumpFrames(2);

    EXPECT_NEAR(channel(pixel(50, 40), 16), 128, 3) << "red at half strength under black";
    EXPECT_EQ(pixel(230, 170), 0x0000ffu) << "the focused window isn't dimmed";
    const std::uint32_t glow = pixel(198, 170); // 2px outside the focused window
    EXPECT_GT(channel(glow, 8), channel(background(), 8) + 60) << std::hex << glow;
    EXPECT_EQ(pixel(18, 40), background()) << "no glow for unfocused windows (transparent by default)";
}

TEST_F(DesktopRenderTest, BlurSoftensWhatsBehindATranslucentWindow)
{
    // Red and blue side by side, a fully transparent window over the seam.
    auto red = solidWindow(*this, 100, 100, kRed);
    desktop->moveWindow(*desktop->windows()[0], {60, 70});
    auto blue = solidWindow(*this, 100, 100, kBlue);
    desktop->moveWindow(*desktop->windows()[1], {160, 70});

    WindowStyle glass = WindowStyle::plain();
    glass.cornerRadius = 1; // styled
    glass.blur = {.enabled = true, .size = 4, .passes = 2, .noise = 0, .contrast = 1, .brightness = 1, .vibrancy = 0};
    WindowStyle plain = WindowStyle::plain();
    desktop->setStyleResolver([&](const Toplevel& t) { return t.appId() == "glass" ? glass : plain; });

    auto w = makeWindow();
    xdg_toplevel_set_app_id(w->toplevel, "glass");
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, makeBuffer(60, 60, 0x00000000, WL_SHM_FORMAT_ARGB8888), 0, 0); // transparent
    wl_surface_damage_buffer(w->surface, 0, 0, 60, 60);
    wl_surface_commit(w->surface);
    ASSERT_TRUE(roundtrip());
    desktop->moveWindow(*desktop->windows()[2], {130, 90});
    pumpFrames(2);

    const std::uint32_t nearSeam = pixel(156, 120); // 4px into the red side, under the glass
    EXPECT_GT(channel(nearSeam, 0), 20) << "blue bleeds across: " << std::hex << nearSeam;
    EXPECT_LT(channel(nearSeam, 16), 250) << std::hex << nearSeam;
    EXPECT_EQ(pixel(100, 120), 0xff0000u) << "outside the glass nothing changes";

    // xray: only what's below all windows is blurred, so the windows under it vanish.
    glass.blur.xray = true;
    desktop->restyle();
    pumpFrames(2);
    const std::uint32_t xray = pixel(156, 120), bg = background();
    for (int shift : {0, 8, 16})
        EXPECT_NEAR(channel(xray, shift), channel(bg, shift), 3) << std::hex << xray;
    glass.blur.xray = false;

    // Without blur the same window shows the seam sharp.
    glass.blur.enabled = false;
    desktop->restyle();
    pumpFrames(2);
    EXPECT_EQ(pixel(156, 120), 0xff0000u);
}

TEST_F(DesktopRenderTest, WallpaperFillsTheScreen)
{
    // 4x2: left half green, right half blue; "fill" on 320x240 crops the sides.
    const auto path = std::filesystem::temp_directory_path() / ("dragon-wallpaper-" + std::to_string(getpid()) + ".png");
    std::vector<std::uint8_t> rgb = {0, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255,
                                     0, 255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255};
    ASSERT_TRUE(png::writeRgb(path.string(), 4, 2, rgb));
    renderer->setWallpaper({.path = path.string(), .mode = "stretch"});
    pumpFrames(2);
    EXPECT_EQ(pixel(10, 120), 0x00ff00u);
    EXPECT_EQ(pixel(310, 120), 0x0000ffu);
    std::filesystem::remove(path);
}

