#include "render/vulkan/VulkanDevice.h"

#include "core/Log.h"
#include "render/vulkan/Formats.h"

#include <array>
#include <cstring>
#include <set>
#include <string>
#include <sys/sysmacros.h>

namespace dragon::vk {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

constexpr std::array kDeviceExtensions = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,      // import memory from fds
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, // ...specifically dma-bufs
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,   // sync_file <-> semaphore
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,    // ownership transfer to KMS / other processes
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,     // match the device to a DRM node
};

bool hasLayer(const char* name)
{
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0)
            return true;
    }
    return false;
}

std::set<std::string> deviceExtensions(VkPhysicalDevice pd)
{
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    std::set<std::string> out;
    for (const auto& e : exts)
        out.emplace(e.extensionName);
    return out;
}

} // namespace

bool check(VkResult result, std::string_view what)
{
    if (result == VK_SUCCESS)
        return true;
    log::error("vulkan", "{} failed: {}", what, resultString(result));
    return false;
}

std::unique_ptr<VulkanDevice> VulkanDevice::create(dev_t renderNode, bool validation)
{
    std::unique_ptr<VulkanDevice> dev(new VulkanDevice());
    if (!dev->init(renderNode, validation))
        return nullptr;
    return dev;
}

VulkanDevice::~VulkanDevice()
{
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_messenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(m_instance, m_messenger, nullptr);
    }
    if (m_instance)
        vkDestroyInstance(m_instance, nullptr);
}

bool VulkanDevice::init(dev_t renderNode, bool validation)
{
    return createInstance(validation) && pickPhysicalDevice(renderNode) && createDevice();
}

bool VulkanDevice::createInstance(bool validation)
{
    if (validation && !hasLayer(kValidationLayer)) {
        log::warn("vulkan", "{} not installed, running without validation", kValidationLayer);
        validation = false;
    }

    VkApplicationInfo app{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "dragon-compositor",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "DragonDE",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = VK_API_VERSION_1_3,
    };

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debugCallback,
        .pUserData = this,
    };

    const char* layers[] = {kValidationLayer};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    VkInstanceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = validation ? &messengerInfo : nullptr, // also catches instance create/destroy messages
        .pApplicationInfo = &app,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = validation ? 1u : 0u,
        .ppEnabledExtensionNames = extensions,
    };
    if (!check(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance"))
        return false;

    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        check(create(m_instance, &messengerInfo, nullptr, &m_messenger), "vkCreateDebugUtilsMessengerEXT");
        m_fn.setDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(m_instance, "vkSetDebugUtilsObjectNameEXT"));
        log::info("vulkan", "validation layers enabled");
    }
    return true;
}

bool VulkanDevice::pickPhysicalDevice(dev_t renderNode)
{
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    for (auto pd : devices) {
        VkPhysicalDeviceProperties basic{};
        vkGetPhysicalDeviceProperties(pd, &basic);

        if (basic.apiVersion < VK_API_VERSION_1_3) {
            log::debug("vulkan", "skipping {}: Vulkan {}.{} < 1.3", basic.deviceName,
                       VK_API_VERSION_MAJOR(basic.apiVersion), VK_API_VERSION_MINOR(basic.apiVersion));
            continue;
        }

        const auto exts = deviceExtensions(pd);
        bool missing = false;
        for (const char* e : kDeviceExtensions) {
            if (!exts.contains(e)) {
                log::debug("vulkan", "skipping {}: missing {}", basic.deviceName, e);
                missing = true;
            }
        }
        if (missing)
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drm};
        vkGetPhysicalDeviceProperties2(pd, &props);

        if (!drm.hasRender ||
            makedev(dev_t(drm.renderMajor), dev_t(drm.renderMinor)) != renderNode)
            continue;

        m_physical = pd;
        vkGetPhysicalDeviceMemoryProperties(pd, &m_memoryProps);
        log::info("vulkan", "using {} (Vulkan {}.{}.{}, driver {:#x})", basic.deviceName,
                  VK_API_VERSION_MAJOR(basic.apiVersion), VK_API_VERSION_MINOR(basic.apiVersion),
                  VK_API_VERSION_PATCH(basic.apiVersion), basic.driverVersion);
        return true;
    }

    log::error("vulkan", "no Vulkan 1.3 device with the required extensions matches render node {}:{}",
               major(renderNode), minor(renderNode));
    return false;
}

bool VulkanDevice::createDevice()
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &count, families.data());

    bool found = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_queueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) {
        log::error("vulkan", "no graphics queue family");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_queueFamily,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    VkPhysicalDeviceVulkan13Features features13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features features12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &features13,
        .timelineSemaphore = VK_TRUE,
    };

    VkDeviceCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = std::uint32_t(kDeviceExtensions.size()),
        .ppEnabledExtensionNames = kDeviceExtensions.data(),
    };
    if (!check(vkCreateDevice(m_physical, &info, nullptr, &m_device), "vkCreateDevice"))
        return false;

    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    auto load = [this](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetDeviceProcAddr(m_device, name));
        if (!fn)
            log::error("vulkan", "missing entry point {}", name);
        return fn != nullptr;
    };
    return load(m_fn.getMemoryFdPropertiesKHR, "vkGetMemoryFdPropertiesKHR") &&
           load(m_fn.getSemaphoreFdKHR, "vkGetSemaphoreFdKHR") &&
           load(m_fn.importSemaphoreFdKHR, "vkImportSemaphoreFdKHR") &&
           load(m_fn.getImageDrmFormatModifierPropertiesEXT, "vkGetImageDrmFormatModifierPropertiesEXT");
}

std::vector<std::uint64_t> VulkanDevice::supportedModifiers(VkFormat format, VkImageUsageFlags usage) const
{
    VkDrmFormatModifierPropertiesListEXT list{.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
    vkGetPhysicalDeviceFormatProperties2(m_physical, format, &props);

    std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vkGetPhysicalDeviceFormatProperties2(m_physical, format, &props);

    VkFormatFeatureFlags needed = 0;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

    std::vector<std::uint64_t> out;
    for (const auto& m : mods) {
        // Multi-plane (e.g. compression metadata) modifiers come later.
        if (m.drmFormatModifierPlaneCount != 1 || (m.drmFormatModifierTilingFeatures & needed) != needed)
            continue;

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = m.drmFormatModifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkPhysicalDeviceExternalImageFormatInfo extInfo{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &modInfo,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkPhysicalDeviceImageFormatInfo2 info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &extInfo,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
        };
        VkExternalImageFormatProperties extProps{.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 imgProps{.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extProps};

        if (vkGetPhysicalDeviceImageFormatProperties2(m_physical, &info, &imgProps) != VK_SUCCESS)
            continue;
        if (!(extProps.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
            continue;
        out.push_back(m.drmFormatModifier);
    }
    return out;
}

std::optional<std::uint32_t> VulkanDevice::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    for (std::uint32_t i = 0; i < m_memoryProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (m_memoryProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return std::nullopt;
}

void VulkanDevice::setName(VkObjectType type, std::uint64_t handle, const char* name) const
{
    if (!m_fn.setDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = type,
        .objectHandle = handle,
        .pObjectName = name,
    };
    m_fn.setDebugUtilsObjectNameEXT(m_device, &info);
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                            VkDebugUtilsMessageTypeFlagsEXT,
                                                            const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                            void* user)
{
    auto* self = static_cast<VulkanDevice*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        self->m_validationErrors.fetch_add(1);
        log::error("vulkan", "{}", data->pMessage);
    } else {
        log::warn("vulkan", "{}", data->pMessage);
    }
    return VK_FALSE;
}

} // namespace dragon::vk
