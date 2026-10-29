#include "desktop/InputRouter.h"

#include "backend/Backend.h"
#include "config/Config.h"
#include "core/Log.h"
#include "desktop/Toplevel.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"
#include "protocols/XdgShell.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace dragon {

namespace {
using Edge = proto::XdgToplevelResizeEdge;
constexpr std::uint32_t kLeft = std::uint32_t(Edge::Left);
constexpr std::uint32_t kRight = std::uint32_t(Edge::Right);
constexpr std::uint32_t kTop = std::uint32_t(Edge::Top);
constexpr std::uint32_t kBottom = std::uint32_t(Edge::Bottom);

// wp_cursor_shape_device_v1 shapes.
constexpr std::uint32_t kCursorDefault = 1, kCursorGrabbing = 17;

std::uint32_t resizeCursor(std::uint32_t edges)
{
    switch (edges) {
    case kTop: return 19;               // n-resize
    case kBottom: return 22;            // s-resize
    case kLeft: return 25;              // w-resize
    case kRight: return 18;             // e-resize
    case kTop | kLeft: return 21;       // nw-resize
    case kTop | kRight: return 20;      // ne-resize
    case kBottom | kLeft: return 24;    // sw-resize
    case kBottom | kRight: return 23;   // se-resize
    default: return kCursorDefault;
    }
}

constexpr std::uint32_t kDoubleClickMs = 400;
constexpr double kDragThreshold = 4.0;
} // namespace

void addWindowActions(ConfigActions& actions, Desktop& desktop)
{
    Desktop* d = &desktop;
    actions.close = [d] {
        if (auto* w = d->focusedWindow())
            w->toplevel->close();
    };
    actions.minimize = [d] {
        if (auto* w = d->focusedWindow())
            d->minimize(*w);
    };
    actions.restore = [d] { d->restoreMinimized(); };
    actions.toggleMaximize = [d] {
        if (auto* w = d->focusedWindow(); w && !w->toplevel->fullscreen())
            d->setMaximized(*w, !w->toplevel->maximized());
    };
    actions.toggleFullscreen = [d] {
        if (auto* w = d->focusedWindow())
            d->setFullscreen(*w, !w->toplevel->fullscreen());
    };
}

