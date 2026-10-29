#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "dragon-protocols/wlr-layer-shell-unstable-v1.h"
#include "protocols/Compositor.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dragon {

class Global;
class Server;

// wlr-layer-shell: surfaces that are part of the desktop rather than windows
// (bars, launchers, notifications, wallpapers, lock screens). The client
// anchors them to screen edges and the compositor places them; they live in
// one of four layers around the normal windows.
class LayerSurface final : public proto::ZwlrLayerSurfaceV1, public SurfaceRole {
public:
    using Layer = proto::ZwlrLayerShellV1Layer;
    using Keyboard = proto::ZwlrLayerSurfaceV1KeyboardInteractivity;
    static constexpr std::uint32_t kTop = 1, kBottom = 2, kLeft = 4, kRight = 8; // anchor bits

    // Double-buffered: applied on wl_surface.commit.
    struct State {
        Size size;                // 0 on an axis: stretch between the anchored edges
        std::uint32_t anchor = 0; // kTop | ...
        std::int32_t exclusiveZone = 0; // > 0: reserve this much; -1: ignore others' zones
        std::uint32_t exclusiveEdge = 0; // v5: which edge the zone is on (0: from the anchors)
        std::int32_t marginTop = 0, marginRight = 0, marginBottom = 0, marginLeft = 0;
        Keyboard keyboard = Keyboard::None;
        Layer layer = Layer::Background;
        friend bool operator==(const State&, const State&) = default;
    };

    LayerSurface(wl_client* client, std::uint32_t version, std::uint32_t id, Surface* surface, Layer layer,
                 std::string nameSpace);
    ~LayerSurface() override;

    static LayerSurface* from(Surface* surface) { return dynamic_cast<LayerSurface*>(surface ? surface->role() : nullptr); }

    Surface* surface() const { return m_surface; } // null once the wl_surface is gone
    const State& state() const { return m_current; }
    const std::string& nameSpace() const { return m_nameSpace; }
    // The edge the exclusive zone is on (0: none).
    std::uint32_t exclusiveEdge() const;

    bool initialized() const { return m_initialCommitDone; } // made its initial commit: needs a configure
    bool isMapped() const { return m_mapped; }

    // Tells the client its size; only sent when it differs from the last one.
    void configure(Size size);
    // The surface can't be shown (no room, output gone): the client should destroy it.
    void close();

    Signal<> mapped;
    Signal<> unmapped;
    Signal<> stateChanged; // a commit changed the state (or made the initial commit)

    // SurfaceRole
    bool precommit(const SurfaceState& pending) override;
    void commit() override;

protected:
    void handleSetSize(std::uint32_t width, std::uint32_t height) override;
    void handleSetAnchor(proto::ZwlrLayerSurfaceV1Anchor anchor) override;
    void handleSetExclusiveZone(std::int32_t zone) override;
    void handleSetMargin(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left) override;
    void handleSetKeyboardInteractivity(Keyboard keyboard) override;
    void handleGetPopup(wl_resource* popup) override;
    void handleAckConfigure(std::uint32_t serial) override;
    void handleSetLayer(Layer layer) override;
    void handleSetExclusiveEdge(proto::ZwlrLayerSurfaceV1Anchor edge) override;

private:
    void unmap();

    Surface* m_surface;
    std::string m_nameSpace;
    State m_pending;
    State m_current;
    bool m_initialCommitDone = false;
    bool m_configured = false; // a configure was acked
    bool m_mapped = false;
    bool m_closed = false;
    std::optional<Size> m_lastSent;
    std::vector<std::uint32_t> m_sentSerials;
    Connection m_surfaceDying;
};

class LayerShell {
public:
    explicit LayerShell(Server& server);
    ~LayerShell();
    LayerShell(const LayerShell&) = delete;
    LayerShell& operator=(const LayerShell&) = delete;

    Signal<LayerSurface*> surfaceCreated;

private:
    std::unique_ptr<Global> m_global;
};

} // namespace dragon
