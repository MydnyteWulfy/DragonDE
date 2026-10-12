#include "protocols/Output.h"

#include "Server.h"
#include "backend/Backend.h"

#include <algorithm>

namespace dragon {

namespace {
constexpr std::uint32_t kOutputVersion = 4;
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
