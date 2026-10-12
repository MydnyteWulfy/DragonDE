// GPU tests: real render node, real GBM buffers, real Vulkan with validation.
// Skipped (not failed) on machines without a usable GPU.

#include "core/Log.h"
#include "render/GbmAllocator.h"
#include "render/RenderNode.h"
#include "render/vulkan/VulkanDevice.h"
#include "render/vulkan/VulkanImage.h"
#include "render/vulkan/VulkanRenderer.h"
#include "render/vulkan/VulkanTexture.h"

#include <cstdlib>
#include <drm_fourcc.h>
#include <gbm.h>
#include <gtest/gtest.h>

using namespace dragon;

namespace {

struct GpuFixture : ::testing::Test {
    std::unique_ptr<GbmAllocator> allocator;
    std::unique_ptr<vk::VulkanDevice> device;
    std::unique_ptr<vk::VulkanRenderer> renderer;

    void SetUp() override
    {
        log::setLevel(LogLevel::Warn);
        auto node = openRenderNode();
        if (!node) {
            GTEST_SKIP() << "no DRM render node";
        }
        const dev_t dev = node->device;

        allocator = GbmAllocator::create(std::move(node->fd));
        ASSERT_NE(allocator, nullptr);

        device = vk::VulkanDevice::create(dev, /*validation=*/true);
        if (!device) {
            GTEST_SKIP() << "no suitable Vulkan device";
        }
        // "0 validation errors" means nothing if the layer isn't loaded.
        ASSERT_NE(device->fn().setDebugUtilsObjectNameEXT, nullptr) << "validation layer not active";

        renderer = vk::VulkanRenderer::create(*device);
        ASSERT_NE(renderer, nullptr);
    }

    void TearDown() override
    {
        if (device) {
            EXPECT_EQ(device->validationErrors(), 0u) << "Vulkan validation reported errors (see log)";
        }
        renderer.reset(); // before the device it was created from
        device.reset();
    }

    // Reads pixel (x, y) of a LINEAR XRGB8888 buffer through a CPU mapping.
    static std::uint32_t readPixel(GbmBuffer& buf, std::uint32_t x, std::uint32_t y)
    {
        std::uint32_t stride = 0;
        void* mapData = nullptr;
        auto* base = static_cast<std::uint8_t*>(gbm_bo_map(buf.bo(), 0, 0, buf.dmabuf().width,
                                                           buf.dmabuf().height, GBM_BO_TRANSFER_READ, &stride,
                                                           &mapData));
        EXPECT_NE(base, nullptr);
        if (!base)
            return 0;
        const auto px = *reinterpret_cast<const std::uint32_t*>(base + y * stride + x * 4);
        gbm_bo_unmap(buf.bo(), mapData);
        return px;
    }
};

} // namespace

