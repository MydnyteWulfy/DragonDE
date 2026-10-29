#include "desktop/InputRouter.h"

#include "backend/Backend.h"
#include "core/Log.h"
#include "desktop/Toplevel.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace dragon {

namespace {
using Edge = proto::XdgToplevelResizeEdge;
constexpr std::uint32_t kLeft = std::uint32_t(Edge::Left);
constexpr std::uint32_t kRight = std::uint32_t(Edge::Right);
constexpr std::uint32_t kTop = std::uint32_t(Edge::Top);
constexpr std::uint32_t kBottom = std::uint32_t(Edge::Bottom);
} // namespace

InputRouter::InputRouter(Backend& backend, Desktop& desktop, Seat& seat, WindowManagerConfig config,
                         WindowManagerActions actions)
    : m_desktop(desktop), m_seat(seat), m_config(std::move(config)), m_actions(std::move(actions))
{
    auto& in = backend.input;
    // Single output at the origin for now: output pixels are global coordinates.
    m_connections.push_back(in.pointerMotionAbsolute.connect(
        [this](Output*, double x, double y, std::uint32_t t) { motion({x, y}, t); }));
    m_connections.push_back(in.pointerButton.connect(
        [this](std::uint32_t b, bool pressed, std::uint32_t t) { button(b, pressed, t); }));
    m_connections.push_back(in.pointerAxis.connect([this](const PointerAxisEvent& e) {
        if (!grabbing())
            m_seat.notifyAxis(e);
    }));
    m_connections.push_back(in.pointerFrame.connect([this] {
        if (!grabbing())
            m_seat.notifyFrame();
    }));
    m_connections.push_back(in.pointerLeft.connect([this] {
        m_pointerInside = false;
        if (!m_seat.buttonsPressed() && !grabbing())
            m_seat.notifyPointerFocus(nullptr, {});
    }));
    m_connections.push_back(
        in.key.connect([this](std::uint32_t k, bool pressed, std::uint32_t t) { key(k, pressed, t); }));
    m_connections.push_back(in.keymap.connect([this](const std::string& keymap) { m_seat.setKeymap(keymap); }));

    // Windows appearing, vanishing or restacking change what's under a still pointer.
    m_connections.push_back(desktop.windowMapped.connect([this](Desktop::Window*) { updatePointerFocus(0, false); }));
    m_connections.push_back(desktop.windowUnmapped.connect([this](Desktop::Window* w) {
        if (m_grab.window == w)
            m_grab = {}; // its window is gone: drop the grab
        updatePointerFocus(0, false);
    }));

    // Drag and drop: report where the pointer is the moment a drag starts,
    // and re-pick normal pointer focus when it ends.
    m_connections.push_back(seat.dragStarted.connect([this] {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        m_seat.dragMotion(hit.surface, hit.local, 0);
    }));
    m_connections.push_back(seat.dragEnded.connect([this] { updatePointerFocus(0, false); }));

    m_connections.push_back(
        desktop.moveRequested.connect([this](Desktop::Window* w, std::optional<std::uint32_t> serial) {
            if (clientGrabAllowed(*w, serial))
                startGrab(GrabKind::Move, *w, m_lastClientButton, 0);
        }));
    m_connections.push_back(desktop.resizeRequested.connect(
        [this](Desktop::Window* w, std::optional<std::uint32_t> serial, std::uint32_t edges) {
            if (clientGrabAllowed(*w, serial) && edges != 0)
                startGrab(GrabKind::Resize, *w, m_lastClientButton, edges);
        }));

    log::info("input", "shortcuts: {0}+Enter terminal ({1}), {0}+Q close, {0}+Shift+Esc quit, "
                       "{0}+drag move, {0}+right-drag resize",
              m_config.modifierLabel, m_config.terminal);
}

InputRouter::~InputRouter() = default;

// --------------------------------------------------------------- keyboard --

