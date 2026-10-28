#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "desktop/Desktop.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace dragon {

class Backend;
class Seat;
class Surface;
struct PointerAxisEvent;

struct WindowManagerConfig {
    // XKB modifier for compositor shortcuts: "Mod4" (Super), "Mod1" (Alt), "Control".
    std::string modifier = "Mod4";
    std::string modifierLabel = "Super"; // for logs
    std::string terminal = "kitty";
};

struct WindowManagerActions {
    std::function<void()> quit;
    std::function<void(const std::string& command)> spawn;
};

// Turns a backend's raw input into seat events and window management:
//  - finds the surface under the pointer; click to focus and raise
//  - keeps pointer focus on the pressed surface while a button is held
//    (the implicit grab, so drags continue outside the window)
//  - compositor shortcuts: Mod+Enter terminal, Mod+Q close, Mod+Shift+Esc quit
//  - interactive move/resize: Mod+drag, Mod+right-drag, or a client's own
//    request (title bar drags); input is withheld from clients meanwhile
class InputRouter {
public:
    InputRouter(Backend& backend, Desktop& desktop, Seat& seat, WindowManagerConfig config = {},
                WindowManagerActions actions = {});
    ~InputRouter();
    InputRouter(const InputRouter&) = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    PointF pointerPosition() const { return m_pointer; }
    bool pointerInside() const { return m_pointerInside; }
    bool grabbing() const { return m_grab.kind != GrabKind::None; }

private:
    enum class GrabKind { None, Move, Resize };
    struct Grab {
        GrabKind kind = GrabKind::None;
        Desktop::Window* window = nullptr;
        std::uint32_t button = 0;
        PointF startPointer;
        Point startPosition;
        Rect startGeometry;
        std::uint32_t edges = 0;
    };

    void motion(PointF global, std::uint32_t timeMs);
    void button(std::uint32_t button, bool pressed, std::uint32_t timeMs);
    void key(std::uint32_t key, bool pressed, std::uint32_t timeMs);
    bool runShortcut(std::uint32_t key);
    // Re-picks the surface under the pointer (after motion, release, stacking changes).
    void updatePointerFocus(std::uint32_t timeMs, bool sendMotion);

    void startGrab(GrabKind kind, Desktop::Window& window, std::uint32_t button, std::uint32_t edges);
    void updateGrab();
    void endGrab(std::uint32_t timeMs);
    // A client asked for move/resize: only honored for the button press it's reacting to.
    bool clientGrabAllowed(std::uint32_t serial) const;

    Desktop& m_desktop;
    Seat& m_seat;
    WindowManagerConfig m_config;
    WindowManagerActions m_actions;

    PointF m_pointer;
    bool m_pointerInside = false;
    std::uint32_t m_lastClientButton = 0;
    Grab m_grab;
    std::set<std::uint32_t> m_swallowedKeys;    // shortcut keys: their releases aren't sent either
    std::set<std::uint32_t> m_swallowedButtons; // Mod+click presses: same for releases
    std::vector<Connection> m_connections;
};

} // namespace dragon
