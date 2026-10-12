#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace dragon::vk {

// DRM fourcc -> VkFormat for the formats we render to and sample from.
// DRM names list components from most to least significant bit of a
// little-endian word, Vulkan lists them in byte order, so the names look
// reversed: DRM_FORMAT_XRGB8888 == VK_FORMAT_B8G8R8A8_UNORM.
// Returns VK_FORMAT_UNDEFINED for unsupported formats.
VkFormat formatFromDrm(std::uint32_t fourcc);

// True for X* formats whose alpha channel must be ignored when sampling.
bool drmFormatHasAlpha(std::uint32_t fourcc);

const char* resultString(VkResult result);

} // namespace dragon::vk