void InputRouter::key(std::uint32_t key, bool pressed, std::uint32_t timeMs)
{
    if (!pressed && m_swallowedKeys.erase(key))
        return; // the release of a shortcut key
    if (pressed && key == KEY_ESC && m_seat.dragActive()) {
        m_seat.cancelDrag();
        m_swallowedKeys.insert(key);
        return;
    }
    if (pressed && m_seat.modifierActive(m_config.modifier.c_str()) && runShortcut(key)) {
        m_swallowedKeys.insert(key);
        return;
    }
    m_seat.notifyKey(timeMs, key, pressed);
}

bool InputRouter::runShortcut(std::uint32_t key)
{
    const bool shift = m_seat.modifierActive("Shift");
    switch (key) {
    case KEY_ENTER:
    case KEY_KPENTER:
        if (m_actions.spawn)
            m_actions.spawn(m_config.terminal);
        return true;
    case KEY_Q:
        if (auto* w = m_desktop.focusedWindow())
            w->toplevel->close();
        return true;
    case KEY_ESC:
        if (!shift)
            return false;
        log::info("input", "quit shortcut");
        if (m_actions.quit)
            m_actions.quit();
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- pointer --

void InputRouter::motion(PointF global, std::uint32_t timeMs)
{
    m_pointer = global;
    m_pointerInside = true;
    if (grabbing())
        updateGrab();
    else if (m_seat.dragActive()) {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        m_seat.dragMotion(hit.surface, hit.local, timeMs);
    } else
        updatePointerFocus(timeMs, true);
}

void InputRouter::updatePointerFocus(std::uint32_t timeMs, bool sendMotion)
{
    if (!m_pointerInside || grabbing())
        return;

    // While a button is held the pressed surface keeps the pointer (implicit grab).
    if (m_seat.buttonsPressed() && m_seat.pointerFocus()) {
        if (auto origin = m_desktop.originOf(m_seat.pointerFocus()); origin && sendMotion)
            m_seat.notifyPointerMotion(timeMs, m_pointer - *origin);
        return;
    }

    const auto hit = m_desktop.surfaceAt(m_pointer);
    const bool changed = hit.surface != m_seat.pointerFocus();
    m_seat.notifyPointerFocus(hit.surface, hit.local);
    if (hit.surface && sendMotion && !changed)
        m_seat.notifyPointerMotion(timeMs, hit.local);
    if (changed && !sendMotion)
        m_seat.notifyFrame(); // focus changed outside a backend frame: close it ourselves
}

void InputRouter::button(std::uint32_t button, bool pressed, std::uint32_t timeMs)
{
    // Compositor grab: nothing reaches clients until its button comes back up.
    if (grabbing()) {
        if (!pressed) {
            m_swallowedButtons.erase(button);
            m_seat.notifyButton(timeMs, button, false); // keeps the seat's button state honest; no focus, nothing sent
            if (button == m_grab.button)
                endGrab(timeMs);
        }
        return;
    }
    if (!pressed && m_swallowedButtons.erase(button))
        return;

    // Dragging: the release drops; other buttons do nothing meanwhile.
    if (m_seat.dragActive()) {
        if (!pressed) {
            m_seat.notifyButton(timeMs, button, false); // no pointer focus during a drag: nothing sent
            if (!m_seat.buttonsPressed())
                m_seat.dragDrop();
        }
        return;
    }

    // Menus open: a click outside their client closes them and goes nowhere
    // else; a click inside goes to the client without changing focus.
    if (pressed && !m_seat.buttonsPressed() && m_desktop.popupGrabActive()) {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        if (!hit.surface || hit.surface->client() != m_desktop.popupGrabClient()) {
            m_desktop.dismissPopups();
            m_swallowedButtons.insert(button);
            return;
        }
        if (hit.surface != m_seat.pointerFocus())
            m_seat.notifyPointerFocus(hit.surface, hit.local);
        m_lastClientButton = button;
        m_seat.notifyButton(timeMs, button, pressed);
        return;
    }

    if (pressed && !m_seat.buttonsPressed()) {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        Desktop::Window* window = hit.surface ? m_desktop.windowOf(hit.surface) : nullptr;
        if (window)
            m_desktop.focus(window); // click to focus and raise

        // Mod+drag: the compositor moves/resizes; the client never sees the click.
        if (window && m_seat.modifierActive(m_config.modifier.c_str()) &&
            (button == BTN_LEFT || button == BTN_RIGHT)) {
            m_swallowedButtons.insert(button);
            std::uint32_t edges = 0;
            if (button == BTN_RIGHT) { // nearest corner
                const Rect g = m_desktop.geometryOf(*window);
                edges |= m_pointer.x < g.x + g.width / 2.0 ? kLeft : kRight;
                edges |= m_pointer.y < g.y + g.height / 2.0 ? kTop : kBottom;
            }
            startGrab(button == BTN_LEFT ? GrabKind::Move : GrabKind::Resize, *window, button, edges);
            return;
        }
        if (hit.surface != m_seat.pointerFocus())
            m_seat.notifyPointerFocus(hit.surface, hit.local);
    }

    if (pressed)
        m_lastClientButton = button;
    m_seat.notifyButton(timeMs, button, pressed);

    // Implicit grab over: the pointer may be above something else by now.
    if (!pressed && !m_seat.buttonsPressed())
        updatePointerFocus(timeMs, false);
}

// ------------------------------------------------------------------ grabs --

bool InputRouter::clientGrabAllowed(Desktop::Window& window, std::optional<std::uint32_t> serial) const
{
    // Only while a button is held, so clients can't start drags on their own.
    if (grabbing() || !m_seat.buttonsPressed())
        return false;
    // Wayland quotes the press; X11 can't, so the press must be on that window.
    if (serial)
        return *serial == m_seat.lastButtonSerial();
    return m_seat.pointerFocus() && m_desktop.windowOf(m_seat.pointerFocus()) == &window;
}

void InputRouter::startGrab(GrabKind kind, Desktop::Window& window, std::uint32_t button, std::uint32_t edges)
{
    // Moving a maximized window un-maximizes it under the pointer.
    if (kind == GrabKind::Move && window.restore) {
        const auto restored = window.restore->size();
        m_desktop.setMaximized(window, false);
        m_desktop.setFullscreen(window, false);
        m_desktop.moveWindow(window, {std::int32_t(m_pointer.x) - restored.width / 2, std::int32_t(m_pointer.y) - 10});
    }

    m_seat.notifyPointerFocus(nullptr, {}); // clients see a leave; input is ours until release
    m_seat.notifyFrame();
    m_grab = {kind, &window, button, m_pointer, window.position, m_desktop.geometryOf(window), edges};
    if (kind == GrabKind::Resize)
        m_desktop.beginResize(window, edges);
}

void InputRouter::updateGrab()
{
    auto& w = *m_grab.window;
    const PointF d = m_pointer - m_grab.startPointer;
    const auto dx = std::int32_t(std::lround(d.x)), dy = std::int32_t(std::lround(d.y));

    if (m_grab.kind == GrabKind::Move) {
        m_desktop.moveWindow(w, m_grab.startPosition + Point{dx, dy});
        return;
    }

    const Rect s = m_grab.startGeometry;
    std::int32_t width = s.width, height = s.height;
    if (m_grab.edges & kLeft)
        width -= dx;
    if (m_grab.edges & kRight)
        width += dx;
    if (m_grab.edges & kTop)
        height -= dy;
    if (m_grab.edges & kBottom)
        height += dy;

    // Respect the client's limits (0 = none); never below 1px.
    const Size min = w.toplevel->minSize(), max = w.toplevel->maxSize();
    width = std::max({width, min.width, 1});
    height = std::max({height, min.height, 1});
    if (max.width > 0)
        width = std::min(width, max.width);
    if (max.height > 0)
        height = std::min(height, max.height);
    w.toplevel->setSize({width, height}); // the client resizes; Desktop anchors left/top edges on commit
}

void InputRouter::endGrab(std::uint32_t timeMs)
{
    if (m_grab.kind == GrabKind::Resize && m_grab.window)
        m_desktop.endResize(*m_grab.window);
    m_grab = {};
    updatePointerFocus(timeMs, true);
}

} // namespace dragon
