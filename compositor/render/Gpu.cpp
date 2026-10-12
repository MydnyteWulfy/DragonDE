#include "render/Gpu.h"

#include "core/Log.h"
#include "render/RenderNode.h"
#include "render/vulkan/Formats.h"

namespace dragon {

std::unique_ptr<Gpu> Gpu::create(std::optional<dev_t> preferred, bool validation)
{
    auto node = openRenderNode(preferred);
    if (!node)
        return nullptr;
    const dev_t renderDevice = node->device;

    std::unique_ptr<Gpu> gpu(new Gpu());
    gpu->m_allocator = GbmAllocator::create(std::move(node->fd));
    if (!gpu->m_allocator)
        return nullptr;
    gpu->m_vulkan = vk::VulkanDevice::create(renderDevice, validation);
    if (!gpu->m_vulkan)
        return nullptr;
    gpu->m_renderer = vk::VulkanRenderer::create(*gpu->m_vulkan);
    if (!gpu->m_renderer)
        return nullptr;
    return gpu;
}

Gpu::~Gpu()
{
    if (m_renderer)
        m_renderer->waitIdle();
}

std::vector<std::uint64_t> Gpu::renderModifiers(std::uint32_t drmFormat) const
{
    const VkFormat format = vk::formatFromDrm(drmFormat);
    if (format == VK_FORMAT_UNDEFINED)
        return {};
    return m_vulkan->supportedModifiers(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}

} // namespace dragon
