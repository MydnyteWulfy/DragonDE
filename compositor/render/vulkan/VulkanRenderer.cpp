#include "render/vulkan/VulkanRenderer.h"

#include "core/Log.h"
#include "render/DmaBuf.h"
#include "render/vulkan/Formats.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanImage.h"
#include "render/vulkan/VulkanTexture.h"

#include <algorithm>
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
    float clipRect[4];
    float shapeRect[4];
    float color1[4];
    float color2[4];
    float clipRadius;
    float alpha;
    float shapeRadius;
    float shapeSize;
    float angle;
    std::uint32_t mode;
    std::uint32_t opaque;
    float pad;
};
// The minimum every Vulkan implementation supports.
static_assert(sizeof(QuadPush) == 128);

// `mode` in the shader: the kind of draw, plus flags.
constexpr std::uint32_t kModeTexture = 0, kModeShadow = 1, kModeBorder = 2, kModeFill = 3, kModeBlurDown = 4,
                        kModeBlurUp = 5, kModeBlurFinish = 6;
constexpr std::uint32_t kInvertClip = 8, kHasMask = 16;

void copy4(float (&dst)[4], const Rect& r)
{
    dst[0] = float(r.x), dst[1] = float(r.y), dst[2] = float(r.width), dst[3] = float(r.height);
}

void copy4(float (&dst)[4], const std::array<float, 4>& v)
{
    std::copy(v.begin(), v.end(), dst);
}

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

VulkanRenderer::VulkanRenderer(VulkanDevice& device) : m_device(device) {}

VulkanRenderer::~VulkanRenderer()
{
    VkDevice dev = m_device.device();
    if (m_fence)
        vkWaitForFences(dev, 1, &m_fence, VK_TRUE, UINT64_MAX);
    m_blank.reset();
    destroyScratch();
    for (auto& [format, pipeline] : m_pipelines)
        vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyShaderModule(dev, m_fragShader, nullptr);
    vkDestroyShaderModule(dev, m_vertShader, nullptr);
    vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    vkDestroySampler(dev, m_sampler, nullptr);
    for (VkSemaphore sem : m_textureWaits)
        vkDestroySemaphore(dev, sem, nullptr);
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
    // Binding 1: a mask (blur only where a layer surface is opaque enough).
    VkDescriptorSetLayoutBinding bindings[2];
    for (std::uint32_t i = 0; i < 2; ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = &m_sampler,
        };
    }
    VkDescriptorSetLayoutCreateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = 2,
        .pBindings = bindings,
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
    if (!m_vertShader || !m_fragShader)
        return false;

    m_blank = createTexture(1, 1, VK_FORMAT_B8G8R8A8_UNORM, false);
    if (!m_blank)
        return false;
    const std::uint32_t transparent = 0;
    const Rect all{0, 0, 1, 1};
    m_blank->write(&transparent, 4, {&all, 1});
    return true;
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

bool VulkanRenderer::importSyncFile(VkSemaphore semaphore, UniqueFd syncFile)
{
    if (!syncFile)
        return false;
    VkImportSemaphoreFdInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphore,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, // required for SYNC_FD; payload consumed by one wait
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = syncFile.get(),
    };
    if (!check(m_device.fn().importSemaphoreFdKHR(m_device.device(), &info), "vkImportSemaphoreFdKHR"))
        return false;
    syncFile.release(); // owned by the semaphore now
    return true;
}

std::unique_ptr<VulkanTexture> VulkanRenderer::importTexture(const DmaBuf& buffer)
{
    auto image = VulkanImage::importDmaBuf(m_device, buffer, VK_IMAGE_USAGE_SAMPLED_BIT, "client dma-buf");
    if (!image)
        return nullptr;
    std::unique_ptr<VulkanTexture> tex(new VulkanTexture(*this));
    tex->m_width = buffer.width;
    tex->m_height = buffer.height;
    tex->m_format = image->format();
    tex->m_opaque = !drmFormatHasAlpha(buffer.format);
    tex->m_image = image->image();
    tex->m_view = image->view();
    tex->m_imported = std::move(image);
    return tex;
}

// ----------------------------------------------------------------- scratch --

struct VulkanRenderer::Scratch {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDevice device = VK_NULL_HANDLE;

    ~Scratch()
    {
        vkDestroyImageView(device, view, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, memory, nullptr);
    }
};

