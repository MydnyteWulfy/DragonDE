#pragma once

// Desktop-level harness: the server fixture plus a real GPU, a headless
// output with CPU-readable buffers, textures and the output renderer, wired
// the way main() wires them.

#include "../server/ServerFixture.h"

#include "backend/headless/HeadlessBackend.h"
#include "desktop/Desktop.h"
#include "desktop/OutputRenderer.h"
#include "desktop/SurfaceTextures.h"
#include "protocols/LinuxDmabuf.h"
#include "render/Gpu.h"
#include "render/vulkan/VulkanTexture.h"

#include <chrono>
#include <cmath>
#include <drm_fourcc.h>
#include <gbm.h>

namespace dragon::test {

class DesktopFixture : public ServerFixture {
public:
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

        DmabufSupport dmabuf{gpu->device(), {}, [this](const DmaBuf& buf) {
                                 return gpu->renderer().importTexture(buf) != nullptr;
                             }};
        for (auto [format, modifier] : gpu->sampleFormats())
            dmabuf.formats.push_back({format, modifier});
        server->enableDmabuf(std::move(dmabuf));
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
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

} // namespace dragon::test
