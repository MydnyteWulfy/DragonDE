#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <sys/types.h>
#include <vector>
#include <vulkan/vulkan.h>

namespace dragon::vk {

// Logs and returns false on failure: `if (!check(vkFoo(...), "vkFoo")) return ...;`
bool check(VkResult result, std::string_view what);

// Extension entry points, loaded once per device.
struct DeviceFunctions {
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdPropertiesKHR = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFdKHR = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFdKHR = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT getImageDrmFormatModifierPropertiesEXT = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT setDebugUtilsObjectNameEXT = nullptr; // null without validation
};

// Instance + physical device + logical device + one graphics queue, for the
// GPU behind a specific DRM render node (so Vulkan and GBM agree on the GPU).
class VulkanDevice {
public:
    static std::unique_ptr<VulkanDevice> create(dev_t renderNode, bool validation);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    std::uint32_t queueFamily() const { return m_queueFamily; }
    const DeviceFunctions& fn() const { return m_fn; }

    // DRM modifiers we can import as single-plane dma-bufs with `usage`, for `format`.
    std::vector<std::uint64_t> supportedModifiers(VkFormat format, VkImageUsageFlags usage) const;

    std::optional<std::uint32_t> findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags props = 0) const;

    // Names an object in validation messages and tools like RenderDoc. No-op without validation.
    void setName(VkObjectType type, std::uint64_t handle, const char* name) const;

    // Validation errors reported so far (tests assert this stays 0).
    std::uint32_t validationErrors() const { return m_validationErrors.load(); }

private:
    VulkanDevice() = default;
    bool init(dev_t renderNode, bool validation);
    bool createInstance(bool validation);
    bool pickPhysicalDevice(dev_t renderNode);
    bool createDevice();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                         void* user);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    std::uint32_t m_queueFamily = 0;
    VkPhysicalDeviceMemoryProperties m_memoryProps{};
    DeviceFunctions m_fn;
    std::atomic<std::uint32_t> m_validationErrors{0};
};

} // namespace dragon::vk