bool VulkanRenderer::ensureScratch(std::unique_ptr<Scratch>& out, VkExtent2D extent, VkFormat format, const char* name)
{
    if (out && out->extent.width == extent.width && out->extent.height == extent.height && out->format == format)
        return true;
    waitIdle(); // the old one may still be in use
    out.reset();
    VkDevice dev = m_device.device();
    auto img = std::make_unique<Scratch>();
    img->device = dev;
    img->extent = extent;
    img->format = format;
    VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!check(vkCreateImage(dev, &info, nullptr, &img->image), name))
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img->image, &req);
    auto type = m_device.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!type)
        return false;
    VkMemoryAllocateInfo alloc{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = *type};
    if (!check(vkAllocateMemory(dev, &alloc, nullptr, &img->memory), name) ||
        !check(vkBindImageMemory(dev, img->image, img->memory, 0), name))
        return false;
    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = kColorRange,
    };
    if (!check(vkCreateImageView(dev, &viewInfo, nullptr, &img->view), name))
        return false;
    out = std::move(img);
    return true;
}

void VulkanRenderer::transition(Scratch& img, VkImageLayout layout)
{
    if (img.layout == layout)
        return;
    // Offscreen work is small and strictly ordered: a full barrier keeps it simple.
    VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = img.layout,
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img.image,
        .subresourceRange = kColorRange,
    };
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(m_cmd, &dep);
    img.layout = layout;
}

void VulkanRenderer::destroyScratch()
{
    m_scene.reset();
    m_levels.clear();
    m_blurOut.reset();
    m_capture.reset();
}

// ------------------------------------------------------------------ frames --

