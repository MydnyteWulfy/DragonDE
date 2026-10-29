#pragma once

#include "core/Geometry.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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

// A rectangle with rounded corners, in target pixels.
struct RoundedRect {
    Rect rect;
    float radius = 0;
};

// A shape drawn without a texture (window frames).
struct ShapeDraw {
    enum class Kind { Shadow, Border, Fill };
    Kind kind = Kind::Border;
    RoundedRect rect;  // the window: the border goes around it, the shadow falls from it
    float size = 0;    // border width / shadow blur distance
    std::array<float, 4> color{}; // straight RGBA
    std::array<float, 4> color2{}; // border gradient end (= color for solid)
    float angle = 0;   // border: gradient direction, radians; shadow: falloff power
};

struct BlurParams {
    std::int32_t size = 8;   // spread per pass
    std::int32_t passes = 2; // 1-6
    float noise = 0.0117f, contrast = 0.8916f, brightness = 0.8172f, vibrancy = 0.1696f;
};

// Blur what has been drawn so far behind `region` and put it back there
// (frosted glass under a translucent window).
struct BackdropDraw {
    RoundedRect region;
    BlurParams params;
    // Use the blur captured earlier this frame (a capture draw) instead of
    // blurring what's behind right now: cheaper, sees through windows.
    bool fromCapture = false;
    // Only where this texture (placed over maskRect) is at least this opaque.
    VulkanTexture* mask = nullptr;
    Rect maskRect;
    float maskThreshold = 0;
};

struct TextureDraw {
    VulkanTexture* texture = nullptr; // null: `shape` is drawn
    Rect dst;                         // target pixels the quad covers
    float alpha = 1.0f;
    std::optional<Rect> scissor;      // hard clip
    std::optional<RoundedRect> clip;  // antialiased clip
    bool invertClip = false;          // draw only outside `clip`
    ShapeDraw shape;
    std::optional<BackdropDraw> backdrop; // instead of a texture or shape
    // With `backdrop`: blur the whole frame so far for later fromCapture draws.
    bool captureBackdrop = false;
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
    explicit VulkanRenderer(VulkanDevice& device);
    bool init();
    bool createPipelineLayout();
    VkPipeline pipelineFor(VkFormat format);

    // GPU-only images for offscreen rendering (the frame, blur levels).
    struct Scratch;
    bool ensureScratch(std::unique_ptr<Scratch>& image, VkExtent2D extent, VkFormat format, const char* name);
    void transition(Scratch& image, VkImageLayout layout);
    void destroyScratch();
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
    std::unique_ptr<VulkanTexture> m_blank;      // bound for shape draws, which sample nothing
    std::unique_ptr<Scratch> m_scene;            // the frame, when it needs blur
    std::vector<std::unique_ptr<Scratch>> m_levels; // blur chain: 1/2, 1/4, ... size
    std::unique_ptr<Scratch> m_blurOut;          // the last blur (half size)
    std::unique_ptr<Scratch> m_capture;          // a captured blur (xray)
};

} // namespace dragon::vk
