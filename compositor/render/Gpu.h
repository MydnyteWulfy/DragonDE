#pragma once

#include "render/GbmAllocator.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanRenderer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <sys/types.h>
#include <vector>

namespace dragon {

// Everything needed to allocate and render on one GPU: the render node, GBM
// on top of it, and a Vulkan device for the same hardware.
class Gpu {
public:
    // `preferred`: any DRM node (primary or render) of the GPU to use, e.g. the
    // parent compositor's main device. Otherwise the first GPU found.
    static std::unique_ptr<Gpu> create(std::optional<dev_t> preferred, bool validation);
    ~Gpu();

    GbmAllocator& allocator() { return *m_allocator; }
    vk::VulkanDevice& vulkan() { return *m_vulkan; }
    vk::VulkanRenderer& renderer() { return *m_renderer; }

    // Modifiers we can render into for a DRM format (empty if unsupported).
    std::vector<std::uint64_t> renderModifiers(std::uint32_t drmFormat) const;

private:
    Gpu() = default;

    // Destroyed bottom-up: renderer, then device, then allocator.
    std::unique_ptr<GbmAllocator> m_allocator;
    std::unique_ptr<vk::VulkanDevice> m_vulkan;
    std::unique_ptr<vk::VulkanRenderer> m_renderer;
};

} // namespace dragon
