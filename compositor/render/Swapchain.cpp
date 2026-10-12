#include "render/Swapchain.h"

#include "core/Log.h"
#include "render/Gpu.h"

#include <format>

namespace dragon {

std::unique_ptr<Swapchain> Swapchain::create(Gpu& gpu, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t format, std::span<const std::uint64_t> modifiers,
                                             std::uint32_t gbmUsage, std::size_t count)
{
    std::unique_ptr<Swapchain> sc(new Swapchain());
    sc->m_width = width;
    sc->m_height = height;
    sc->m_format = format;
    sc->m_slots.resize(count);

    for (std::size_t i = 0; i < count; ++i) {
        auto& slot = sc->m_slots[i];
        slot.index = i;
        slot.buffer = gpu.allocator().allocate(width, height, format, modifiers, gbmUsage);
        if (!slot.buffer)
            return nullptr;
        const auto name = std::format("swapchain {}x{} #{}", width, height, i);
        slot.image = vk::VulkanImage::importDmaBuf(gpu.vulkan(), slot.buffer->dmabuf(),
                                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, name.c_str());
        if (!slot.image)
            return nullptr;
    }

    log::debug("render", "swapchain {}x{} format {:#010x} modifier {:#018x} x{}", width, height, format,
               sc->m_slots[0].buffer->dmabuf().modifier, count);
    return sc;
}

SwapchainSlot* Swapchain::acquire()
{
    for (auto& slot : m_slots) {
        if (!slot.busy) {
            slot.busy = true;
            return &slot;
        }
    }
    return nullptr;
}

} // namespace dragon
