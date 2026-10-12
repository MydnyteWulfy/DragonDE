#pragma once

#include "render/GbmAllocator.h"
#include "render/vulkan/VulkanImage.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace dragon {

class Gpu;

struct SwapchainSlot {
    std::size_t index = 0;
    std::unique_ptr<GbmBuffer> buffer;
    std::unique_ptr<vk::VulkanImage> image; // declared after buffer: destroyed first
    // Set while we render into it or the display holds it; the backend clears
    // it when the buffer comes back (wl_buffer.release, page flip, ...).
    bool busy = false;
};

// A small ring of GBM render targets for one output. This is our own
// swapchain, not a VkSwapchainKHR: the backend decides how to present.
class Swapchain {
public:
    static std::unique_ptr<Swapchain> create(Gpu& gpu, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t format, std::span<const std::uint64_t> modifiers,
                                             std::uint32_t gbmUsage, std::size_t count = 3);

    // A free slot, marked busy; nullptr if the display holds them all.
    SwapchainSlot* acquire();
    static void release(SwapchainSlot& slot) { slot.busy = false; }

    std::span<SwapchainSlot> slots() { return m_slots; }
    std::uint32_t width() const { return m_width; }
    std::uint32_t height() const { return m_height; }
    std::uint32_t format() const { return m_format; }

private:
    Swapchain() = default;

    std::vector<SwapchainSlot> m_slots; // never resized after create(): slot addresses are stable
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::uint32_t m_format = 0;
};

} // namespace dragon
