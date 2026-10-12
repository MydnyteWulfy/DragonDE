#pragma once

#include "core/UniqueFd.h"
#include "render/DmaBuf.h"

#include <memory>
#include <vulkan/vulkan.h>

namespace dragon::vk {

class VulkanDevice;

// A dma-buf imported into Vulkan (zero-copy). Used for our render targets and,
// later, for client buffers from linux-dmabuf-v1.
class VulkanImage {
public:
    // Single-plane buffers only for now. `usage` e.g. COLOR_ATTACHMENT or SAMPLED.
    static std::unique_ptr<VulkanImage> importDmaBuf(VulkanDevice& device, const DmaBuf& buf,
                                                     VkImageUsageFlags usage, const char* debugName = nullptr);
    ~VulkanImage();
    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    VkImage image() const { return m_image; }
    VkImageView view() const { return m_view; }
    VkFormat format() const { return m_format; }
    VkExtent2D extent() const { return m_extent; }

    // Our own reference to the buffer, used to exchange implicit-sync fences.
    int dmabufFd() const { return m_syncFd.get(); }

private:
    explicit VulkanImage(VulkanDevice& device) : m_device(device) {}

    VulkanDevice& m_device;
    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D m_extent{};
    UniqueFd m_syncFd;
};

} // namespace dragon::vk