TEST_F(GpuFixture, OffersRenderableModifiers)
{
    const auto mods = device->supportedModifiers(VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    EXPECT_FALSE(mods.empty());
    EXPECT_NE(std::ranges::find(mods, DRM_FORMAT_MOD_LINEAR), mods.end());
}

TEST_F(GpuFixture, ClearsLinearBufferToExactColor)
{
    const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    auto buf = allocator->allocate(64, 48, DRM_FORMAT_XRGB8888, {&linear, 1}, GBM_BO_USE_RENDERING);
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(buf->dmabuf().modifier, DRM_FORMAT_MOD_LINEAR);

    auto image = vk::VulkanImage::importDmaBuf(*device, buf->dmabuf(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                               "test target");
    ASSERT_NE(image, nullptr);

    ASSERT_TRUE(renderer->clear(*image, {1.0f, 0.0f, 0.0f, 1.0f}));
    ASSERT_TRUE(renderer->waitIdle());
    // XRGB8888 is a little-endian 0xXXRRGGBB word.
    EXPECT_EQ(readPixel(*buf, 0, 0) & 0x00ffffff, 0x00ff0000u);
    EXPECT_EQ(readPixel(*buf, 63, 47) & 0x00ffffff, 0x00ff0000u);

    // Second frame into the same buffer exercises the implicit-sync wait path.
    ASSERT_TRUE(renderer->clear(*image, {0.0f, 1.0f, 0.0f, 1.0f}));
    ASSERT_TRUE(renderer->waitIdle());
    EXPECT_EQ(readPixel(*buf, 10, 10) & 0x00ffffff, 0x0000ff00u);
}

TEST_F(GpuFixture, ClearsTiledBufferWithDriverChosenModifier)
{
    // Tiled/compressed layouts can't be read back by the CPU; this checks that
    // the full modifier negotiation + import + render path is valid.
    const auto mods = device->supportedModifiers(VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ASSERT_FALSE(mods.empty());

    auto buf = allocator->allocate(1920, 1080, DRM_FORMAT_XRGB8888, mods, GBM_BO_USE_RENDERING);
    ASSERT_NE(buf, nullptr);
    EXPECT_NE(std::ranges::find(mods, buf->dmabuf().modifier), mods.end()) << "GBM chose an unoffered modifier";

    auto image = vk::VulkanImage::importDmaBuf(*device, buf->dmabuf(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ASSERT_NE(image, nullptr);

    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(renderer->clear(*image, {0.1f * float(i), 0.2f, 0.3f, 1.0f}));
    ASSERT_TRUE(renderer->waitIdle());
}

// ------------------------------------------------------------ textured quads --

namespace {
struct LinearTarget {
    std::unique_ptr<GbmBuffer> buffer;
    std::unique_ptr<vk::VulkanImage> image;
};

LinearTarget makeLinearTarget(GbmAllocator& allocator, vk::VulkanDevice& device, std::uint32_t w, std::uint32_t h)
{
    const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    LinearTarget t;
    t.buffer = allocator.allocate(w, h, DRM_FORMAT_XRGB8888, {&linear, 1}, GBM_BO_USE_RENDERING);
    if (t.buffer)
        t.image = vk::VulkanImage::importDmaBuf(device, t.buffer->dmabuf(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    return t;
}

// |a - b| <= 1 per channel (blending rounds).
bool closeTo(std::uint32_t a, std::uint32_t b)
{
    for (int shift = 0; shift < 24; shift += 8) {
        const int ca = int((a >> shift) & 0xff), cb = int((b >> shift) & 0xff);
        if (std::abs(ca - cb) > 1)
            return false;
    }
    return true;
}
} // namespace

TEST_F(GpuFixture, TextureLandsOnExactPixels)
{
    auto target = makeLinearTarget(*allocator, *device, 64, 48);
    ASSERT_NE(target.image, nullptr);

    // B8G8R8A8 texels read as little-endian 0xAARRGGBB words.
    const std::uint32_t texels[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
    auto tex = renderer->createTexture(2, 2, VK_FORMAT_B8G8R8A8_UNORM, false);
    ASSERT_NE(tex, nullptr);
    const Rect full{0, 0, 2, 2};
    tex->write(texels, 8, {&full, 1});

    const vk::TextureDraw draw{tex.get(), {10, 10, 2, 2}};
    ASSERT_TRUE(renderer->render(*target.image, {0, 0, 0, 1}, {&draw, 1}));
    ASSERT_TRUE(renderer->waitIdle());

    EXPECT_EQ(readPixel(*target.buffer, 10, 10) & 0xffffff, 0xff0000u);
    EXPECT_EQ(readPixel(*target.buffer, 11, 10) & 0xffffff, 0x00ff00u);
    EXPECT_EQ(readPixel(*target.buffer, 10, 11) & 0xffffff, 0x0000ffu);
    EXPECT_EQ(readPixel(*target.buffer, 11, 11) & 0xffffff, 0xffffffu);
    EXPECT_EQ(readPixel(*target.buffer, 9, 9) & 0xffffff, 0x000000u);
    EXPECT_EQ(readPixel(*target.buffer, 12, 12) & 0xffffff, 0x000000u);
}

TEST_F(GpuFixture, PremultipliedAlphaBlendsOverBackground)
{
    auto target = makeLinearTarget(*allocator, *device, 16, 16);
    ASSERT_NE(target.image, nullptr);

    const std::uint32_t halfRed = 0x80800000; // 50% opaque red, premultiplied
    auto tex = renderer->createTexture(1, 1, VK_FORMAT_B8G8R8A8_UNORM, false);
    const Rect full{0, 0, 1, 1};
    tex->write(&halfRed, 4, {&full, 1});

    const vk::TextureDraw draw{tex.get(), {0, 0, 16, 16}};
    ASSERT_TRUE(renderer->render(*target.image, {1, 1, 1, 1}, {&draw, 1}));
    ASSERT_TRUE(renderer->waitIdle());

    // red: 0.5 + 1 * 0.5 = 1.0; green/blue: 0 + 1 * 0.5 = 0.5
    EXPECT_PRED2(closeTo, readPixel(*target.buffer, 8, 8) & 0xffffff, 0xff8080u);
}

TEST_F(GpuFixture, OpaqueTextureIgnoresAlpha)
{
    auto target = makeLinearTarget(*allocator, *device, 16, 16);
    ASSERT_NE(target.image, nullptr);

    const std::uint32_t xrgbRed = 0x00ff0000; // XRGB: the X byte is garbage, often 0
    auto tex = renderer->createTexture(1, 1, VK_FORMAT_B8G8R8A8_UNORM, /*opaque=*/true);
    const Rect full{0, 0, 1, 1};
    tex->write(&xrgbRed, 4, {&full, 1});

    const vk::TextureDraw draw{tex.get(), {0, 0, 16, 16}};
    ASSERT_TRUE(renderer->render(*target.image, {0, 0, 1, 1}, {&draw, 1}));
    ASSERT_TRUE(renderer->waitIdle());

    EXPECT_EQ(readPixel(*target.buffer, 8, 8) & 0xffffff, 0xff0000u) << "blue background must not show through";
}

TEST_F(GpuFixture, PartialTextureUpdateKeepsTheRest)
{
    auto target = makeLinearTarget(*allocator, *device, 16, 16);
    ASSERT_NE(target.image, nullptr);
    auto tex = renderer->createTexture(2, 1, VK_FORMAT_B8G8R8A8_UNORM, false);

    const std::uint32_t red[] = {0xffff0000, 0xffff0000};
    const Rect full{0, 0, 2, 1};
    tex->write(red, 8, {&full, 1});
    const vk::TextureDraw draw{tex.get(), {0, 0, 2, 1}};
    ASSERT_TRUE(renderer->render(*target.image, {0, 0, 0, 1}, {&draw, 1}));

    const std::uint32_t green[] = {0, 0xff00ff00}; // only texel 1 is copied
    const Rect right{1, 0, 1, 1};
    tex->write(green, 8, {&right, 1}); // waits for the frame above to stop reading the staging buffer
    ASSERT_TRUE(renderer->render(*target.image, {0, 0, 0, 1}, {&draw, 1}));
    ASSERT_TRUE(renderer->waitIdle());

    EXPECT_EQ(readPixel(*target.buffer, 0, 0) & 0xffffff, 0xff0000u);
    EXPECT_EQ(readPixel(*target.buffer, 1, 0) & 0xffffff, 0x00ff00u);
}