InputRouter::InputRouter(Backend& backend, Desktop& desktop, Seat& seat, Config& config)
    : m_desktop(desktop), m_seat(seat), m_config(config)
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
    m_connections.push_back(in.keymap.connect([this](const std::string& keymap) {
        m_backendKeymap = keymap;
        applyKeyboardSettings();
    }));
    m_connections.push_back(config.changed.connect([this] { applyKeyboardSettings(); }));
    applyKeyboardSettings();

    // Windows appearing, vanishing or restacking change what's under a still pointer.
    m_connections.push_back(desktop.windowMapped.connect([this](Desktop::Window*) { updatePointerFocus(0, false); }));
    m_connections.push_back(
        desktop.windowVisibilityChanged.connect([this](Desktop::Window*) { updatePointerFocus(0, false); }));
    m_connections.push_back(desktop.layersChanged.connect([this] { updatePointerFocus(0, false); }));
    m_connections.push_back(desktop.windowUnmapped.connect([this](Desktop::Window* w) {
        if (m_grab.window == w)
            m_grab = {}; // its window is gone: drop the grab
        if (m_pendingMove.window == w)
            m_pendingMove = {};
        if (m_buttonPress.window == w)
            m_buttonPress = {};
        if (m_hoverWindow == w)
            m_hoverWindow = nullptr;
        if (m_lastTitleClick == w)
            m_lastTitleClick = nullptr;
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
    if (pressed && runShortcut(key)) {
        m_swallowedKeys.insert(key);
        return;
    }
    m_seat.notifyKey(timeMs, key, pressed);
}

bool InputRouter::runShortcut(std::uint32_t key)
{
    KeyCombo combo{.keysym = m_seat.baseKeysym(key)};
    if (!combo.keysym)
        return false;
    if (m_seat.modifierActive(XKB_MOD_NAME_SHIFT))
        combo.modifiers |= mods::Shift;
    if (m_seat.modifierActive(XKB_MOD_NAME_CTRL))
        combo.modifiers |= mods::Ctrl;
    if (m_seat.modifierActive(XKB_MOD_NAME_ALT))
        combo.modifiers |= mods::Alt;
    if (m_seat.modifierActive(XKB_MOD_NAME_LOGO))
        combo.modifiers |= mods::Super;
    if (!combo.modifiers && combo.keysym < XKB_KEY_F1)
        return false; // plain typing is never a shortcut (bare F-keys may be)
    return m_config.trigger(combo);
}

bool InputRouter::modifierHeld() const
{
    const std::string mod = m_config.modifier();
    const char* name = mod == "alt" ? XKB_MOD_NAME_ALT : mod == "ctrl" ? XKB_MOD_NAME_CTRL : XKB_MOD_NAME_LOGO;
    return m_seat.modifierActive(name);
}

void InputRouter::applyKeyboardSettings()
{
    const auto& kb = m_config.settings().keyboard;
    m_seat.setRepeatInfo(kb.repeatRate, kb.repeatDelay);
    const bool named = !kb.layout.empty() || !kb.variant.empty() || !kb.options.empty() || !kb.model.empty();
    // Recompiling resends the keymap to every client: only when it changes.
    std::string source = named ? std::format("names:{}:{}:{}:{}", kb.layout, kb.variant, kb.options, kb.model)
                               : m_backendKeymap;
    if (source.empty() || source == m_keymapSource)
        return;
    const bool ok = named ? m_seat.setKeymapNames(kb.layout, kb.variant, kb.options, kb.model)
                          : m_seat.setKeymap(m_backendKeymap);
    if (ok)
        m_keymapSource = std::move(source);
}

// ---------------------------------------------------------------- pointer --

void InputRouter::motion(PointF global, std::uint32_t timeMs)
{
    m_pointer = global;
    m_pointerInside = true;
    if (m_pendingMove.window) {
        const PointF d = m_pointer - m_pendingMove.start;
        if (std::hypot(d.x, d.y) >= kDragThreshold) {
            auto pending = std::exchange(m_pendingMove, {});
            m_pointer = pending.start; // the move starts where the press was
            startGrab(GrabKind::Move, *pending.window, pending.button, 0);
            m_pointer = global;
            updateGrab();
        }
        return;
    }
    if (m_buttonPress.window) {
        // Held on a title bar button: it stays pressed only while the pointer is on it.
        const auto deco = m_desktop.decorationAt(m_pointer);
        const bool over = deco.window == m_buttonPress.window && deco.button == m_buttonPress.index;
        m_buttonPress.window->pressedButton = over ? m_buttonPress.index : -1;
        return;
    }
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

    // Over one of our title bars or borders: the pointer is ours.
    if (const auto deco = m_desktop.decorationAt(m_pointer); deco.window) {
        const bool changed = m_seat.pointerFocus() != nullptr;
        m_seat.notifyPointerFocus(nullptr, {});
        if (changed)
            m_seat.notifyFrame();
        setHover(deco.window, deco.part == Desktop::DecorationHit::Part::Button ? deco.button : -1);
        m_seat.setCursorShape(deco.part == Desktop::DecorationHit::Part::Edge ? resizeCursor(deco.edges)
                                                                              : kCursorDefault);
        return;
    }
    setHover(nullptr, -1);

    const auto hit = m_desktop.surfaceAt(m_pointer);
    const bool changed = hit.surface != m_seat.pointerFocus();
    m_seat.notifyPointerFocus(hit.surface, hit.local);
    if (!hit.surface)
        m_seat.setCursorShape(kCursorDefault);
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
    if (!pressed && decorationRelease(button))
        return;
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

    if (pressed && !m_seat.buttonsPressed() && !modifierHeld() && decorationPress(button, timeMs))
        return;

    if (pressed && !m_seat.buttonsPressed()) {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        Desktop::Window* window = hit.surface ? m_desktop.windowOf(hit.surface) : nullptr;
        if (!window && modifierHeld())
            window = m_desktop.decorationAt(m_pointer).window; // Mod+drag works on the frame too
        if (window)
            m_desktop.focus(window); // click to focus and raise
        else if (hit.surface)
            m_desktop.focusLayer(hit.surface); // a layer surface that takes the keyboard on click

        // Mod+drag: the compositor moves/resizes; the client never sees the click.
        if (window && modifierHeld() &&
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

// ------------------------------------------------------------ decorations --

void InputRouter::setHover(Desktop::Window* window, int button)
{
    if (m_hoverWindow && m_hoverWindow != window)
        m_hoverWindow->hoveredButton = -1;
    m_hoverWindow = window;
    if (window)
        window->hoveredButton = button;
}

bool InputRouter::decorationPress(std::uint32_t button, std::uint32_t timeMs)
{
    using Part = Desktop::DecorationHit::Part;
    const auto deco = m_desktop.decorationAt(m_pointer);
    if (!deco.window)
        return false;
    Desktop::Window& window = *deco.window;
    m_desktop.focus(&window);
    m_swallowedButtons.insert(button); // the release is ours too

    if (button != BTN_LEFT)
        return true; // right click: the window menu, one day
    switch (deco.part) {
    case Part::Titlebar:
        if (m_lastTitleClick == &window && timeMs - m_lastTitleClickTime <= kDoubleClickMs) {
            m_lastTitleClick = nullptr;
            if (!window.toplevel->fullscreen())
                m_desktop.setMaximized(window, !window.toplevel->maximized());
            return true;
        }
        m_lastTitleClick = &window;
        m_lastTitleClickTime = timeMs;
        m_pendingMove = {&window, button, m_pointer};
        return true;
    case Part::Button:
        m_buttonPress = {&window, deco.button, deco.action, button};
        window.pressedButton = deco.button;
        return true;
    case Part::Edge:
        startGrab(GrabKind::Resize, window, button, deco.edges);
        return true;
    case Part::None:
        break;
    }
    return true;
}

bool InputRouter::decorationRelease(std::uint32_t button)
{
    if (m_pendingMove.window && button == m_pendingMove.button) {
        m_pendingMove = {}; // a click on the title bar: nothing to move
        m_swallowedButtons.erase(button);
        updatePointerFocus(0, false);
        return true;
    }
    if (!m_buttonPress.window || button != m_buttonPress.button)
        return false;
    const auto press = std::exchange(m_buttonPress, {});
    m_swallowedButtons.erase(button);
    Desktop::Window& window = *press.window;
    const bool over = window.pressedButton == press.index;
    window.pressedButton = -1;
    if (over) {
        log::debug("input", "title bar button '{}' on '{}'", press.action, window.toplevel->title());
        if (press.action == "close")
            window.toplevel->close();
        else if (press.action == "minimize")
            m_desktop.minimize(window); // may unmap pointer focus targets: refocus below
        else if (press.action == "maximize" && !window.toplevel->fullscreen())
            m_desktop.setMaximized(window, !window.toplevel->maximized());
    }
    updatePointerFocus(0, false);
    return true;
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
    // Moving a maximized window un-maximizes it under the pointer, which
    // keeps its relative spot across the window (and stays in the title bar).
    if (kind == GrabKind::Move && window.restore) {
        const auto restored = window.restore->size();
        const Rect before = m_desktop.frameOf(window).outer;
        const double fx = before.width > 0 ? (m_pointer.x - before.x) / before.width : 0.5;
        m_desktop.setMaximized(window, false);
        m_desktop.setFullscreen(window, false);
        const auto th = m_desktop.frameOf(window).titlebar.height;
        const std::int32_t y = th > 0 ? std::int32_t(m_pointer.y) + th / 2 : std::int32_t(m_pointer.y) - 10;
        m_desktop.moveWindow(window, {std::int32_t(m_pointer.x - fx * restored.width), y});
    }
    setHover(nullptr, -1);
    m_seat.setCursorShape(kind == GrabKind::Move ? kCursorGrabbing : resizeCursor(edges));

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