bool VulkanRenderer::render(VulkanImage& target, std::array<float, 4> rgba, std::span<const TextureDraw> draws)
{
    // One frame in flight for now; the frame pacing work will add more.
    if (!waitIdle())
        return false;

    std::vector<VkSemaphoreSubmitInfo> waits;

    // Our target: writing, so wait for everyone (readers and writers) still using it.
    if (importSyncFile(m_waitSemaphore, dmabuf::exportSyncFile(target.dmabufFd(), dmabuf::Access::Write)))
        waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m_waitSemaphore,
                         .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT});

    // Client dma-bufs: reading, so wait for the client's rendering (writers) only.
    std::vector<VulkanTexture*> imported;
    auto noteTexture = [&imported](VulkanTexture* t) {
        if (t && t->imported() && std::ranges::find(imported, t) == imported.end())
            imported.push_back(t);
    };
    bool offscreen = false;
    for (const auto& d : draws) {
        noteTexture(d.texture);
        if (d.backdrop) {
            noteTexture(d.backdrop->mask);
            offscreen = true;
        }
    }
    while (m_textureWaits.size() < imported.size()) {
        VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        if (!check(vkCreateSemaphore(m_device.device(), &info, nullptr, &sem), "vkCreateSemaphore(texture wait)"))
            return false;
        m_textureWaits.push_back(sem);
    }
    for (std::size_t i = 0; i < imported.size(); ++i) {
        if (importSyncFile(m_textureWaits[i], dmabuf::exportSyncFile(imported[i]->dmabufFd(), dmabuf::Access::Read)))
            waits.push_back({.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m_textureWaits[i],
                             .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT});
    }

    const VkExtent2D extent = target.extent();
    const VkFormat format = target.format();
    if (offscreen && !ensureScratch(m_scene, extent, format, "scene image"))
        offscreen = false; // no blur this frame, but still a frame

    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo begin{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(m_cmd, &begin);

    const std::uint64_t frame = m_submitted + 1;
    m_blank->recordUpload(m_cmd, frame);
    for (const auto& d : draws) {
        if (d.texture)
            d.texture->recordUpload(m_cmd, frame);
        if (d.backdrop && d.backdrop->mask)
            d.backdrop->mask->recordUpload(m_cmd, frame);
    }

    // Acquire everything other processes own: our target (UNDEFINED: we
    // overwrite every pixel) and client images (GENERAL: keep their content).
    std::vector<VkImageMemoryBarrier2> acquire;
    acquire.push_back({
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
    });
    for (auto* tex : imported) {
        acquire.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = m_device.queueFamily(),
            .image = tex->m_image,
            .subresourceRange = kColorRange,
        });
    }
    VkDependencyInfo acquireDep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::uint32_t(acquire.size()),
        .pImageMemoryBarriers = acquire.data(),
    };
    vkCmdPipelineBarrier2(m_cmd, &acquireDep);

    VkPipeline pipeline = pipelineFor(format);
    if (!pipeline) {
        vkEndCommandBuffer(m_cmd);
        return false;
    }

    // Starts rendering into `view` (`area` of an `ext`-sized image) and sets up the pipeline.
    auto beginPass = [&](VkImageView view, VkExtent2D ext, VkImageLayout layout, VkAttachmentLoadOp load,
                         const Rect& area, std::array<float, 4> clear) {
        VkRenderingAttachmentInfo color{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view,
            .imageLayout = layout,
            .loadOp = load,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {clear[0], clear[1], clear[2], clear[3]}}},
        };
        VkRenderingInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = {{area.x, area.y}, {std::uint32_t(area.width), std::uint32_t(area.height)}},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color,
        };
        vkCmdBeginRendering(m_cmd, &info);
        vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport vp{0.0f, 0.0f, float(ext.width), float(ext.height), 0.0f, 1.0f};
        vkCmdSetViewport(m_cmd, 0, 1, &vp);
        const VkRect2D sc{{area.x, area.y}, {std::uint32_t(area.width), std::uint32_t(area.height)}};
        vkCmdSetScissor(m_cmd, 0, 1, &sc);
    };
    // One quad: `push.rect` in NDC; samples `view` (and `maskView`).
    auto drawQuad = [&](QuadPush& push, VkImageView view, VkImageView maskView) {
        VkDescriptorImageInfo images[2] = {
            {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.imageView = maskView, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkWriteDescriptorSet writes[2];
        for (std::uint32_t i = 0; i < 2; ++i) {
            writes[i] = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &images[i],
            };
        }
        m_device.fn().cmdPushDescriptorSetKHR(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 2,
                                              writes);
        vkCmdPushConstants(m_cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDraw(m_cmd, 4, 1, 0, 0);
    };
    auto ndc = [](const Rect& r, VkExtent2D ext, float (&out)[4]) {
        const float sx = 2.0f / float(ext.width), sy = 2.0f / float(ext.height);
        out[0] = float(r.x) * sx - 1.0f, out[1] = float(r.y) * sy - 1.0f;
        out[2] = float(r.width) * sx, out[3] = float(r.height) * sy;
    };
    auto uvOf = [](const Rect& r, VkExtent2D ext, float (&out)[4]) {
        out[0] = float(r.x) / float(ext.width), out[1] = float(r.y) / float(ext.height);
        out[2] = float(r.x + r.width) / float(ext.width), out[3] = float(r.y + r.height) / float(ext.height);
    };

    const Rect full{0, 0, std::int32_t(extent.width), std::int32_t(extent.height)};
    VkImageView sceneView = offscreen ? m_scene->view : target.view();
    auto beginScene = [&](VkAttachmentLoadOp load) {
        if (offscreen)
            transition(*m_scene, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        beginPass(sceneView, extent, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, load, full, rgba);
    };

    // Dual Kawase: down `passes` times by halves, then back up to half size,
    // all limited to `region` plus the blur's reach. Result: m_blurOut.
    auto blurChain = [&](const Rect& region, const BlurParams& p) -> bool {
        const int passes = std::clamp(p.passes, 1, 6);
        // Only ever grows: a smaller blur later in the frame must not free
        // levels the command buffer already uses.
        if (m_levels.size() < std::size_t(passes))
            m_levels.resize(std::size_t(passes));
        for (int i = 0; i < passes; ++i) {
            const VkExtent2D e{std::max(1u, extent.width >> (i + 1)), std::max(1u, extent.height >> (i + 1))};
            if (!ensureScratch(m_levels[std::size_t(i)], e, format, "blur level"))
                return false;
        }
        if (!ensureScratch(m_blurOut, m_levels[0]->extent, format, "blur output"))
            return false;
        const std::int32_t reach = p.size * (2 << passes);
        const Rect work = region.adjusted(reach).intersected(full);
        auto scaled = [&](int level, VkExtent2D e) { // `work` in a level's pixels, rounded outward
            const int f = 1 << level;
            const std::int32_t x0 = work.x / f, y0 = work.y / f;
            const std::int32_t x1 = (work.x + work.width + f - 1) / f, y1 = (work.y + work.height + f - 1) / f;
            return Rect{x0, y0, std::min(x1, std::int32_t(e.width)) - x0, std::min(y1, std::int32_t(e.height)) - y0};
        };
        auto pass = [&](Scratch& src, VkExtent2D srcExt, Scratch& dst, int level, std::uint32_t mode) {
            transition(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            transition(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            const Rect area = scaled(level, dst.extent);
            if (area.width <= 0 || area.height <= 0)
                return;
            beginPass(dst.view, dst.extent, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                      area, {0, 0, 0, 1});
            QuadPush push{.alpha = 1.0f, .mode = mode};
            ndc(area, dst.extent, push.rect);
            uvOf(area, dst.extent, push.uv);
            push.color1[0] = 1.0f / float(srcExt.width);
            push.color1[1] = 1.0f / float(srcExt.height);
            push.color1[2] = float(p.size);
            drawQuad(push, src.view, m_blank->m_view);
            vkCmdEndRendering(m_cmd);
        };
        Scratch* src = m_scene.get();
        for (int i = 0; i < passes; ++i) {
            pass(*src, src->extent, *m_levels[std::size_t(i)], i + 1, kModeBlurDown);
            src = m_levels[std::size_t(i)].get();
        }
        for (int i = passes - 1; i >= 1; --i) {
            pass(*src, src->extent, *m_levels[std::size_t(i - 1)], i, kModeBlurUp);
            src = m_levels[std::size_t(i - 1)].get();
        }
        pass(*src, src->extent, *m_blurOut, 1, kModeBlurUp);
        transition(*m_blurOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return true;
    };
    // The blurred backdrop, back into the scene.
    auto drawBlurred = [&](const TextureDraw& d, Scratch& blurred) {
        const BackdropDraw& b = *d.backdrop;
        QuadPush push{.alpha = d.alpha, .mode = kModeBlurFinish};
        const Rect dst = b.region.rect.intersected(full);
        if (dst.width <= 0 || dst.height <= 0)
            return;
        ndc(dst, extent, push.rect);
        uvOf(dst, extent, push.uv); // blur images cover the whole frame, at half size
        copy4(push.clipRect, b.region.rect);
        push.clipRadius = b.region.radius;
        push.color1[0] = b.params.noise;
        push.color1[1] = b.params.contrast;
        push.color1[2] = b.params.brightness;
        push.color1[3] = b.params.vibrancy;
        VkImageView maskView = m_blank->m_view;
        if (b.mask && b.mask->ready()) {
            push.mode |= kHasMask;
            copy4(push.shapeRect, b.maskRect);
            push.shapeSize = b.maskThreshold;
            maskView = b.mask->m_view;
            b.mask->m_lastUsedFrame = frame;
        }
        drawQuad(push, blurred.view, maskView);
    };

    if (offscreen)
        m_scene->layout = VK_IMAGE_LAYOUT_UNDEFINED; // cleared anyway
    beginScene(VK_ATTACHMENT_LOAD_OP_CLEAR);
    for (const auto& d : draws) {
        if (d.backdrop) {
            if (!offscreen)
                continue;
            if (d.captureBackdrop || !d.backdrop->fromCapture) {
                vkCmdEndRendering(m_cmd);
                const bool ok = blurChain(d.captureBackdrop ? full : d.backdrop->region.rect, d.backdrop->params);
                if (ok && d.captureBackdrop && ensureScratch(m_capture, m_blurOut->extent, format, "blur capture")) {
                    transition(*m_blurOut, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    transition(*m_capture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    const VkImageCopy region{
                        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .extent = {m_capture->extent.width, m_capture->extent.height, 1},
                    };
                    vkCmdCopyImage(m_cmd, m_blurOut->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_capture->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                    transition(*m_capture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    transition(*m_blurOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
                beginScene(VK_ATTACHMENT_LOAD_OP_LOAD);
                if (ok && !d.captureBackdrop)
                    drawBlurred(d, *m_blurOut);
            } else if (m_capture && m_capture->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                drawBlurred(d, *m_capture);
            }
            continue;
        }

        VulkanTexture* texture = d.texture ? d.texture : m_blank.get();
        if (!texture->ready())
            continue;
        const Rect sc = d.scissor ? d.scissor->intersected(full) : full;
        if (sc.width <= 0 || sc.height <= 0)
            continue;
        const VkRect2D scissor{{sc.x, sc.y}, {std::uint32_t(sc.width), std::uint32_t(sc.height)}};
        vkCmdSetScissor(m_cmd, 0, 1, &scissor);
        QuadPush push{
            .uv = {0.0f, 0.0f, 1.0f, 1.0f},
            .alpha = d.alpha,
            .opaque = texture->opaque() ? 1u : 0u,
        };
        ndc(d.dst, extent, push.rect);
        if (d.clip) {
            copy4(push.clipRect, d.clip->rect);
            push.clipRadius = d.clip->radius;
            if (d.invertClip)
                push.mode |= kInvertClip;
        }
        if (!d.texture) {
            using Kind = ShapeDraw::Kind;
            push.mode |= d.shape.kind == Kind::Shadow ? kModeShadow : d.shape.kind == Kind::Fill ? kModeFill : kModeBorder;
            copy4(push.shapeRect, d.shape.rect.rect);
            push.shapeRadius = d.shape.rect.radius;
            push.shapeSize = std::max(d.shape.size, 0.5f);
            copy4(push.color1, d.shape.color);
            copy4(push.color2, d.shape.color2);
            push.angle = d.shape.angle;
        } else {
            push.mode |= kModeTexture;
        }
        drawQuad(push, texture->m_view, m_blank->m_view);
        const VkRect2D all{{0, 0}, extent};
        vkCmdSetScissor(m_cmd, 0, 1, &all);
        texture->m_lastUsedFrame = frame;
    }
    vkCmdEndRendering(m_cmd);

    // The offscreen frame onto the real target.
    if (offscreen) {
        transition(*m_scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        beginPass(target.view(), extent, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                  full, rgba);
        QuadPush push{.uv = {0.0f, 0.0f, 1.0f, 1.0f}, .alpha = 1.0f, .mode = kModeTexture, .opaque = 1u};
        ndc(full, extent, push.rect);
        drawQuad(push, m_scene->view, m_blank->m_view);
        vkCmdEndRendering(m_cmd);
    }

    // Hand everything back to FOREIGN in GENERAL, the layout other processes expect.
    std::vector<VkImageMemoryBarrier2> release;
    release.push_back({
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
    });
    for (auto* tex : imported) {
        release.push_back({
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_NONE, // reads only: nothing to flush
            .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstAccessMask = VK_ACCESS_2_NONE,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = m_device.queueFamily(),
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image = tex->m_image,
            .subresourceRange = kColorRange,
        });
    }
    VkDependencyInfo releaseDep{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = std::uint32_t(release.size()),
        .pImageMemoryBarriers = release.data(),
    };
    vkCmdPipelineBarrier2(m_cmd, &releaseDep);

    if (!check(vkEndCommandBuffer(m_cmd), "vkEndCommandBuffer"))
        return false;
    return submit(target, waits, imported);
}

bool VulkanRenderer::submit(VulkanImage& target, std::span<const VkSemaphoreSubmitInfo> waits,
                            std::span<VulkanTexture* const> imported)
{
    VkSemaphoreSubmitInfo signal{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_signalSemaphore,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cmd{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = m_cmd};
    VkSubmitInfo2 submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = std::uint32_t(waits.size()),
        .pWaitSemaphoreInfos = waits.data(),
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

    // Hand our completion to implicit sync: the next user of each dma-buf
    // waits for it in the kernel (KMS / the parent compositor for the target,
    // the client's next render into the buffers we sampled).
    VkSemaphoreGetFdInfoKHR getFd{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = m_signalSemaphore,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    int fd = -1;
    if (!check(m_device.fn().getSemaphoreFdKHR(m_device.device(), &getFd, &fd), "vkGetSemaphoreFdKHR"))
        return false;
    UniqueFd syncFile(fd); // -1 means "already signaled", nothing to attach
    if (syncFile) {
        dmabuf::importSyncFile(target.dmabufFd(), syncFile.get(), dmabuf::Access::Write);
        for (auto* tex : imported)
            dmabuf::importSyncFile(tex->dmabufFd(), syncFile.get(), dmabuf::Access::Read);
    }
    return true;
}

} // namespace dragon::vk
