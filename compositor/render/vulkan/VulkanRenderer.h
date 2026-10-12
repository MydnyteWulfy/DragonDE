#pragma once

#include "core/Geometry.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

#include "core/UniqueFd.h"

namespace dragon {
struct DmaBuf;
}

namespace dragon::vk {

class VulkanDevice;
class VulkanImage;
class VulkanTexture;

struct TextureDraw {
    VulkanTexture* texture;
    Rect dst; // target pixels
    float alpha = 1.0f;
};

// Records and submits frames.
//
// Every frame follows the same protocol, so targets can be shared with KMS or
// another compositor without either side waiting on the CPU:
//   1. wait for the buffer's implicit fences (previous readers) on the GPU
//   2. acquire the image from VK_QUEUE_FAMILY_FOREIGN_EXT
//   3. upload changed textures, clear, draw
//   4. release it back to FOREIGN in GENERAL layout
//   5. attach our completion fence to the dma-buf for the next consumer
class VulkanRenderer {
public:
    static std::unique_ptr<VulkanRenderer> create(VulkanDevice& device);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    VulkanDevice& device() const { return m_device; }

    std::unique_ptr<VulkanTexture> createTexture(std::uint32_t width, std::uint32_t height, VkFormat format,
                                                 bool opaque);
    // Zero-copy texture over a client's dma-buf. Null if Vulkan can't import it.
    std::unique_ptr<VulkanTexture> importTexture(const DmaBuf& buffer);

    // Clears `target`, then draws `draws` in order (premultiplied alpha blending).
    bool render(VulkanImage& target, std::array<float, 4> clearColor, std::span<const TextureDraw> draws);
    bool clear(VulkanImage& target, std::array<float, 4> rgba) { return render(target, rgba, {}); }

    // Blocks until the last submitted frame has finished on the GPU.
    bool waitIdle(std::chrono::nanoseconds timeout = std::chrono::seconds(1));

    // Frames are numbered from 1. completedFrame() <= submittedFrame().
    std::uint64_t submittedFrame() const { return m_submitted; }
    std::uint64_t completedFrame() const;

private:
    explicit VulkanRenderer(VulkanDevice& device) : m_device(device) {}
    bool init();
    bool createPipelineLayout();
    VkPipeline pipelineFor(VkFormat format);
    bool importSyncFile(VkSemaphore semaphore, UniqueFd syncFile);
    bool submit(VulkanImage& target, std::span<const VkSemaphoreSubmitInfo> waits,
                std::span<VulkanTexture* const> imported);

    VulkanDevice& m_device;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;               // CPU knows when the frame is done
    VkSemaphore m_waitSemaphore = VK_NULL_HANDLE;   // imported from the buffer's sync_file
    VkSemaphore m_signalSemaphore = VK_NULL_HANDLE; // exported as a sync_file into the buffer
    std::vector<VkSemaphore> m_textureWaits;        // one per imported texture in a frame
    std::uint64_t m_submitted = 0;

    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule m_vertShader = VK_NULL_HANDLE;
    VkShaderModule m_fragShader = VK_NULL_HANDLE;
    std::map<VkFormat, VkPipeline> m_pipelines; // one per render target format
};

} // namespace dragon::vk
