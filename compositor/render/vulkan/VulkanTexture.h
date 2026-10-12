#pragma once

#include "core/Geometry.h"
#include "render/vulkan/VulkanImage.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace dragon::vk {

class VulkanRenderer;

// A texture the renderer can draw. Two kinds:
//  - owned (wl_shm): write() copies changed pixels into a staging buffer right
//    away, so the client can reuse its buffer; the GPU copy into the image is
//    recorded with the next frame.
//  - imported (client dma-buf): zero-copy; each frame waits for the client's
//    implicit write fence and takes the image from VK_QUEUE_FAMILY_FOREIGN_EXT.
class VulkanTexture {
public:
    ~VulkanTexture();
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    std::uint32_t width() const { return m_width; }
    std::uint32_t height() const { return m_height; }
    VkFormat format() const { return m_format; }
    // Ignore the alpha channel when sampling (DRM X* formats).
    bool opaque() const { return m_opaque; }

    // Copies `rects` (texture coordinates, clipped to the texture) from `data`,
    // whose rows are `stride` bytes apart. 4 bytes per pixel.
    void write(const void* data, std::uint32_t stride, std::span<const Rect> rects);

    // True once the image holds content and can be sampled.
    bool ready() const
    {
        return imported() || m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || !m_pending.empty();
    }

    bool imported() const { return m_imported != nullptr; }
    // Imported only: the dma-buf whose implicit fences we wait on / signal.
    int dmabufFd() const;

private:
    friend class VulkanRenderer;

    explicit VulkanTexture(VulkanRenderer& renderer) : m_renderer(renderer) {}
    bool init(std::uint32_t width, std::uint32_t height, VkFormat format, bool opaque);
    // Records pending staging->image copies and leaves the image sampleable.
    void recordUpload(VkCommandBuffer cmd, std::uint64_t frame);

    VulkanRenderer& m_renderer;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    bool m_opaque = false;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkBuffer m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
    std::uint8_t* m_stagingData = nullptr;
    std::vector<VkBufferImageCopy> m_pending;

    std::uint64_t m_lastUsedFrame = 0; // last frame that read the image or the staging buffer
    std::unique_ptr<VulkanImage> m_imported; // owns image/view/memory when imported
};

} // namespace dragon::vk
