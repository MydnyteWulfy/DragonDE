#pragma once

#include "dragon-protocols/cursor-shape-v1.h"
#include "wayland/Global.h"

#include <memory>

namespace dragon {

class Seat;
class Server;

// wp_cursor_shape_manager_v1: clients name a cursor ("text", "pointer",
// "ew-resize", ...) instead of drawing one, so it matches the system theme.
class CursorShapeManager {
public:
    CursorShapeManager(Server& server, Seat& seat);
    ~CursorShapeManager();

private:
    std::unique_ptr<Global> m_global;
};

} // namespace dragon
