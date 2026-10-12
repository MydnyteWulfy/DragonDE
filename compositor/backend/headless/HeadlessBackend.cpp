#include "backend/headless/HeadlessBackend.h"

#include "core/EventLoop.h"
#include "core/Log.h"
#include "render/Gpu.h"

#include <chrono>
#include <drm_fourcc.h>
#include <format>
#include <gbm.h>

namespace dragon {

namespace {

class HeadlessOutput final : public Output {
public:
    HeadlessOutput(EventLoop& loop, std::string name, std::chrono::milliseconds period,
                   std::unique_ptr<Swapchain> swapchain)
        : Output(std::move(name)), m_period(period)
    {
        m_swapchain = std::move(swapchain);
        // Fixed-rate "vblank": fire, then re-arm, whether or not a frame was drawn.
        m_timer = loop.addTimer([this] {
            m_timer.arm(m_period);
            frame.emit();
        });
        m_timer.arm(std::chrono::milliseconds(1));
    }

    void present(SwapchainSlot& slot) override
    {
        // Nobody scans this out; the renderer's fences already order reuse.
        Swapchain::release(slot);
        ++m_framesPresented;
    }

private:
    std::chrono::milliseconds m_period;
    EventSource m_timer;
};

} // namespace

bool HeadlessBackend::start(Gpu& gpu)
{
    const auto modifiers = gpu.renderModifiers(DRM_FORMAT_XRGB8888);
    if (modifiers.empty()) {
        log::error("backend", "headless: GPU can't render XRGB8888");
        return false;
    }
    const auto period = std::chrono::milliseconds(1000 / std::max<std::uint32_t>(m_config.refreshHz, 1));

    for (int i = 0; i < m_config.outputs; ++i) {
        auto swapchain = Swapchain::create(gpu, m_config.width, m_config.height, DRM_FORMAT_XRGB8888, modifiers,
                                           GBM_BO_USE_RENDERING);
        if (!swapchain)
            return false;
        addOutput(std::make_unique<HeadlessOutput>(m_loop, std::format("HEADLESS-{}", i + 1), period,
                                                   std::move(swapchain)));
    }
    return true;
}

} // namespace dragon
