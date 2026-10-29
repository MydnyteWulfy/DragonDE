#pragma once

#include "config/Settings.h"
#include "core/Signal.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dragon {

// What shortcuts (and dragon.* functions in config.lua) can do.
struct ConfigActions {
    std::function<void(const std::string& command)> spawn;
    std::function<void()> quit;
    std::function<void()> close;
    std::function<void()> minimize;
    std::function<void()> restore; // the most recently minimized window
    std::function<void()> toggleMaximize;
    std::function<void()> toggleFullscreen;
    std::function<void()> reload; // must defer: it may be called from inside the config
};

// The user configuration: ~/.config/dragon/config.lua, a Lua script that
// adjusts settings (dragon.set{...}) and shortcuts (dragon.bind/unbind) on
// top of built-in defaults. Every load runs in a fresh Lua state; a script
// with an error leaves the previous configuration in place.
class Config {
public:
    explicit Config(ConfigActions actions);
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    // $XDG_CONFIG_HOME/dragon/config.lua (default ~/.config/...).
    static std::filesystem::path defaultPath();

    // A missing file means "all defaults" and succeeds.
    bool loadFile(const std::filesystem::path& path);
    bool loadString(std::string_view source, std::string_view chunkName = "config");

    const Settings& settings() const;
    // settings().windows with the dragon.rule overrides for this window applied.
    WindowStyle styleFor(std::string_view appId, std::string_view title) const;
    // How a layer surface (by its namespace: "waybar", "wofi", ...) is drawn.
    LayerStyle layerStyleFor(std::string_view nameSpace) const;
    // The shortcut modifier, "auto" resolved: "super", "alt" or "ctrl".
    std::string modifier() const;
    // What "auto" means here (Alt when nested).
    void setAutoModifier(std::string name) { m_autoModifier = std::move(name); }
    // Why the last load failed (empty after a success).
    const std::string& lastError() const { return m_error; }

    // Runs the shortcut for `combo` (real modifiers, no Mod bit). False if none is bound.
    bool trigger(const KeyCombo& combo);
    // Bound shortcuts and what they do (for logs and help).
    std::vector<std::pair<std::string, std::string>> describeBindings() const;

    Signal<> changed; // after every successful load

    struct State; // one evaluated config (Lua state + results); internal

private:
    std::unique_ptr<State> build(std::string_view source, std::string_view chunkName);
    void runBuiltin(const std::string& action);

    ConfigActions m_actions;
    std::unique_ptr<State> m_state;
    std::string m_error;
    std::string m_autoModifier = "super";
};

} // namespace dragon
