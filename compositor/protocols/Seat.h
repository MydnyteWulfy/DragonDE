#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"
#include "core/UniqueFd.h"
#include "dragon-protocols/primary-selection-unstable-v1.h"
#include "dragon-protocols/wayland.h"
#include "wayland/Global.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace dragon {

class DataSource;
class PrimarySource;
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
    // ------------------------------------------------------------- cursor --
    // wp_cursor_shape_device_v1 shape values; kHiddenCursor hides it.
    static constexpr std::uint32_t kDefaultCursor = 1;
    static constexpr std::uint32_t kHiddenCursor = 0;
    // A client wants a cursor (set_cursor or cursor-shape). Honored only for the
    // client under the pointer, quoting the serial of its latest enter.
    // `shape` 0 = hidden; std::nullopt = its own cursor image (not drawn yet: default arrow).
    void requestCursor(wl_client* client, std::uint32_t serial, std::optional<std::uint32_t> shape);
    Signal<std::uint32_t> cursorShapeChanged;

    // Serial of the latest button press: move/resize/selection requests must quote it.
    std::uint32_t lastButtonSerial() const { return m_lastButtonSerial; }
    // True if `serial` is one of the recent button/key events `client` received
    // (popup grabs and activation tokens must come from real user input).
    bool isRecentInputSerial(wl_client* client, std::uint32_t serial) const;

    // ----------------------------------------------------------- selection --
    void setSelection(DataSource* source);
    DataSource* selection() const { return m_selection; }
    // Middle-click paste.
    void setPrimarySelection(PrimarySource* source);
    PrimarySource* primarySelection() const { return m_primary; }

    // --- protocol object registration (resources call these) ---
    void addPointer(proto::WlPointer* pointer);
    void addKeyboard(proto::WlKeyboard* keyboard);
    void addDataDevice(proto::WlDataDevice* device);
    void addPrimaryDevice(proto::ZwpPrimarySelectionDeviceV1* device);

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
    void sendPrimarySelection(wl_client* client);
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
    Tracked<proto::ZwpPrimarySelectionDeviceV1> m_primaryDevices;

    Surface* m_keyboardFocus = nullptr;
    Connection m_keyboardFocusGone;
    Surface* m_pointerFocus = nullptr;
    Connection m_pointerFocusGone;
    PointF m_pointerLocal;
    std::uint32_t m_pointerEnterSerial = 0;
    std::uint32_t m_cursorShape = kDefaultCursor;
    void setCursorShape(std::uint32_t shape);
    std::vector<std::uint32_t> m_pressedButtons;
    std::uint32_t m_lastButtonSerial = 0;
    struct InputSerial {
        std::uint32_t serial;
        wl_client* client;
    };
    std::vector<InputSerial> m_recentInput; // newest last, bounded
    void rememberInput(std::uint32_t serial, Surface* focus);

    DataSource* m_selection = nullptr;
    Connection m_selectionGone;
    PrimarySource* m_primary = nullptr;
    Connection m_primaryGone;
};

} // namespace dragon
