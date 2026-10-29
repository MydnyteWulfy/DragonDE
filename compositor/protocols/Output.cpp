#include "protocols/Output.h"

#include "Server.h"
#include "backend/Backend.h"

#include <algorithm>

namespace dragon {

namespace {
constexpr std::uint32_t kOutputVersion = 4;
constexpr std::uint32_t kXdgOutputVersion = 3;

class XdgOutputManager final : public proto::ZxdgOutputManagerV1 {
public:
    XdgOutputManager(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server)
        : ZxdgOutputManagerV1(client, version, id), m_server(server) {}

protected:
    void handleGetXdgOutput(std::uint32_t id, wl_resource* output) override
    {
        auto* xdg = makeResource<proto::ZxdgOutputV1>(client(), version(), id);
        if (!xdg)
            return;
        if (OutputGlobal* global = m_server.outputGlobalFor(output))
            global->addXdgOutput(xdg, output);
        // else: the output is already gone; the object stays inert
    }

private:
    Server& m_server;
};
} // namespace

std::unique_ptr<Global> createXdgOutputManager(Server& server)
{
    return Global::create<proto::ZxdgOutputManagerV1>(
        server.display(), kXdgOutputVersion, [&server](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<XdgOutputManager>(client, version, id, server);
        });
}

bool OutputGlobal::owns(wl_resource* wlOutput) const
{
    return std::ranges::any_of(m_resources, [wlOutput](auto* r) { return r->resource() == wlOutput; });
}

void OutputGlobal::addXdgOutput(proto::ZxdgOutputV1* xdg, wl_resource* wlOutput)
{
    m_xdgOutputs.emplace_back(xdg, wlOutput);
    m_connections.push_back(xdg->destroyed.connect([this, xdg] {
        std::erase_if(m_xdgOutputs, [xdg](const auto& e) { return e.first == xdg; });
    }));
    sendXdgState(*xdg, wlOutput);
}

void OutputGlobal::sendXdgState(proto::ZxdgOutputV1& xdg, wl_resource* wlOutput) const
{
    // One output at the origin, scale 1: logical = physical.
    const auto& mode = m_output.mode();
    xdg.sendLogicalPosition(0, 0);
    xdg.sendLogicalSize(mode.width, mode.height);
    if (xdg.version() >= 2) {
        xdg.sendName(m_output.name().c_str());
        xdg.sendDescription(m_output.description().c_str());
    }
    // v3 batches with wl_output.done instead of its own.
    if (xdg.version() >= 3) {
        for (auto* r : m_resources) {
            if (r->resource() == wlOutput)
                r->sendDone();
        }
    } else {
        xdg.sendDone();
    }
}

void OutputGlobal::sendState(proto::WlOutput& r) const
{
    const auto& mode = m_output.mode();
    // Physical size unknown (nested/headless): 0x0 tells clients not to compute DPI.
    r.sendGeometry(0, 0, 0, 0, proto::WlOutputSubpixel::Unknown, "DragonDE", m_output.name().c_str(),
                   proto::WlOutputTransform::Normal);
    r.sendMode(proto::WlOutputMode::Current | proto::WlOutputMode::Preferred, mode.width, mode.height,
               mode.refreshMhz);
    r.sendScale(1);
    r.sendName(m_output.name().c_str());
    r.sendDescription(m_output.description().c_str());
    r.sendDone();
}

OutputGlobal::OutputGlobal(Server& server, Output& output) : m_output(output)
{
    m_global = Global::create<proto::WlOutput>(
        server.display(), kOutputVersion, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            auto* r = makeResource<proto::WlOutput>(client, version, id);
            if (!r)
                return;
            m_resources.push_back(r);
            m_connections.push_back(r->destroyed.connect([this, r] { std::erase(m_resources, r); }));
            sendState(*r);
        });

    m_modeChanged = output.modeChanged.connect([this] {
        for (auto* r : m_resources)
            sendState(*r);
        for (auto [xdg, wlOutput] : m_xdgOutputs)
            sendXdgState(*xdg, wlOutput);
    });
}

OutputGlobal::~OutputGlobal() = default;

std::vector<wl_resource*> OutputGlobal::resourcesFor(wl_client* client) const
{
    std::vector<wl_resource*> out;
    for (auto* r : m_resources) {
        if (r->client() == client)
            out.push_back(r->resource());
    }
    return out;
}

} // namespace dragon
