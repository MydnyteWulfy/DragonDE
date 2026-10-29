#pragma once

#include "core/Signal.h"
#include "dragon-protocols/wayland.h"
#include "dragon-protocols/xdg-output-unstable-v1.h"
#include "wayland/Global.h"

#include <memory>
#include <vector>

namespace dragon {

class Output;
class Server;

// The wl_output global for one backend Output. Clients bind it to learn the
// output's mode, scale and name; surfaces get enter/leave with its resources.
class OutputGlobal {
public:
    OutputGlobal(Server& server, Output& output);
    ~OutputGlobal();
    OutputGlobal(const OutputGlobal&) = delete;
    OutputGlobal& operator=(const OutputGlobal&) = delete;

    Output& output() const { return m_output; }

    // This client's wl_output resources for this output (for wl_surface.enter).
    std::vector<wl_resource*> resourcesFor(wl_client* client) const;
    bool owns(wl_resource* wlOutput) const;
    // xdg-output: the output's place in the desktop and its name.
    void addXdgOutput(proto::ZxdgOutputV1* xdgOutput, wl_resource* wlOutput);

private:
    void sendState(proto::WlOutput& resource) const;
    void sendXdgState(proto::ZxdgOutputV1& xdgOutput, wl_resource* wlOutput) const;

    Output& m_output;
    std::unique_ptr<Global> m_global;
    std::vector<proto::WlOutput*> m_resources;
    std::vector<std::pair<proto::ZxdgOutputV1*, wl_resource*>> m_xdgOutputs;
    std::vector<Connection> m_connections;
    Connection m_modeChanged;
};

// The zxdg_output_manager_v1 global (one for all outputs).
std::unique_ptr<Global> createXdgOutputManager(Server& server);

} // namespace dragon
