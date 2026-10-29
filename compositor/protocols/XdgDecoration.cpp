#include "protocols/XdgDecoration.h"

#include "protocols/XdgShell.h"
#include "wayland/Global.h"

namespace dragon {

namespace {

class DecorationManager final : public proto::ZxdgDecorationManagerV1 {
public:
    using ZxdgDecorationManagerV1::ZxdgDecorationManagerV1;

protected:
    void handleGetToplevelDecoration(std::uint32_t id, wl_resource* toplevelResource) override
    {
        XdgToplevel* toplevel = XdgToplevel::from(toplevelResource);
        if (toplevel && toplevel->decoration()) {
            wl_resource_post_error(toplevelResource,
                                   std::uint32_t(proto::ZxdgToplevelDecorationV1Error::AlreadyConstructed),
                                   "this xdg_toplevel already has a decoration object");
            return;
        }
        makeResource<XdgToplevelDecoration>(client(), version(), id, toplevel);
    }
};

} // namespace

XdgToplevelDecoration::XdgToplevelDecoration(wl_client* client, std::uint32_t version, std::uint32_t id,
                                             XdgToplevel* toplevel)
    : ZxdgToplevelDecorationV1(client, version, id), m_toplevel(toplevel)
{
    if (!m_toplevel)
        return;
    m_toplevelGone = m_toplevel->destroyed.connect([this] { m_toplevel = nullptr; });
    m_toplevel->attachDecoration(this);
}

XdgToplevelDecoration::~XdgToplevelDecoration()
{
    if (m_toplevel)
        m_toplevel->attachDecoration(nullptr);
}

void XdgToplevelDecoration::handleSetMode(Mode mode)
{
    if (mode != Mode::ClientSide && mode != Mode::ServerSide) {
        postError(Error::InvalidMode, "unknown decoration mode");
        return;
    }
    m_requested = mode;
    if (m_toplevel)
        m_toplevel->decorationRequested();
}

void XdgToplevelDecoration::handleUnsetMode()
{
    m_requested.reset();
    if (m_toplevel)
        m_toplevel->decorationRequested();
}

std::unique_ptr<Global> createXdgDecorationGlobal(wl_display* display)
{
    return Global::create<proto::ZxdgDecorationManagerV1>(
        display, 1, [](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<DecorationManager>(client, version, id);
        });
}

} // namespace dragon
