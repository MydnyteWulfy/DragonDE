#pragma once

#include "core/Geometry.h"
#include "core/Signal.h"

#include <vector>

namespace dragon {

class Backend;
class Desktop;
class Seat;
class Surface;
struct PointerAxisEvent;

// Turns a backend's raw input into seat events: finds the surface under the
// pointer, applies focus policy (click to focus and raise) and keeps pointer
// focus on the pressed surface while a button is held (the implicit grab, so
// drags and text selection continue outside the window).
class InputRouter {
public:
    InputRouter(Backend& backend, Desktop& desktop, Seat& seat);
    ~InputRouter();
    InputRouter(const InputRouter&) = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    PointF pointerPosition() const { return m_pointer; }

private:
    void motion(PointF global, std::uint32_t timeMs);
    void button(std::uint32_t button, bool pressed, std::uint32_t timeMs);
    // Re-picks the surface under the pointer (after motion, release, stacking changes).
    void updatePointerFocus(std::uint32_t timeMs, bool sendMotion);

    Desktop& m_desktop;
    Seat& m_seat;
    PointF m_pointer;
    bool m_pointerInside = false;
    std::vector<Connection> m_connections;
};

} // namespace dragon
