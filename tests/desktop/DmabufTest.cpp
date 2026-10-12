// linux-dmabuf end to end: the test plays a GPU client, allocating buffers
// with GBM on the same GPU and sharing them as dma-bufs, like Mesa does for
// kitty and every other GL/Vulkan app.

#include "DesktopFixture.h"

#include "linux-dmabuf-v1-client-protocol.h"

#include <set>

using namespace dragon;

namespace {

using Error = std::pair<std::string, std::uint32_t>;

class DmabufTest : public test::DesktopFixture {
protected:
    zwp_linux_dmabuf_v1* dmabuf = nullptr;

    void SetUp() override
    {
        DesktopFixture::SetUp();
        if (IsSkipped())
            return;
        static const wl_registry_listener listener = {
            .global =
                [](void* d, wl_registry* reg, std::uint32_t name, const char* iface, std::uint32_t) {
                    auto* self = static_cast<DmabufTest*>(d);
                    if (std::string_view(iface) == zwp_linux_dmabuf_v1_interface.name)
                        self->dmabuf = self->track(static_cast<zwp_linux_dmabuf_v1*>(
                            wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 4)));
                },
            .global_remove = [](void*, wl_registry*, std::uint32_t) {},
        };
        auto* registry = wl_display_get_registry(client);
        wl_registry_add_listener(registry, &listener, this);
        ASSERT_TRUE(roundtrip());
        wl_registry_destroy(registry);
        ASSERT_NE(dmabuf, nullptr) << "zwp_linux_dmabuf_v1 not advertised";
    }

    // A LINEAR XRGB8888 buffer filled with `pixel`, allocated like a client would.
    std::unique_ptr<GbmBuffer> allocate(std::uint32_t w, std::uint32_t h, std::uint32_t pixel)
    {
        const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
        auto bo = gpu->allocator().allocate(w, h, DRM_FORMAT_XRGB8888, {&linear, 1}, GBM_BO_USE_RENDERING);
        EXPECT_NE(bo, nullptr);
        std::uint32_t stride = 0;
        void* mapData = nullptr;
        auto* base = static_cast<std::uint8_t*>(
            gbm_bo_map(bo->bo(), 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &mapData));
        for (std::uint32_t y = 0; y < h; ++y)
            std::fill_n(reinterpret_cast<std::uint32_t*>(base + y * stride), w, pixel);
        gbm_bo_unmap(bo->bo(), mapData);
        return bo;
    }

    zwp_linux_buffer_params_v1* params(const GbmBuffer& bo)
    {
        auto* p = track(zwp_linux_dmabuf_v1_create_params(dmabuf));
        const auto& d = bo.dmabuf();
        for (std::uint32_t i = 0; i < d.planes.size(); ++i)
            zwp_linux_buffer_params_v1_add(p, d.planes[i].fd.get(), i, d.planes[i].offset, d.planes[i].stride,
                                           std::uint32_t(d.modifier >> 32), std::uint32_t(d.modifier));
        return p;
    }

    wl_buffer* importImmed(const GbmBuffer& bo)
    {
        const auto& d = bo.dmabuf();
        return track(zwp_linux_buffer_params_v1_create_immed(params(bo), std::int32_t(d.width),
                                                             std::int32_t(d.height), d.format, 0));
    }
};

struct Feedback {
    std::optional<dev_t> mainDevice;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> table;
    std::set<std::pair<std::uint32_t, std::uint64_t>> offered;
    bool done = false;
};

} // namespace

TEST_F(DmabufTest, FeedbackNamesOurGpuAndItsFormats)
{
    Feedback fb;
    static const zwp_linux_dmabuf_feedback_v1_listener listener = {
        .done = [](void* d, zwp_linux_dmabuf_feedback_v1*) { static_cast<Feedback*>(d)->done = true; },
        .format_table =
            [](void* d, zwp_linux_dmabuf_feedback_v1*, std::int32_t fd, std::uint32_t size) {
                auto* f = static_cast<Feedback*>(d);
                void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                ASSERT_NE(map, MAP_FAILED);
                struct Entry {
                    std::uint32_t format, pad;
                    std::uint64_t modifier;
                };
                const auto* e = static_cast<const Entry*>(map);
                for (std::size_t i = 0; i < size / sizeof(Entry); ++i)
                    f->table.emplace_back(e[i].format, e[i].modifier);
                munmap(map, size);
                close(fd);
            },
        .main_device =
            [](void* d, zwp_linux_dmabuf_feedback_v1*, wl_array* dev) {
                dev_t value;
                std::memcpy(&value, dev->data, sizeof(value));
                static_cast<Feedback*>(d)->mainDevice = value;
            },
        .tranche_done = [](void*, zwp_linux_dmabuf_feedback_v1*) {},
        .tranche_target_device = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {},
        .tranche_formats =
            [](void* d, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
                auto* f = static_cast<Feedback*>(d);
                const auto* idx = static_cast<const std::uint16_t*>(indices->data);
                for (std::size_t i = 0; i < indices->size / 2; ++i)
                    f->offered.insert(f->table.at(idx[i]));
            },
        .tranche_flags = [](void*, zwp_linux_dmabuf_feedback_v1*, std::uint32_t) {},
    };
    auto* feedback = track(zwp_linux_dmabuf_v1_get_default_feedback(dmabuf));
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &listener, &fb);
    ASSERT_TRUE(roundtrip());

    EXPECT_TRUE(fb.done);
    EXPECT_EQ(fb.mainDevice, gpu->device());
    EXPECT_TRUE(fb.offered.contains({DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR}));
    EXPECT_TRUE(fb.offered.contains({DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR}));
}

