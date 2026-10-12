#include "render/vulkan/VulkanRenderer.h"

#include "core/Log.h"
#include "render/DmaBuf.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanImage.h"

namespace dragon::vk {

namespace {
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

std::unique_ptr<VulkanRenderer> VulkanRenderer::create(VulkanDevice& device)
{
    std::unique_ptr<VulkanRenderer> r(new VulkanRenderer(device));
    if (!r->init())
        return nullptr;
    return r;
}

VulkanRenderer::~VulkanRenderer()
{
    VkDevice dev = m_device.device();
    if (m_fence)
        vkWaitForFences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
    vkDestroySemaphore(dev, m_signalSemaphore, nullptr);
    vkDestroySemaphore(dev, m_waitSemaphore, nullptr);
    vkDestroyFence(dev, m_fence, nullptr);
    vkDestroyCommandPool(dev, m_pool, nullptr); // frees m_cmd
}

bool VulkanRenderer::init()
{
    VkDevice dev = m_device.device();

    VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = m_device.queueFamily(),
    };
    if (!check(vkCreateCommandPool(dev, &poolInfo, nullptr, &m_pool), "vkCreateCommandPool"))
        return false;

    VkCommandBufferAllocateInfo cmdInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = m_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (!check(vkAllocateCommandBuffers(dev, &cmdInfo, &m_cmd), "vkAllocateCommandBuffers"))
        return false;

    VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (!check(vkCreateFence(dev, &fenceInfo, nullptr, &m_fence), "vkCreateFence"))
        return false;

    VkSemaphoreCreateInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!check(vkCreateSemaphore(dev, &waitInfo, nullptr, &m_waitSemaphore), "vkCreateSemaphore(wait)"))
        return false;

    VkExportSemaphoreCreateInfo exportInfo{
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkSemaphoreCreateInfo signalInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &exportInfo};
    return check(vkCreateSemaphore(dev, &signalInfo, nullptr, &m_signalSemaphore), "vkCreateSemaphore(signal)");
}

bool VulkanRenderer::waitIdle(std::chrono::nanoseconds timeout)
{
    const VkResult r = vkWaitForFences(m_device.device(), 1, &m_fence, VK_TRUE, std::uint64_t(timeout.count()));
    if (r == VK_TIMEOUT) {
        log::error("vulkan", "GPU frame did not finish within {}", timeout);
        return false;
    }
    return check(r, "vkWaitForFences");
}

bool VulkanRenderer::importWaitSemaphore(VulkanImage& target)
{
    // Writing, so wait for everyone (readers and writers) still using the buffer.
    UniqueFd syncFile = dmabuf::exportSyncFile(target.dmabufFd(), dmabuf::Access::Write);
    if (!syncFile)
        return false;

    VkImportSemaphoreFdInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = m_waitSemaphore,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, // required for SYNC_FD; payload consumed by one wait
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = syncFile.get(),
    };
    if (!check(m_device.fn().importSemaphoreFdKHR(m_device.device(), &info), "vkImportSemaphoreFdKHR"))
        return false;
    syncFile.release(); // owned by the semaphore now
    return true;
}

bool VulkanRenderer::clear(VulkanImage& target, std::array<float, 4> rgba)
{
    // One frame in flight for now; the frame pacing work will add more.
    if (!waitIdle())
        return false;

    const bool waitOnBuffer = importWaitSemaphore(target);

    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &begin);

    // Acquire from FOREIGN. UNDEFINED old layout: we overwrite every pixel.
    VkImageMemoryBarrier2 acquire{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, // chains with the semaphore wait
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = m_device.queueFamily(),
        .image = target.image(),
        .subresourceRange = kColorRange,
    };
    VkDependencyInfo acquireDep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &acquire,
    };
    vkCmdPipelineBarrier2(m_cmd, &acquireDep);

    VkRenderingAttachmentInfo color{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {rgba[0], rgba[1], rgba[2], rgba[3]}}},
    };
    VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, target.extent()},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(m_cmd, &rendering);
    vkCmdEndRendering(m_cmd);

    // Release to FOREIGN in GENERAL, the layout external consumers expect.
    VkImageMemoryBarrier2 release{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = VK_ACCESS_2_NONE,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = m_device.queueFamily(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = target.image(),
        .subresourceRange = kColorRange,
    };
    VkDependencyInfo releaseDep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &release,
    };
    vkCmdPipelineBarrier2(m_cmd, &releaseDep);

    if (!check(vkEndCommandBuffer(m_cmd), "vkEndCommandBuffer"))
        return false;
    return submit(target, waitOnBuffer);
}

bool VulkanRenderer::submit(VulkanImage& target, bool waitOnBuffer)
{
    VkSemaphoreSubmitInfo wait{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_waitSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_signalSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cmd{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = m_cmd};
    VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = waitOnBuffer ? 1u : 0u,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cmd,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    // Reset only now: if anything above failed, the fence stays signaled and
    // the next frame doesn't hang waiting for work that was never submitted.
    vkResetFences(m_device.device(), 1, &m_fence);
    if (!check(vkQueueSubmit2(m_device.queue(), 1, &submit, m_fence), "vkQueueSubmit2"))
        return false;

    // Hand our completion to implicit sync: the next reader of the dma-buf
    // (KMS, the parent compositor) waits for it in the kernel.
    VkSemaphoreGetFdInfoKHR getFd{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = m_signalSemaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int fd = -1;
    if (!check(m_device.fn().getSemaphoreFdKHR(m_device.device(), &getFd, &fd), "vkGetSemaphoreFdKHR"))
        return false;
    UniqueFd syncFile(fd); // -1 means "already signaled", nothing to attach
    if (syncFile)
        dmabuf::importSyncFile(target.dmabufFd(), syncFile.get(), dmabuf::Access::Write);
    return true;
}

} // namespace dragon::vk
