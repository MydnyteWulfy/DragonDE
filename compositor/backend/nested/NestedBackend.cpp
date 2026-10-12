#include "backend/nested/NestedBackend.h"

#include "core/Log.h"
#include "render/Gpu.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <drm_fourcc.h>
#include <gbm.h>
#include <map>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h> // WL_EVENT_* for our own event loop

#include "linux-dmabuf-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace dragon {

// ------------------------------------------------------------ parent state --

struct NestedBackend::Parent {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wmBase = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    std::uint32_t dmabufVersion = 0;
    zxdg_decoration_manager_v1* decorationManager = nullptr;

    // What the parent can import: DRM format -> modifiers.
    std::map<std::uint32_t, std::vector<std::uint64_t>> formats;
    std::optional<dev_t> mainDevice;

    // linux-dmabuf feedback parsing state.
    struct TableEntry {
        std::uint32_t format;
        std::uint32_t padding;
        std::uint64_t modifier;
    };
    std::vector<TableEntry> formatTable;
    bool feedbackDone = false;

    void addFormat(std::uint32_t format, std::uint64_t modifier)
    {
        auto& mods = formats[format];
        if (std::ranges::find(mods, modifier) == mods.end())
            mods.push_back(modifier);
    }

    ~Parent()
    {
        if (decorationManager)
            zxdg_decoration_manager_v1_destroy(decorationManager);
        if (dmabuf)
            zwp_linux_dmabuf_v1_destroy(dmabuf);
        if (wmBase)
            xdg_wm_base_destroy(wmBase);
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
    }
};

namespace {

using Parent = NestedBackend::Parent;

// ------------------------------------------------------------- listeners --

const xdg_wm_base_listener kWmBaseListener = {
    .ping = [](void*, xdg_wm_base* base, std::uint32_t serial) { xdg_wm_base_pong(base, serial); },
};

// linux-dmabuf v3: formats are announced right after bind.
const zwp_linux_dmabuf_v1_listener kDmabufListener = {
    .format = [](void*, zwp_linux_dmabuf_v1*, std::uint32_t) {},
    .modifier = [](void* data, zwp_linux_dmabuf_v1*, std::uint32_t format, std::uint32_t hi, std::uint32_t lo) {
        static_cast<Parent*>(data)->addFormat(format, (std::uint64_t(hi) << 32) | lo);
    },
};

// linux-dmabuf v4+: a shared-memory format table plus tranches of indices into it.
const zwp_linux_dmabuf_feedback_v1_listener kFeedbackListener = {
    .done = [](void* data, zwp_linux_dmabuf_feedback_v1*) { static_cast<Parent*>(data)->feedbackDone = true; },
    .format_table =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, std::int32_t fd, std::uint32_t size) {
            auto* p = static_cast<Parent*>(data);
            UniqueFd owned(fd);
            void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map == MAP_FAILED) {
                log::error("backend", "nested: cannot map dmabuf format table: {}", std::strerror(errno));
                return;
            }
            const auto* entries = static_cast<const Parent::TableEntry*>(map);
            p->formatTable.assign(entries, entries + size / sizeof(Parent::TableEntry));
            munmap(map, size);
        },
    .main_device =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* device) {
            dev_t dev;
            if (device->size == sizeof(dev)) {
                std::memcpy(&dev, device->data, sizeof(dev));
                static_cast<Parent*>(data)->mainDevice = dev;
            }
        },
    .tranche_done = [](void*, zwp_linux_dmabuf_feedback_v1*) {},
    .tranche_target_device = [](void*, zwp_linux_dmabuf_feedback_v1*, wl_array*) {},
    .tranche_formats =
        [](void* data, zwp_linux_dmabuf_feedback_v1*, wl_array* indices) {
            auto* p = static_cast<Parent*>(data);
            const auto* idx = static_cast<const std::uint16_t*>(indices->data);
            for (std::size_t i = 0; i < indices->size / sizeof(std::uint16_t); ++i) {
                if (idx[i] < p->formatTable.size())
                    p->addFormat(p->formatTable[idx[i]].format, p->formatTable[idx[i]].modifier);
            }
        },
    .tranche_flags = [](void*, zwp_linux_dmabuf_feedback_v1*, std::uint32_t) {},
};

