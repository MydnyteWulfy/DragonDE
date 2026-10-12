#include "render/vulkan/VulkanTexture.h"

#include "core/Log.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanImage.h"
#include "render/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cstring>

namespace dragon::vk {

namespace {
constexpr std::uint32_t kBytesPerPixel = 4;
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
} // namespace

bool VulkanTexture::init(std::uint32_t width, std::uint32_t height, VkFormat format, bool opaque)
{
    m_width = width;
    m_height = height;
    m_format = format;
    m_opaque = opaque;
    VulkanDevice& device = m_renderer.device();
    VkDevice dev = device.device();

    VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!check(vkCreateImage(dev, &imageInfo, nullptr, &m_image), "vkCreateImage(texture)"))
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, m_image, &req);
    auto type = device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type)
        return false;
    VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = *type,
    };
    if (!check(vkAllocateMemory(dev, &alloc, nullptr, &m_memory), "vkAllocateMemory(texture)") ||
        !check(vkBindImageMemory(dev, m_image, m_memory, 0), "vkBindImageMemory(texture)"))
        return false;

    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = m_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = kColorRange,
    };
    if (!check(vkCreateImageView(dev, &viewInfo, nullptr, &m_view), "vkCreateImageView(texture)"))
        return false;

    // Staging buffer with the texture's exact layout (tightly packed rows).
    VkBufferCreateInfo bufInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(width) * height * kBytesPerPixel,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (!check(vkCreateBuffer(dev, &bufInfo, nullptr, &m_staging), "vkCreateBuffer(staging)"))
        return false;
    vkGetBufferMemoryRequirements(dev, m_staging, &req);
    type = device.findMemoryType(req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type)
        return false;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = *type;
    if (!check(vkAllocateMemory(dev, &alloc, nullptr, &m_stagingMemory), "vkAllocateMemory(staging)") ||
        !check(vkBindBufferMemory(dev, m_staging, m_stagingMemory, 0), "vkBindBufferMemory(staging)"))
        return false;
    void* mapped = nullptr;
    if (!check(vkMapMemory(dev, m_stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(staging)"))
        return false;
    m_stagingData = static_cast<std::uint8_t*>(mapped);
    return true;
}

VulkanTexture::~VulkanTexture()
{
    if (m_lastUsedFrame > m_renderer.completedFrame())
        m_renderer.waitIdle();
    if (m_imported)
        return; // VulkanImage cleans up
    VkDevice dev = m_renderer.device().device();
    vkDestroyImageView(dev, m_view, nullptr);
    vkDestroyImage(dev, m_image, nullptr);
    vkFreeMemory(dev, m_memory, nullptr);
    vkDestroyBuffer(dev, m_staging, nullptr); // memory unmaps when freed
    vkFreeMemory(dev, m_stagingMemory, nullptr);
}

int VulkanTexture::dmabufFd() const
{
    return m_imported ? m_imported->dmabufFd() : -1;
}

void VulkanTexture::write(const void* data, std::uint32_t stride, std::span<const Rect> rects)
{
    if (m_imported)
        return;
    // An in-flight frame may still be copying out of the staging buffer.
    if (m_lastUsedFrame > m_renderer.completedFrame())
        m_renderer.waitIdle();

    const auto* src = static_cast<const std::uint8_t*>(data);
    const Rect bounds{0, 0, std::int32_t(m_width), std::int32_t(m_height)};
    for (const Rect& r : rects) {
        const std::int32_t x0 = std::max(r.x, bounds.x), y0 = std::max(r.y, bounds.y);
        const std::int32_t x1 = std::min(r.x + r.width, bounds.width), y1 = std::min(r.y + r.height, bounds.height);
        if (x1 <= x0 || y1 <= y0)
            continue;

        const auto rowBytes = std::size_t(x1 - x0) * kBytesPerPixel;
        for (std::int32_t y = y0; y < y1; ++y) {
            std::memcpy(m_stagingData + (std::size_t(y) * m_width + std::size_t(x0)) * kBytesPerPixel,
                        src + std::size_t(y) * stride + std::size_t(x0) * kBytesPerPixel, rowBytes);
        }
        m_pending.push_back({
            .bufferOffset = (VkDeviceSize(y0) * m_width + VkDeviceSize(x0)) * kBytesPerPixel,
            .bufferRowLength = m_width,
            .bufferImageHeight = m_height,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {x0, y0, 0},
            .imageExtent = {std::uint32_t(x1 - x0), std::uint32_t(y1 - y0), 1},
        });
    }
}

void VulkanTexture::recordUpload(VkCommandBuffer cmd, std::uint64_t frame)
{
    if (m_pending.empty())
        return;

    VkImageMemoryBarrier2 toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, // previous frame's sampling
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = m_layout, // UNDEFINED the first time; otherwise keeps the old content
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_image,
        .subresourceRange = kColorRange,
    };
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &toTransfer};
    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdCopyBufferToImage(cmd, m_staging, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           std::uint32_t(m_pending.size()), m_pending.data());

    VkImageMemoryBarrier2 toSample{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = m_image,
        .subresourceRange = kColorRange,
    };
    dep.pImageMemoryBarriers = &toSample;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_pending.clear();
    m_lastUsedFrame = frame;
}

} // namespace dragon::vk
