// End to end: a real client maps shm windows, the compositor composites them
// onto a headless output, and we read the presented frame back.

#include "../server/ServerFixture.h"

#include "backend/headless/HeadlessBackend.h"
#include "desktop/Desktop.h"
#include "desktop/OutputRenderer.h"
#include "desktop/SurfaceTextures.h"
#include "render/Gpu.h"

#include <chrono>
#include <cmath>
#include <drm_fourcc.h>
#include <gbm.h>

using namespace dragon;
using namespace std::chrono_literals;

namespace {

class DesktopRenderTest : public test::ServerFixture {
protected:
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<Desktop> desktop;
    std::unique_ptr<SurfaceTextures> textures;
    std::unique_ptr<HeadlessBackend> backend;
    std::unique_ptr<OutputRenderer> renderer;
    Output* output = nullptr;

    void SetUp() override
    {
        ServerFixture::SetUp();
        gpu = Gpu::create(std::nullopt, /*validation=*/true);
        if (!gpu) {
            GTEST_SKIP() << "no usable GPU";
        }
        desktop = std::make_unique<Desktop>(*server);
        textures = std::make_unique<SurfaceTextures>(*server, *gpu);
        backend = std::make_unique<HeadlessBackend>(
            *loop, HeadlessBackend::Config{.outputs = 1, .width = 320, .height = 240, .refreshHz = 120,
                                           .modifiers = {DRM_FORMAT_MOD_LINEAR}});
        ASSERT_TRUE(backend->start(*gpu));
        output = backend->outputs().front().get();
        server->addOutput(*output);
        renderer = std::make_unique<OutputRenderer>(*server, *gpu, *desktop, *textures, *output);
        desktop->setArea({0, 0, 320, 240});
    }

    void TearDown() override
    {
        if (gpu) {
            if (client)
                disconnect();
            gpu->renderer().waitIdle();
            renderer.reset();
            server->removeOutput(*output);
            backend.reset();
            textures.reset();
            desktop.reset();
            EXPECT_EQ(gpu->vulkan().validationErrors(), 0u);
        }
        ServerFixture::TearDown();
        gpu.reset();
    }

    // Lets the headless output present `n` more frames, servicing the client meanwhile.
    void pumpFrames(int n)
    {
        const auto target = output->framesPresented() + std::uint64_t(n);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (output->framesPresented() < target && std::chrono::steady_clock::now() < deadline)
            ASSERT_TRUE(roundtrip());
        ASSERT_GE(output->framesPresented(), target);
    }

    std::uint32_t pixel(std::int32_t x, std::int32_t y)
    {
        gpu->renderer().waitIdle();
        const SwapchainSlot* slot = output->lastPresented();
        EXPECT_NE(slot, nullptr);
        if (!slot)
            return 0;
        std::uint32_t stride = 0;
        void* mapData = nullptr;
        auto* base = static_cast<const std::uint8_t*>(gbm_bo_map(slot->buffer->bo(), 0, 0, 320, 240,
                                                                 GBM_BO_TRANSFER_READ, &stride, &mapData));
        const auto px = *reinterpret_cast<const std::uint32_t*>(base + std::size_t(y) * stride + std::size_t(x) * 4);
        gbm_bo_unmap(slot->buffer->bo(), mapData);
        return px & 0xffffff;
    }

    static std::uint32_t background()
    {
        const auto& c = OutputRenderer::kBackground;
        auto ch = [](float v) { return std::uint32_t(std::lround(v * 255.0f)); };
        return (ch(c[0]) << 16) | (ch(c[1]) << 8) | ch(c[2]);
    }
};

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