TEST_F(DmabufTest, DmabufWindowIsCompositedZeroCopy)
{
    auto bo = allocate(64, 48, 0xffff0000);
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    wl_surface_attach(w->surface, importImmed(*bo), 0, 0);
    wl_surface_damage_buffer(w->surface, 0, 0, 64, 48);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    const Point pos = desktop->windows().at(0)->position;
    EXPECT_EQ(pixel(pos.x + 1, pos.y + 1), 0xff0000u);
    EXPECT_EQ(pixel(pos.x + 63, pos.y + 47), 0xff0000u);
    EXPECT_EQ(pixel(pos.x + 64, pos.y + 47), background());

    // Zero-copy: new content in the same buffer shows up without a new import.
    auto* base = static_cast<std::uint8_t*>(nullptr);
    std::uint32_t stride = 0;
    void* mapData = nullptr;
    base = static_cast<std::uint8_t*>(gbm_bo_map(bo->bo(), 0, 0, 64, 48, GBM_BO_TRANSFER_WRITE, &stride, &mapData));
    for (std::uint32_t y = 0; y < 48; ++y)
        std::fill_n(reinterpret_cast<std::uint32_t*>(base + y * stride), 64, 0xff00ff00u);
    gbm_bo_unmap(bo->bo(), mapData);
    wl_surface_damage_buffer(w->surface, 0, 0, 64, 48);
    wl_surface_commit(w->surface);
    pumpFrames(2);
    EXPECT_EQ(pixel(pos.x + 1, pos.y + 1), 0x00ff00u);
}

TEST_F(DmabufTest, AsyncCreateReportsCreated)
{
    auto bo = allocate(16, 16, 0xff0000ff);
    struct Result {
        wl_buffer* buffer = nullptr;
        bool failed = false;
    } result;
    static const zwp_linux_buffer_params_v1_listener listener = {
        .created = [](void* d, zwp_linux_buffer_params_v1*, wl_buffer* b) { static_cast<Result*>(d)->buffer = b; },
        .failed = [](void* d, zwp_linux_buffer_params_v1*) { static_cast<Result*>(d)->failed = true; },
    };
    auto* p = params(*bo);
    zwp_linux_buffer_params_v1_add_listener(p, &listener, &result);
    zwp_linux_buffer_params_v1_create(p, 16, 16, DRM_FORMAT_XRGB8888, 0);
    ASSERT_TRUE(roundtrip());

    EXPECT_FALSE(result.failed);
    ASSERT_NE(result.buffer, nullptr);
    track(result.buffer);
}

TEST_F(DmabufTest, UnadvertisedModifierFailsWithoutKillingTheClient)
{
    auto bo = allocate(16, 16, 0xff0000ff);
    bool failed = false;
    static const zwp_linux_buffer_params_v1_listener listener = {
        .created = [](void*, zwp_linux_buffer_params_v1*, wl_buffer*) {},
        .failed = [](void* d, zwp_linux_buffer_params_v1*) { *static_cast<bool*>(d) = true; },
    };
    auto* p = track(zwp_linux_dmabuf_v1_create_params(dmabuf));
    const auto& d = bo->dmabuf();
    zwp_linux_buffer_params_v1_add(p, d.planes[0].fd.get(), 0, d.planes[0].offset, d.planes[0].stride, 0x00abcdef,
                                   0x12345678); // nonsense modifier
    zwp_linux_buffer_params_v1_add_listener(p, &listener, &failed);
    zwp_linux_buffer_params_v1_create(p, 16, 16, DRM_FORMAT_XRGB8888, 0);

    EXPECT_TRUE(roundtrip()) << "an import failure isn't a protocol error";
    EXPECT_TRUE(failed);
}

TEST_F(DmabufTest, PlaneSetTwiceIsAnError)
{
    auto bo = allocate(16, 16, 0xff0000ff);
    auto* p = params(*bo);
    const auto& d = bo->dmabuf();
    zwp_linux_buffer_params_v1_add(p, d.planes[0].fd.get(), 0, 0, d.planes[0].stride, 0, 0);

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (Error{"zwp_linux_buffer_params_v1", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET}));
}

TEST_F(DmabufTest, CreateWithoutPlanesIsIncomplete)
{
    auto* p = track(zwp_linux_dmabuf_v1_create_params(dmabuf));
    track(zwp_linux_buffer_params_v1_create_immed(p, 16, 16, DRM_FORMAT_XRGB8888, 0));

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(), (Error{"zwp_linux_buffer_params_v1", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE}));
}

TEST_F(DmabufTest, UnknownFormatIsAnError)
{
    auto bo = allocate(16, 16, 0xff0000ff);
    track(zwp_linux_buffer_params_v1_create_immed(params(*bo), 16, 16, 0x20202020, 0));

    EXPECT_FALSE(roundtrip());
    EXPECT_EQ(protocolError(),
              (Error{"zwp_linux_buffer_params_v1", ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT}));
}

TEST_F(DmabufTest, DestroyingBufferWhileShownKeepsLastFrame)
{
    auto bo = allocate(32, 32, 0xffff0000);
    auto w = makeWindow();
    ASSERT_TRUE(roundtrip());
    xdg_surface_ack_configure(w->xdg, w->lastSerial);
    auto* buffer = importImmed(*bo);
    wl_surface_attach(w->surface, buffer, 0, 0);
    wl_surface_commit(w->surface);
    pumpFrames(2);

    wl_buffer_destroy(buffer); // allowed: the compositor keeps what it needs
    forget(buffer);
    pumpFrames(2);

    const Point pos = desktop->windows().at(0)->position;
    EXPECT_EQ(pixel(pos.x + 1, pos.y + 1), 0xff0000u);
}
