#include "protocols/XdgActivation.h"

#include "Server.h"
#include "core/Log.h"
#include "protocols/Compositor.h"
#include "protocols/Seat.h"

#include <format>
#include <random>

namespace dragon {

namespace {

constexpr auto kClientTokenLifetime = std::chrono::seconds(30);
constexpr auto kLaunchTokenLifetime = std::chrono::seconds(120); // apps can be slow to start

class TokenResource final : public proto::XdgActivationTokenV1 {
public:
    TokenResource(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server, XdgActivation& activation)
        : XdgActivationTokenV1(client, version, id), m_server(server), m_activation(activation) {}

protected:
    void handleSetSerial(std::uint32_t serial, wl_resource*) override { m_serial = serial; }
    void handleSetAppId(std::string_view) override {}
    void handleSetSurface(wl_resource*) override {}

    void handleCommit() override
    {
        if (m_committed) {
            postError(proto::XdgActivationTokenV1Error::AlreadyUsed, "token already committed");
            return;
        }
        m_committed = true;
        // Trusted only if the request came straight out of a user action in this client.
        const bool trusted = m_serial && m_server.seat().isRecentInputSerial(client(), *m_serial);
        sendDone(m_activation.createToken(trusted, kClientTokenLifetime).c_str());
    }

private:
    Server& m_server;
    XdgActivation& m_activation;
    std::optional<std::uint32_t> m_serial;
    bool m_committed = false;
};

class ActivationResource final : public proto::XdgActivationV1 {
public:
    ActivationResource(wl_client* client, std::uint32_t version, std::uint32_t id, Server& server,
                       XdgActivation& activation)
        : XdgActivationV1(client, version, id), m_server(server), m_activation(activation) {}

protected:
    void handleGetActivationToken(std::uint32_t id) override
    {
        makeResource<TokenResource>(client(), version(), id, m_server, m_activation);
    }

    void handleActivate(std::string_view token, wl_resource* surface) override
    {
        Surface* s = Surface::from(surface);
        if (s && m_activation.redeem(std::string(token)))
            m_activation.activationRequested.emit(s);
    }

private:
    Server& m_server;
    XdgActivation& m_activation;
};

} // namespace

XdgActivation::XdgActivation(Server& server) : m_server(server)
{
    m_global = Global::create<proto::XdgActivationV1>(
        server.display(), 1, [this](wl_client* client, std::uint32_t version, std::uint32_t id) {
            makeResource<ActivationResource>(client, version, id, m_server, *this);
        });
}

XdgActivation::~XdgActivation() = default;

std::string XdgActivation::createCompositorToken()
{
    return createToken(true, kLaunchTokenLifetime);
}

std::string XdgActivation::createToken(bool trusted, std::chrono::seconds lifetime)
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(m_tokens, [now](const auto& entry) { return entry.second.expires < now; });

    static std::random_device rd;
    std::string token = std::format("dragon-{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());
    m_tokens.emplace(token, Token{now + lifetime, trusted});
    return token;
}

bool XdgActivation::redeem(const std::string& token)
{
    auto it = m_tokens.find(token);
    if (it == m_tokens.end()) {
        log::debug("proto", "activation: unknown or used token");
        return false;
    }
    const Token t = it->second;
    m_tokens.erase(it); // single use
    if (t.expires < std::chrono::steady_clock::now()) {
        log::debug("proto", "activation: token expired");
        return false;
    }
    if (!t.trusted)
        log::debug("proto", "activation: token not backed by user input, not focusing");
    return t.trusted;
}

} // namespace dragon
