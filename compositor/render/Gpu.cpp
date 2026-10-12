#include "render/Gpu.h"

#include "core/Log.h"
#include "render/RenderNode.h"
#include "render/vulkan/Formats.h"

#include <drm_fourcc.h>

namespace dragon {

std::unique_ptr<Gpu> Gpu::create(std::optional<dev_t> preferred, bool validation)
{
    auto node = openRenderNode(preferred);
    if (!node)
        return nullptr;
    const dev_t renderDevice = node->device;

    std::unique_ptr<Gpu> gpu(new Gpu());
    gpu->m_device = renderDevice;
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

std::vector<std::pair<std::uint32_t, std::uint64_t>> Gpu::sampleFormats() const
{
    // What clients render to in practice; formatFromDrm() knows each of these.
    static constexpr std::uint32_t kFormats[] = {
        DRM_FORMAT_ARGB8888,    DRM_FORMAT_XRGB8888,    DRM_FORMAT_ABGR8888,    DRM_FORMAT_XBGR8888,
        DRM_FORMAT_ARGB2101010, DRM_FORMAT_XRGB2101010, DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR2101010,
    };
    std::vector<std::pair<std::uint32_t, std::uint64_t>> out;
    for (std::uint32_t format : kFormats) {
        for (std::uint64_t mod : m_vulkan->supportedModifiers(vk::formatFromDrm(format), VK_IMAGE_USAGE_SAMPLED_BIT))
            out.emplace_back(format, mod);
    }
    return out;
}

} // namespace dragon
