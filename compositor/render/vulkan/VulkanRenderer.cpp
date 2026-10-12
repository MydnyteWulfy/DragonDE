#include "render/vulkan/VulkanRenderer.h"

#include "core/Log.h"
#include "render/DmaBuf.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanImage.h"
#include "render/vulkan/VulkanTexture.h"

#include <bit>

namespace dragon::vk {

namespace {
constexpr VkImageSubresourceRange kColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

constexpr std::uint32_t kQuadVert[] = {
#include "shaders/quad.vert.spv.inc"
};
constexpr std::uint32_t kTextureFrag[] = {
#include "shaders/texture.frag.spv.inc"
};

// Must match `Push` in the shaders.
struct QuadPush {
    float rect[4]; // NDC x, y, width, height
    float uv[4];
    float alpha;
    std::uint32_t opaque;
};
static_assert(sizeof(QuadPush) == 40);

VkShaderModule createShader(VkDevice dev, std::span<const std::uint32_t> code)
{
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(dev, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}
} // namespace

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
    for (auto& [format, pipeline] : m_pipelines)
        vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, m_fragShader, nullptr);
    vkDestroyShaderModule(dev, m_vertShader, nullptr);
    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    vkDestroySampler(dev, m_sampler, nullptr);
    vkDestroySemaphore(dev, m_signalSemaphore, nullptr);
    vkDestroySemaphore(dev, m_waitSemaphore, nullptr);
    vkDestroyFence(dev, m_fence, nullptr);
    vkDestroyCommandPool(dev, m_pool, nullptr); // frees m_cmd
}

std::uint64_t VulkanRenderer::completedFrame() const
{
    const bool idle = vkGetFenceStatus(m_device.device(), m_fence) == VK_SUCCESS;
    return idle ? m_submitted : m_submitted - 1;
}

std::unique_ptr<VulkanTexture> VulkanRenderer::createTexture(std::uint32_t width, std::uint32_t height,
                                                             VkFormat format, bool opaque)
{
    std::unique_ptr<VulkanTexture> tex(new VulkanTexture(*this));
    if (!tex->init(width, height, format, opaque))
        return nullptr;
    return tex;
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
    if (!check(vkCreateSemaphore(dev, &signalInfo, nullptr, &m_signalSemaphore), "vkCreateSemaphore(signal)"))
        return false;
    return createPipelineLayout();
}

bool VulkanRenderer::createPipelineLayout()
{
    VkDevice dev = m_device.device();

    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    if (!check(vkCreateSampler(dev, &samplerInfo, nullptr, &m_sampler), "vkCreateSampler"))
        return false;

    // One texture, pushed per draw (VK_KHR_push_descriptor): no pools, no sets to manage.
    VkDescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &m_sampler,
    };
    VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (!check(vkCreateDescriptorSetLayout(dev, &setInfo, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout"))
        return false;

    VkPushConstantRange push{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(QuadPush),
    };
    VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &m_setLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (!check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout"))
        return false;

    m_vertShader = createShader(dev, kQuadVert);
    m_fragShader = createShader(dev, kTextureFrag);
    return m_vertShader && m_fragShader;
}

VkPipeline VulkanRenderer::pipelineFor(VkFormat format)
{
    if (auto it = m_pipelines.find(format); it != m_pipelines.end())
        return it->second;

    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = m_vertShader, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = m_fragShader, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    // Premultiplied alpha: out = src + dst * (1 - src.a)
    VkPipelineColorBlendAttachmentState blendAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamicStates,
    };
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = m_pipelineLayout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!check(vkCreateGraphicsPipelines(m_device.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
               "vkCreateGraphicsPipelines"))
        return VK_NULL_HANDLE;
    m_pipelines.emplace(format, pipeline);
    return pipeline;
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

bool VulkanRenderer::render(VulkanImage& target, std::array<float, 4> rgba, std::span<const TextureDraw> draws)
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

    const std::uint64_t frame = m_submitted + 1;
    for (const auto& d : draws)
        d.texture->recordUpload(m_cmd, frame);

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

    if (!draws.empty()) {
        const VkExtent2D extent = target.extent();
        if (VkPipeline pipeline = pipelineFor(target.format())) {
            vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const VkViewport vp{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(m_cmd, 0, 1, &vp);
            vkCmdSetScissor(m_cmd, 0, 1, &scissor);

            const float sx = 2.0f / float(extent.width), sy = 2.0f / float(extent.height);
            for (const auto& d : draws) {
                if (!d.texture->ready())
                    continue;
                VkDescriptorImageInfo image{
                    .imageView = d.texture->m_view,
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                };
                VkWriteDescriptorSet write{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = 0,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .pImageInfo = &image,
                };
                m_device.fn().cmdPushDescriptorSetKHR(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
                                                      1, &write);
                const QuadPush push{
                    .rect = {float(d.dst.x) * sx - 1.0f, float(d.dst.y) * sy - 1.0f, float(d.dst.width) * sx,
                             float(d.dst.height) * sy},
                    .uv = {0.0f, 0.0f, 1.0f, 1.0f},
                    .alpha = d.alpha,
                    .opaque = d.texture->opaque() ? 1u : 0u,
                };
                vkCmdPushConstants(m_cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                vkCmdDraw(m_cmd, 4, 1, 0, 0);
                d.texture->m_lastUsedFrame = frame;
            }
        }
    }

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
    ++m_submitted;

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
