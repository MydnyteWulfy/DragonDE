#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "core/UniqueFd.h"
#include "dragon-protocols/wayland.h"
#include "wayland/Global.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace dragon {

class DataSource;
class Server;
class Surface;
struct PointerAxisEvent;

// The wl_seat: one keyboard and one pointer, shared by all clients. Input
// arrives already routed (the desktop decides which surface is under the
// pointer and which has keyboard focus); the seat speaks the protocol:
// enter/leave, keys, modifiers, motion, buttons, scrolling, and hands the
// clipboard selection to whoever has keyboard focus.
class Seat {
public:
    Seat(Server& server, std::string name = "seat0");
    ~Seat();
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;

    // ------------------------------------------------------------ keyboard --
    // XKB text keymap (e.g. the nested parent's). Starts with the XKB defaults
    // ($XKB_DEFAULT_LAYOUT etc., else US).
    bool setKeymap(const std::string& keymap);
    void setKeyboardFocus(Surface* surface);
    Surface* keyboardFocus() const { return m_keyboardFocus; }
    // evdev keycode; updates modifier state and tells the focused client.
    void notifyKey(std::uint32_t timeMs, std::uint32_t key, bool pressed);
    // Super/Alt/Ctrl/Shift currently held, as XKB effective modifier names.
    bool modifierActive(const char* name) const;

    // ------------------------------------------------------------- pointer --
    // Moves pointer focus (enter/leave). Null: the pointer is over nothing.
    void notifyPointerFocus(Surface* surface, PointF local);
    Surface* pointerFocus() const { return m_pointerFocus; }
    void notifyPointerMotion(std::uint32_t timeMs, PointF local);
    void notifyButton(std::uint32_t timeMs, std::uint32_t button, bool pressed);
    void notifyAxis(const PointerAxisEvent& event);
    void notifyFrame();
    bool buttonsPressed() const { return !m_pressedButtons.empty(); }
    // Serial of the latest button press: move/resize/selection requests must quote it.
    std::uint32_t lastButtonSerial() const { return m_lastButtonSerial; }

    // ----------------------------------------------------------- selection --
    void setSelection(DataSource* source);
    DataSource* selection() const { return m_selection; }

    // --- protocol object registration (resources call these) ---
    void addPointer(proto::WlPointer* pointer);
    void addKeyboard(proto::WlKeyboard* keyboard);
    void addDataDevice(proto::WlDataDevice* device);

private:
    template <typename T>
    struct Tracked {
        std::vector<T*> objects;
        std::vector<Connection> connections;
        void add(T* obj);
        std::vector<T*> forClient(wl_client* client) const;
    };

    void sendKeymap(proto::WlKeyboard& keyboard);
    void sendModifiers(wl_client* client, std::uint32_t serial);
    void sendKeyboardEnter(proto::WlKeyboard& keyboard, std::uint32_t serial);
    void sendSelection(wl_client* client);
    void applyKeymap(xkb_keymap* keymap);
    std::uint32_t nextSerial();

    Server& m_server;
    std::string m_name;
    std::unique_ptr<Global> m_global;

    xkb_context* m_xkbContext = nullptr;
    xkb_keymap* m_keymap = nullptr;
    xkb_state* m_xkbState = nullptr;
    UniqueFd m_keymapFd; // sealed memfd shared with every client
    std::uint32_t m_keymapSize = 0;
    std::vector<std::uint32_t> m_pressedKeys;
    std::uint32_t m_modsSent[4] = {};

    Tracked<proto::WlPointer> m_pointers;
    Tracked<proto::WlKeyboard> m_keyboards;
    Tracked<proto::WlDataDevice> m_dataDevices;

    Surface* m_keyboardFocus = nullptr;
    Connection m_keyboardFocusGone;
    Surface* m_pointerFocus = nullptr;
    Connection m_pointerFocusGone;
    PointF m_pointerLocal;
    std::vector<std::uint32_t> m_pressedButtons;
    std::uint32_t m_lastButtonSerial = 0;

    DataSource* m_selection = nullptr;
    Connection m_selectionGone;
};

} // namespace dragon
