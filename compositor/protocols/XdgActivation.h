#pragma once

#include "core/Signal.h"
#include "dragon-protocols/xdg-activation-v1.h"
#include "wayland/Global.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace dragon {

class Server;
class Surface;

// xdg-activation-v1: one client hands another a token so the second may take
// focus (clicking a link in a terminal raises the browser; a launcher starts
// an app). Tokens are random, single-use and expire. Only tokens backed by
// real user input, or minted by the compositor itself, grant focus.
class XdgActivation {
public:
    explicit XdgActivation(Server& server);
    ~XdgActivation();
    XdgActivation(const XdgActivation&) = delete;
    XdgActivation& operator=(const XdgActivation&) = delete;

    // For apps the compositor launches (passed as XDG_ACTIVATION_TOKEN).
    std::string createCompositorToken();
    std::string createToken(bool trusted, std::chrono::seconds lifetime);

    // Uses up `token`. True if it was valid and allows taking focus.
    bool redeem(const std::string& token);

    // A client presented a good token for `surface`: the desktop should focus it.
    Signal<Surface*> activationRequested;

private:
    struct Token {
        std::chrono::steady_clock::time_point expires;
        bool trusted;
    };

    Server& m_server;
    std::unique_ptr<Global> m_global;
    std::unordered_map<std::string, Token> m_tokens;
};

} // namespace dragon
