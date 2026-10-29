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
class Config;
class Seat;
class Surface;
struct ConfigActions;
struct PointerAxisEvent;

// The window actions shortcuts run (close, minimize, maximize...) on the
// focused window. Spawn, quit and reload are left for the caller.
void addWindowActions(ConfigActions& actions, Desktop& desktop);

// Turns a backend's raw input into seat events and window management:
//  - finds the surface under the pointer; click to focus and raise
//  - keeps pointer focus on the pressed surface while a button is held
//    (the implicit grab, so drags continue outside the window)
//  - compositor shortcuts, from the config (keyboard settings too)
//  - interactive move/resize: Mod+drag, Mod+right-drag, or a client's own
//    request (title bar drags); input is withheld from clients meanwhile
class InputRouter {
public:
    InputRouter(Backend& backend, Desktop& desktop, Seat& seat, Config& config);
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
    bool modifierHeld() const; // the configured Mod
    void applyKeyboardSettings();
    // Re-picks the surface under the pointer (after motion, release, stacking changes).
    void updatePointerFocus(std::uint32_t timeMs, bool sendMotion);

    // Our window decorations: title bars, their buttons, the resize edges.
    bool decorationPress(std::uint32_t button, std::uint32_t timeMs);
    bool decorationRelease(std::uint32_t button);
    void setHover(Desktop::Window* window, int button);

    void startGrab(GrabKind kind, Desktop::Window& window, std::uint32_t button, std::uint32_t edges);
    void updateGrab();
    void endGrab(std::uint32_t timeMs);
    // A client asked for move/resize: only honored for the button press it's reacting to.
    bool clientGrabAllowed(Desktop::Window& window, std::optional<std::uint32_t> serial) const;

    Desktop& m_desktop;
    Seat& m_seat;
    Config& m_config;
    std::string m_backendKeymap; // the parent's keymap (nested), used unless the config names one
    std::string m_keymapSource;  // what the seat's keymap was last built from

    PointF m_pointer;
    bool m_pointerInside = false;
    std::uint32_t m_lastClientButton = 0;
    Grab m_grab;
    // Title bar pressed: becomes a move once the pointer travels a little
    // (a click alone mustn't un-maximize).
    struct PendingMove {
        Desktop::Window* window = nullptr;
        std::uint32_t button = 0;
        PointF start;
    } m_pendingMove;
    // Title bar button held: runs if released over the same button.
    struct ButtonPress {
        Desktop::Window* window = nullptr;
        int index = -1;
        std::string action;
        std::uint32_t button = 0;
    } m_buttonPress;
    Desktop::Window* m_hoverWindow = nullptr;
    Desktop::Window* m_lastTitleClick = nullptr; // double-click detection
    std::uint32_t m_lastTitleClickTime = 0;
    std::set<std::uint32_t> m_swallowedKeys;    // shortcut keys: their releases aren't sent either
    std::set<std::uint32_t> m_swallowedButtons; // Mod+click presses: same for releases
    std::vector<Connection> m_connections;
};

} // namespace dragon