const wl_registry_listener kRegistryListener = {
    .global =
        [](void* data, wl_registry* registry, std::uint32_t name, const char* iface, std::uint32_t version) {
            auto* p = static_cast<Parent*>(data);
            const std::string_view i(iface);
            auto bind = [&](const wl_interface* interface, std::uint32_t maxVersion) {
                return wl_registry_bind(registry, name, interface, std::min(version, maxVersion));
            };
            if (i == wl_compositor_interface.name && version >= 4) {
                p->compositor = static_cast<wl_compositor*>(bind(&wl_compositor_interface, 4));
            } else if (i == xdg_wm_base_interface.name) {
                p->wmBase = static_cast<xdg_wm_base*>(bind(&xdg_wm_base_interface, 6));
                xdg_wm_base_add_listener(p->wmBase, &kWmBaseListener, p);
            } else if (i == zwp_linux_dmabuf_v1_interface.name && version >= 3) {
                p->dmabufVersion = std::min(version, 4u);
                p->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(bind(&zwp_linux_dmabuf_v1_interface, 4));
                zwp_linux_dmabuf_v1_add_listener(p->dmabuf, &kDmabufListener, p);
            } else if (i == zxdg_decoration_manager_v1_interface.name) {
                p->decorationManager =
                    static_cast<zxdg_decoration_manager_v1*>(bind(&zxdg_decoration_manager_v1_interface, 1));
            }
        },
    .global_remove = [](void*, wl_registry*, std::uint32_t) {},
};

// ---------------------------------------------------------------- output --

class NestedOutput final : public Output {
public:
    NestedOutput(NestedBackend& backend, Parent& parent, Gpu& gpu, std::string name, std::uint32_t format,
                 std::vector<std::uint64_t> modifiers)
        : Output(std::move(name)), m_backend(backend), m_parent(parent), m_gpu(gpu), m_format(format),
          m_modifiers(std::move(modifiers))
    {
    }

    ~NestedOutput() override
    {
        m_gpu.renderer().waitIdle(); // swapchain images must be idle before they're destroyed
        if (m_frameCallback)
            wl_callback_destroy(m_frameCallback);
        destroyBuffers();
        if (m_decoration)
            zxdg_toplevel_decoration_v1_destroy(m_decoration);
        if (m_toplevel)
            xdg_toplevel_destroy(m_toplevel);
        if (m_xdgSurface)
            xdg_surface_destroy(m_xdgSurface);
        if (m_surface)
            wl_surface_destroy(m_surface);
    }

