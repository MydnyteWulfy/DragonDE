#include "desktop/InputRouter.h"

#include "backend/Backend.h"
#include "core/Log.h"
#include "desktop/Desktop.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"

namespace dragon {

InputRouter::InputRouter(Backend& backend, Desktop& desktop, Seat& seat) : m_desktop(desktop), m_seat(seat)
{
    auto& in = backend.input;
    // Single output at the origin for now: output pixels are global coordinates.
    m_connections.push_back(in.pointerMotionAbsolute.connect(
        [this](Output*, double x, double y, std::uint32_t t) { motion({x, y}, t); }));
    m_connections.push_back(in.pointerButton.connect(
        [this](std::uint32_t b, bool pressed, std::uint32_t t) { button(b, pressed, t); }));
    m_connections.push_back(in.pointerAxis.connect([this](const PointerAxisEvent& e) { m_seat.notifyAxis(e); }));
    m_connections.push_back(in.pointerFrame.connect([this] { m_seat.notifyFrame(); }));
    m_connections.push_back(in.pointerLeft.connect([this] {
        m_pointerInside = false;
        if (!m_seat.buttonsPressed())
            m_seat.notifyPointerFocus(nullptr, {});
    }));
    m_connections.push_back(in.key.connect(
        [this](std::uint32_t key, bool pressed, std::uint32_t t) { m_seat.notifyKey(t, key, pressed); }));
    m_connections.push_back(in.keymap.connect([this](const std::string& keymap) { m_seat.setKeymap(keymap); }));

    // Windows appearing, vanishing or restacking change what's under a still pointer.
    m_connections.push_back(desktop.windowMapped.connect([this](Desktop::Window*) { updatePointerFocus(0, false); }));
    m_connections.push_back(
        desktop.windowUnmapped.connect([this](Desktop::Window*) { updatePointerFocus(0, false); }));
}

InputRouter::~InputRouter() = default;

void InputRouter::motion(PointF global, std::uint32_t timeMs)
{
    m_pointer = global;
    m_pointerInside = true;
    updatePointerFocus(timeMs, true);
}

void InputRouter::updatePointerFocus(std::uint32_t timeMs, bool sendMotion)
{
    if (!m_pointerInside)
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
    // Click to focus: the first press on a window raises and focuses it.
    if (pressed && !m_seat.buttonsPressed()) {
        const auto hit = m_desktop.surfaceAt(m_pointer);
        if (Desktop::Window* window = hit.surface ? m_desktop.windowOf(hit.surface) : nullptr)
            m_desktop.focus(window);
        if (hit.surface != m_seat.pointerFocus())
            m_seat.notifyPointerFocus(hit.surface, hit.local);
    }

    m_seat.notifyButton(timeMs, button, pressed);

    // Grab over: the pointer may be above something else by now.
    if (!pressed && !m_seat.buttonsPressed())
        updatePointerFocus(timeMs, false);
}

} // namespace dragon
