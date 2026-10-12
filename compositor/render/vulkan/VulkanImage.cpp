#include "render/vulkan/VulkanImage.h"

#include "core/Log.h"
#include "render/vulkan/Formats.h"
#include "render/vulkan/VulkanDevice.h"

#include <bit>

namespace dragon::vk {

std::unique_ptr<VulkanImage> VulkanImage::importDmaBuf(VulkanDevice& device, const DmaBuf& buf,
                                                       VkImageUsageFlags usage, const char* debugName)
{
    const VkFormat format = formatFromDrm(buf.format);
    if (format == VK_FORMAT_UNDEFINED) {
        log::error("vulkan", "unsupported DRM format {:#010x}", buf.format);
        return nullptr;
    }
    if (buf.planes.size() != 1) {
        log::error("vulkan", "multi-plane dma-bufs not supported yet ({} planes)", buf.planes.size());
        return nullptr;
    }

    std::unique_ptr<VulkanImage> img(new VulkanImage(device));
    img->m_format = format;
    img->m_extent = {buf.width, buf.height};
    VkDevice dev = device.device();

    // 1. Image whose layout is described by the dma-buf's modifier + plane layout.
    const VkSubresourceLayout planeLayout{
        .offset = buf.planes[0].offset,
        .size = 0, // must be 0 for explicit modifier layouts
        .rowPitch = buf.planes[0].stride,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = buf.modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &planeLayout,
    };
    VkExternalMemoryImageCreateInfo extInfo{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modInfo,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {buf.width, buf.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (!check(vkCreateImage(dev, &imageInfo, nullptr, &img->m_image), "vkCreateImage(dmabuf)"))
        return nullptr;

    // 2. Import the fd as device memory. Vulkan takes ownership of the fd on
    //    success, so hand it a duplicate.
    UniqueFd memFd = buf.planes[0].fd.duplicate();
    VkMemoryFdPropertiesKHR fdProps{.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (!check(device.fn().getMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                                    memFd.get(), &fdProps),
               "vkGetMemoryFdPropertiesKHR"))
        return nullptr;

    VkImageMemoryRequirementsInfo2 reqInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = img->m_image,
    };
    VkMemoryRequirements2 reqs{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetImageMemoryRequirements2(dev, &reqInfo, &reqs);

    const auto memType = device.findMemoryType(reqs.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits);
    if (!memType) {
        log::error("vulkan", "no memory type compatible with the dma-buf");
        return nullptr;
    }

    VkMemoryDedicatedAllocateInfo dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = img->m_image,
    };
    VkImportMemoryFdInfoKHR importInfo{
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = memFd.get(),
    };
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &importInfo,
        .allocationSize = reqs.memoryRequirements.size,
        .memoryTypeIndex = *memType,
    };
    if (!check(vkAllocateMemory(dev, &allocInfo, nullptr, &img->m_memory), "vkAllocateMemory(dmabuf import)"))
        return nullptr;
    memFd.release(); // now owned by the VkDeviceMemory

    if (!check(vkBindImageMemory(dev, img->m_image, img->m_memory, 0), "vkBindImageMemory"))
        return nullptr;

    // 3. View for rendering/sampling.
    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->m_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (!check(vkCreateImageView(dev, &viewInfo, nullptr, &img->m_view), "vkCreateImageView"))
        return nullptr;

    img->m_syncFd = buf.planes[0].fd.duplicate();

    if (debugName) {
        device.setName(VK_OBJECT_TYPE_IMAGE, std::bit_cast<std::uint64_t>(img->m_image), debugName);
        device.setName(VK_OBJECT_TYPE_IMAGE_VIEW, std::bit_cast<std::uint64_t>(img->m_view), debugName);
    }
    return img;
}

VulkanImage::~VulkanImage()
{
    VkDevice dev = m_device.device();
    if (m_view)
        vkDestroyImageView(dev, m_view, nullptr);
    if (m_image)
        vkDestroyImage(dev, m_image, nullptr);
    if (m_memory)
        vkFreeMemory(dev, m_memory, nullptr);
}

} // namespace dragon::vk