    // Creates the window. Rendering starts after the parent's first configure.
    void init()
    {
        m_surface = wl_compositor_create_surface(m_parent.compositor);
        m_xdgSurface = xdg_wm_base_get_xdg_surface(m_parent.wmBase, m_surface);
        xdg_surface_add_listener(m_xdgSurface, &kXdgSurfaceListener, this);
        m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
        xdg_toplevel_add_listener(m_toplevel, &kToplevelListener, this);

        const auto title = "DragonDE — " + name();
        xdg_toplevel_set_title(m_toplevel, title.c_str());
        xdg_toplevel_set_app_id(m_toplevel, "dragonde-nested");
        xdg_toplevel_set_min_size(m_toplevel, 320, 240);

        if (m_parent.decorationManager) {
            m_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(m_parent.decorationManager, m_toplevel);
            zxdg_toplevel_decoration_v1_set_mode(m_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        wl_surface_commit(m_surface); // no buffer yet: asks for the first configure
    }

    std::string description() const override { return "DragonDE nested window " + name(); }

    void present(SwapchainSlot& slot) override
    {
        wl_surface_attach(m_surface, m_buffers[slot.index], 0, 0);
        wl_surface_damage_buffer(m_surface, 0, 0, INT32_MAX, INT32_MAX);
        if (!m_frameCallback) {
            m_frameCallback = wl_surface_frame(m_surface);
            wl_callback_add_listener(m_frameCallback, &kFrameListener, this);
        }
        wl_surface_commit(m_surface);

        if (m_framesPresented++ == 0)
            log::info("backend", "nested: first frame on {} ({}x{}, modifier {:#018x})", name(),
                      m_swapchain->width(), m_swapchain->height(), slot.buffer->dmabuf().modifier);
    }

private:
    static constexpr std::int32_t kDefaultWidth = 1280;
    static constexpr std::int32_t kDefaultHeight = 720;

    void handleConfigure(std::uint32_t serial)
    {
        xdg_surface_ack_configure(m_xdgSurface, serial);

        // 0 means "you pick": keep the current size, or use the default.
        const auto curW = m_swapchain ? std::int32_t(m_swapchain->width()) : kDefaultWidth;
        const auto curH = m_swapchain ? std::int32_t(m_swapchain->height()) : kDefaultHeight;
        const auto w = m_pendingWidth > 0 ? m_pendingWidth : curW;
        const auto h = m_pendingHeight > 0 ? m_pendingHeight : curH;

        const bool resized = !m_swapchain || std::uint32_t(w) != m_swapchain->width() ||
                             std::uint32_t(h) != m_swapchain->height();
        if (resized && !recreateSwapchain(std::uint32_t(w), std::uint32_t(h))) {
            m_backend.quitRequested.emit();
            return;
        }

        // Draw right away for the first configure and after a resize, unless a
        // frame callback is already on its way.
        if (resized && !m_frameCallback)
            frame.emit();
    }

    bool recreateSwapchain(std::uint32_t width, std::uint32_t height)
    {
        m_gpu.renderer().waitIdle();
        destroyBuffers();
        m_swapchain = Swapchain::create(m_gpu, width, height, m_format, m_modifiers, GBM_BO_USE_RENDERING);
        if (!m_swapchain) {
            log::error("backend", "nested: cannot create {}x{} swapchain", width, height);
            return false;
        }

        for (auto& slot : m_swapchain->slots()) {
            const auto& buf = slot.buffer->dmabuf();
            auto* params = zwp_linux_dmabuf_v1_create_params(m_parent.dmabuf);
            for (std::uint32_t i = 0; i < buf.planes.size(); ++i) {
                const auto& plane = buf.planes[i];
                // libwayland duplicates the fd when sending; we keep ownership.
                zwp_linux_buffer_params_v1_add(params, plane.fd.get(), i, plane.offset, plane.stride,
                                               std::uint32_t(buf.modifier >> 32),
                                               std::uint32_t(buf.modifier & 0xffffffff));
            }
            auto* wlBuffer = zwp_linux_buffer_params_v1_create_immed(params, std::int32_t(width),
                                                                     std::int32_t(height), buf.format, 0);
            zwp_linux_buffer_params_v1_destroy(params);
            wl_buffer_add_listener(wlBuffer, &kBufferListener, &slot);
            m_buffers.push_back(wlBuffer);
        }
        log::debug("backend", "nested: {} resized to {}x{}", name(), width, height);
        // We can't know the parent's refresh rate here; frame callbacks pace us anyway.
        setMode({std::int32_t(width), std::int32_t(height), 60000});
        return true;
    }

    void destroyBuffers()
    {
        for (auto* b : m_buffers)
            wl_buffer_destroy(b);
        m_buffers.clear();
    }

    static const xdg_surface_listener kXdgSurfaceListener;
    static const xdg_toplevel_listener kToplevelListener;
    static const wl_callback_listener kFrameListener;
    static const wl_buffer_listener kBufferListener;

    NestedBackend& m_backend;
    Parent& m_parent;
    Gpu& m_gpu;
    std::uint32_t m_format;
    std::vector<std::uint64_t> m_modifiers;

    wl_surface* m_surface = nullptr;
    xdg_surface* m_xdgSurface = nullptr;
    xdg_toplevel* m_toplevel = nullptr;
    zxdg_toplevel_decoration_v1* m_decoration = nullptr;
    wl_callback* m_frameCallback = nullptr;
    std::vector<wl_buffer*> m_buffers; // parallel to m_swapchain->slots()

    std::int32_t m_pendingWidth = 0;
    std::int32_t m_pendingHeight = 0;
};

const xdg_surface_listener NestedOutput::kXdgSurfaceListener = {
    .configure = [](void* data, xdg_surface*, std::uint32_t serial) {
        static_cast<NestedOutput*>(data)->handleConfigure(serial);
    },
};

const xdg_toplevel_listener NestedOutput::kToplevelListener = {
    .configure =
        [](void* data, xdg_toplevel*, std::int32_t w, std::int32_t h, wl_array*) {
            auto* self = static_cast<NestedOutput*>(data);
            self->m_pendingWidth = w;
            self->m_pendingHeight = h;
        },
    .close =
        [](void* data, xdg_toplevel*) {
            log::info("backend", "nested: window closed");
            static_cast<NestedOutput*>(data)->m_backend.quitRequested.emit();
        },
    .configure_bounds = [](void*, xdg_toplevel*, std::int32_t, std::int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

const wl_callback_listener NestedOutput::kFrameListener = {
    .done = [](void* data, wl_callback* cb, std::uint32_t) {
        auto* self = static_cast<NestedOutput*>(data);
        wl_callback_destroy(cb);
        self->m_frameCallback = nullptr;
        self->frame.emit();
    },
};

const wl_buffer_listener NestedOutput::kBufferListener = {
    .release = [](void* data, wl_buffer*) { Swapchain::release(*static_cast<SwapchainSlot*>(data)); },
};

} // namespace

// --------------------------------------------------------------- backend --

NestedBackend::NestedBackend(EventLoop& loop) : m_loop(loop) {}

std::unique_ptr<NestedBackend> NestedBackend::connect(EventLoop& loop)
{
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        log::debug("backend", "nested: no parent Wayland compositor");
        return nullptr;
    }

    std::unique_ptr<NestedBackend> backend(new NestedBackend(loop));
    backend->m_parent = std::make_unique<Parent>();
    Parent& p = *backend->m_parent;
    p.display = display;

    p.registry = wl_display_get_registry(display);
    wl_registry_add_listener(p.registry, &kRegistryListener, &p);
    wl_display_roundtrip(display);

    if (!p.compositor || !p.wmBase || !p.dmabuf) {
        log::error("backend", "nested: parent lacks{}{}{}", p.compositor ? "" : " wl_compositor v4",
                   p.wmBase ? "" : " xdg_wm_base", p.dmabuf ? "" : " linux-dmabuf v3");
        return nullptr;
    }

    if (p.dmabufVersion >= 4) {
        auto* feedback = zwp_linux_dmabuf_v1_get_default_feedback(p.dmabuf);
        zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &kFeedbackListener, &p);
        for (int i = 0; i < 5 && !p.feedbackDone; ++i)
            wl_display_roundtrip(display);
        zwp_linux_dmabuf_feedback_v1_destroy(feedback);
    } else {
        wl_display_roundtrip(display); // v3 modifier events follow the bind
    }

    if (p.mainDevice)
        log::info("backend", "nested: parent renders on DRM device {}:{}, accepts {} formats",
                  major(*p.mainDevice), minor(*p.mainDevice), p.formats.size());
    else
        log::info("backend", "nested: parent accepts {} formats (no main device advertised)", p.formats.size());

    auto* raw = backend.get();
    backend->m_parentSource = loop.addFd(wl_display_get_fd(display), WL_EVENT_READABLE,
                                         [raw](int, std::uint32_t mask) { raw->handleParentEvents(mask); });
    backend->m_flush = loop.beforeSleep.connect([raw] {
        wl_display_dispatch_pending(raw->m_parent->display);
        wl_display_flush(raw->m_parent->display);
    });
    return backend;
}

NestedBackend::~NestedBackend()
{
    // Outputs hold parent objects: destroy them while the connection is alive.
    clearOutputs();
    m_flush.disconnect();
    m_parentSource = {};
}

std::optional<dev_t> NestedBackend::preferredDevice() const
{
    return m_parent->mainDevice;
}

bool NestedBackend::start(Gpu& gpu)
{
    // First format both sides support; XRGB first since we don't need alpha.
    for (std::uint32_t format : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}) {
        auto it = m_parent->formats.find(format);
        if (it == m_parent->formats.end())
            continue;

        std::vector<std::uint64_t> common;
        for (auto mod : gpu.renderModifiers(format)) {
            if (std::ranges::find(it->second, mod) != it->second.end())
                common.push_back(mod);
        }
        if (common.empty())
            continue;

        auto output = std::make_unique<NestedOutput>(*this, *m_parent, gpu, "NESTED-1", format, std::move(common));
        output->init();
        addOutput(std::move(output));
        return true;
    }

    log::error("backend", "nested: no format/modifier both we and the parent compositor support");
    return false;
}

void NestedBackend::handleParentEvents(std::uint32_t mask)
{
    auto fail = [this](std::string_view why) {
        log::error("backend", "nested: lost connection to parent compositor: {}", why);
        m_parentSource.setMask(0); // stop polling a dead fd (can't remove the source from its own callback)
        quitRequested.emit();
    };

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        fail("hangup");
        return;
    }
    if (wl_display_dispatch(m_parent->display) < 0)
        fail(std::strerror(wl_display_get_error(m_parent->display)));
}

} // namespace dragon
