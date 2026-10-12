#pragma once

#include <cstdint>
#include <functional>
#include <memory>

struct wl_client;
struct wl_display;
struct wl_global;
struct wl_interface;

namespace dragon {

// RAII wrapper for a wl_global: an object clients discover through wl_registry
// and bind to (wl_compositor, xdg_wm_base, ...).
class Global {
public:
    // Called when a client binds. `version` is already clamped to what we advertised.
    using BindFn = std::function<void(wl_client* client, std::uint32_t version, std::uint32_t id)>;

    Global(wl_display* display, const wl_interface* interface, std::uint32_t version, BindFn bind);
    ~Global();
    Global(const Global&) = delete;
    Global& operator=(const Global&) = delete;

    // For a generated class T: advertise T at `version` (clamped to T::maxVersion).
    // Heap-allocated because libwayland holds a pointer to the Global.
    template <typename T>
    static std::unique_ptr<Global> create(wl_display* display, std::uint32_t version, BindFn bind)
    {
        return std::make_unique<Global>(display, T::interface(), checkVersion(version, T::maxVersion),
                                        std::move(bind));
    }

    wl_global* handle() const { return m_global; }

private:
    static std::uint32_t checkVersion(std::uint32_t requested, std::uint32_t max);
    static void handleBind(wl_client* client, void* data, std::uint32_t version, std::uint32_t id);

    wl_global* m_global = nullptr;
    BindFn m_bind;
};

} // namespace dragon
