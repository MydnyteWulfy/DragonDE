#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <vulkan/vulkan.h>

namespace dragon::vk {

class VulkanDevice;
class VulkanImage;

// Records and submits frames. For now it can only clear a target; the scene
// renderer grows from here.
//
// Every frame follows the same protocol, so targets can be shared with KMS or
// another compositor without either side waiting on the CPU:
//   1. wait for the buffer's implicit fences (previous readers) on the GPU
//   2. acquire the image from VK_QUEUE_FAMILY_FOREIGN_EXT
//   3. render
//   4. release it back to FOREIGN in GENERAL layout
//   5. attach our completion fence to the dma-buf for the next consumer
class VulkanRenderer {
public:
    static std::unique_ptr<VulkanRenderer> create(VulkanDevice& device);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool clear(VulkanImage& target, std::array<float, 4> rgba);

    // Blocks until the last submitted frame has finished on the GPU.
    bool waitIdle(std::chrono::nanoseconds timeout = std::chrono::seconds(1));

private:
    explicit VulkanRenderer(VulkanDevice& device) : m_device(device) {}
    bool init();
    bool importWaitSemaphore(VulkanImage& target);
    bool submit(VulkanImage& target, bool waitOnBuffer);

    VulkanDevice& m_device;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;           // CPU knows when the frame is done
    VkSemaphore m_waitSemaphore = VK_NULL_HANDLE;   // imported from the buffer's sync_file
    VkSemaphore m_signalSemaphore = VK_NULL_HANDLE; // exported as a sync_file into the buffer
};

} // namespace dragon::vk
