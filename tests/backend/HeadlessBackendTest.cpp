// Runs the real frame loop on the headless backend: timer-driven frames,
// swapchain acquire/release, Vulkan clears, with validation on.

#include "backend/headless/HeadlessBackend.h"
#include "core/EventLoop.h"
#include "core/Log.h"
#include "render/Gpu.h"

#include <chrono>
#include <gtest/gtest.h>
#include <wayland-server-core.h>

using namespace dragon;
using namespace std::chrono_literals;

TEST(HeadlessBackend, PresentsFramesOnEveryOutput)
{
    log::setLevel(LogLevel::Warn);
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> raw(wl_event_loop_create(),
                                                                         wl_event_loop_destroy);
    EventLoop loop(raw.get());

    auto gpu = Gpu::create(std::nullopt, /*validation=*/true);
    if (!gpu) {
        GTEST_SKIP() << "no usable GPU";
    }

    // Scope the backend so its outputs (and their swapchains) go before the GPU.
    {
        HeadlessBackend backend(loop, {.outputs = 2, .width = 320, .height = 240, .refreshHz = 120});
        std::vector<Connection> connections;
        auto added = backend.outputAdded.connect([&](Output* output) {
            connections.push_back(output->frame.connect([&gpu, output] {
                auto* slot = output->swapchain()->acquire();
                ASSERT_NE(slot, nullptr);
                ASSERT_TRUE(gpu->renderer().clear(*slot->image, {0.2f, 0.4f, 0.6f, 1.0f}));
                output->present(*slot);
            }));
        });
        ASSERT_TRUE(backend.start(*gpu));
        ASSERT_EQ(backend.outputs().size(), 2u);

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        auto enoughFrames = [&] {
            for (const auto& o : backend.outputs()) {
                if (o->framesPresented() < 10)
                    return false;
            }
            return true;
        };
        while (!enoughFrames() && std::chrono::steady_clock::now() < deadline)
            loop.dispatch(50);

        for (const auto& o : backend.outputs())
            EXPECT_GE(o->framesPresented(), 10u) << o->name();
        gpu->renderer().waitIdle();
    }
    EXPECT_EQ(gpu->vulkan().validationErrors(), 0u);
}
