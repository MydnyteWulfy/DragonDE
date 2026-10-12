#pragma once

#include "backend/Backend.h"
#include "core/EventLoop.h"
#include "core/Signal.h"

#include <memory>

namespace dragon {

// Runs DragonDE as a window inside another Wayland compositor (KDE during
// development). Each output is an xdg_toplevel; frames are GBM buffers handed
// to the parent through linux-dmabuf, so this exercises the same render path
// as real hardware.
class NestedBackend final : public Backend {
public:
    // Connects to $WAYLAND_DISPLAY. nullptr if there's no parent compositor or
    // it lacks wl_compositor v4 / xdg_wm_base / linux-dmabuf v3.
    static std::unique_ptr<NestedBackend> connect(EventLoop& loop);
    ~NestedBackend() override;

    const char* name() const override { return "nested"; }
    std::optional<dev_t> preferredDevice() const override;
    bool start(Gpu& gpu) override;

    struct Parent; // connection state, defined in the .cpp

private:
    explicit NestedBackend(EventLoop& loop);
    void handleParentEvents(std::uint32_t mask);

    EventLoop& m_loop;
    std::unique_ptr<Parent> m_parent;
    EventSource m_parentSource;
    Connection m_flush;
};

} // namespace dragon
