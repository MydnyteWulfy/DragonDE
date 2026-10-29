#include "config/Config.h"

#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace dragon {

namespace {

const std::set<std::string> kBuiltins = {"terminal", "close",      "quit",       "minimize",
                                         "restore",  "maximize",   "fullscreen", "reload"};
const std::set<std::string> kButtons = {"minimize", "maximize", "close"};

const std::pair<const char*, const char*> kDefaultBindings[] = {
    {"Mod+Return", "terminal"}, {"Mod+Q", "close"},       {"Mod+Shift+Escape", "quit"},
    {"Mod+M", "minimize"},      {"Mod+Shift+M", "restore"}, {"Mod+Up", "maximize"},
    {"Mod+F", "fullscreen"},    {"Mod+Shift+R", "reload"},
};

} // namespace

// One evaluation of the config: its Lua state and what the script produced.
struct Config::State {
    struct Binding {
        std::string builtin; // or
        int functionRef = LUA_NOREF;
    };

    Config* owner = nullptr;
    lua_State* L = nullptr;
    Settings settings;
    std::map<KeyCombo, Binding> bindings; // as written: "Mod" unresolved
    struct Rule {
        std::string appId, title; // globs; empty matches anything
        int styleRef = LUA_NOREF;  // the override table
    };
    std::vector<Rule> rules; // in order: later ones win
    std::vector<Rule> layerRules; // appId holds the namespace glob

    ~State()
    {
        if (L)
            lua_close(L);
    }

    static State& from(lua_State* L) { return *static_cast<State*>(lua_touserdata(L, lua_upvalueindex(1))); }

    void spawn(const std::string& command)
    {
        if (owner && owner->m_actions.spawn)
            owner->m_actions.spawn(command);
    }
    void run(const std::string& action)
    {
        if (owner)
            owner->runBuiltin(action);
    }
};

namespace {

// Lua errors longjmp, which would skip C++ destructors. Our code throws this
// instead; guarded() turns it into a Lua error once nothing needs unwinding.
struct LuaError {
    std::string message;
};

[[noreturn]] [[gnu::format(printf, 1, 2)]] void fail(const char* format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    throw LuaError{buffer};
}

template <typename F>
int guarded(lua_State* L, F&& body)
{
    try {
        return body();
    } catch (const LuaError& e) {
        luaL_where(L, 1); // "config.lua:12: "
        lua_pushstring(L, e.message.c_str());
    }
    lua_concat(L, 2);
    return lua_error(L);
}

// ------------------------------------------------------- reading lua values --

std::string fieldName(lua_State* L, int keyIndex)
{
    if (lua_type(L, keyIndex) != LUA_TSTRING)
        fail("setting names must be strings");
    return lua_tostring(L, keyIndex);
}

std::string checkString(lua_State* L, const std::string& name)
{
    if (lua_type(L, -1) != LUA_TSTRING)
        fail("'%s' must be a string", name.c_str());
    return lua_tostring(L, -1);
}

std::int32_t checkInt(lua_State* L, const std::string& name, std::int32_t min, std::int32_t max)
{
    int isInt = 0;
    const lua_Integer v = lua_tointegerx(L, -1, &isInt);
    if (!isInt || v < min || v > max)
        fail("'%s' must be a whole number from %d to %d", name.c_str(), int(min), int(max));
    return std::int32_t(v);
}

double checkNumber(lua_State* L, const std::string& name, double min, double max)
{
    int isNum = 0;
    const lua_Number v = lua_tonumberx(L, -1, &isNum);
    if (!isNum || v < min || v > max)
        fail("'%s' must be a number from %g to %g", name.c_str(), min, max);
    return v;
}

bool checkBool(lua_State* L, const std::string& name)
{
    if (lua_type(L, -1) != LUA_TBOOLEAN)
        fail("'%s' must be true or false", name.c_str());
    return lua_toboolean(L, -1);
}

Color checkColor(lua_State* L, const std::string& name)
{
    const auto c = Color::parse(checkString(L, name));
    if (!c)
        fail("'%s' must be a color like \"#rrggbb\" or \"#rrggbbaa\"", name.c_str());
    return *c;
}

std::string checkChoice(lua_State* L, const std::string& name, std::initializer_list<const char*> choices)
{
    const std::string v = checkString(L, name);
    for (const char* c : choices) {
        if (v == c)
            return v;
    }
    std::string list;
    for (const char* c : choices)
        list += std::string(list.empty() ? "" : ", ") + c;
    fail("'%s' must be one of: %s", name.c_str(), list.c_str());
}

void checkTable(lua_State* L, const std::string& name)
{
    if (lua_type(L, -1) != LUA_TTABLE)
        fail("'%s' must be a table", name.c_str());
}

// Calls `field(key)` for every key of the table at the top of the stack, with
// the value at the top during the call.
template <typename F>
void forEachField(lua_State* L, F&& field)
{
    const int table = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        field(fieldName(L, -2));
        lua_pop(L, 1);
    }
}

[[noreturn]] void unknown(const std::string& where, const std::string& key)
{
    fail("unknown setting '%s%s'", where.c_str(), key.c_str());
}

Paint checkPaint(lua_State* L, const std::string& name)
{
    if (lua_type(L, -1) == LUA_TSTRING)
        return {checkColor(L, name)};
    if (lua_type(L, -1) != LUA_TTABLE || lua_rawlen(L, -1) != 2)
        fail("'%s' must be a color or a gradient { \"#from\", \"#to\" }", name.c_str());
    Paint p;
    lua_rawgeti(L, -1, 1);
    p.color = checkColor(L, name);
    lua_pop(L, 1);
    lua_rawgeti(L, -1, 2);
    p.end = checkColor(L, name);
    lua_pop(L, 1);
    return p;
}

void readTitlebarColors(lua_State* L, TitlebarColors& c, const std::string& prefix)
{
    forEachField(L, [&](const std::string& k) {
        const std::string name = prefix + k;
        if (k == "background") c.background = checkColor(L, name);
        else if (k == "text") c.text = checkColor(L, name);
        else if (k == "button") c.button = checkColor(L, name);
        else if (k == "button_hover") c.buttonHover = checkColor(L, name);
        else if (k == "close_hover") c.closeHover = checkColor(L, name);
        else unknown(prefix, k);
    });
}

void readTitlebar(lua_State* L, TitlebarStyle& t, const std::string& prefix)
{
    forEachField(L, [&](const std::string& k) {
        const std::string name = prefix + k;
        if (k == "enabled") t.enabled = checkBool(L, name);
        else if (k == "height") t.height = checkInt(L, name, 12, 200);
        else if (k == "font") t.font = checkString(L, name);
        else if (k == "font_size") t.fontSize = checkNumber(L, name, 4, 72);
        else if (k == "bold") t.bold = checkBool(L, name);
        else if (k == "align") t.align = checkChoice(L, name, {"left", "center", "right"});
        else if (k == "buttons_side") t.buttonsSide = checkChoice(L, name, {"left", "right"});
        else if (k == "button_style") t.buttonStyle = checkChoice(L, name, {"icon", "circle"});
        else if (k == "button_size") t.buttonSize = checkInt(L, name, 8, 100);
        else if (k == "button_spacing") t.buttonSpacing = checkInt(L, name, 0, 50);
        else if (k == "padding") t.padding = checkInt(L, name, 0, 100);
        else if (k == "buttons") {
            checkTable(L, name);
            t.buttons.clear();
            const auto n = lua_Integer(lua_rawlen(L, -1));
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                const std::string b = checkString(L, name);
                if (!kButtons.contains(b))
                    fail("unknown button '%s' (minimize, maximize, close)", b.c_str());
                t.buttons.push_back(b);
                lua_pop(L, 1);
            }
        } else if (k == "colors") {
            checkTable(L, name);
            forEachField(L, [&](const std::string& state) {
                checkTable(L, name + "." + state);
                if (state == "active") readTitlebarColors(L, t.active, name + ".active.");
                else if (state == "inactive") readTitlebarColors(L, t.inactive, name + ".inactive.");
                else unknown(name + ".", state);
            });
        } else unknown(prefix, k);
    });
}

// `blur = true | false | { ... }`
void readBlur(lua_State* L, BlurStyle& b, const std::string& name)
{
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        b.enabled = lua_toboolean(L, -1);
        return;
    }
    checkTable(L, name);
    forEachField(L, [&](const std::string& f) {
        const std::string n = name + "." + f;
        if (f == "enabled") b.enabled = checkBool(L, n);
        else if (f == "size") b.size = checkInt(L, n, 1, 64);
        else if (f == "passes") b.passes = checkInt(L, n, 1, 6);
        else if (f == "noise") b.noise = checkNumber(L, n, 0, 1);
        else if (f == "contrast") b.contrast = checkNumber(L, n, 0, 2);
        else if (f == "brightness") b.brightness = checkNumber(L, n, 0, 2);
        else if (f == "vibrancy") b.vibrancy = checkNumber(L, n, 0, 1);
        else if (f == "xray") b.xray = checkBool(L, n);
        else unknown(name + ".", f);
    });
}

// The overrides of dragon.layer_rule.
void readLayerStyle(lua_State* L, LayerStyle& s)
{
    forEachField(L, [&](const std::string& k) {
        if (k == "blur") {
            // Layers start without blur: a table means "on, like this" unless it says enabled = false.
            if (lua_type(L, -1) == LUA_TTABLE)
                s.blur.enabled = true;
            readBlur(L, s.blur, k);
        } else if (k == "opacity") s.opacity = checkNumber(L, k, 0.05, 1);
        else if (k == "ignore_alpha") s.ignoreAlpha = checkNumber(L, k, 0, 1);
        else if (k == "corner_radius") s.cornerRadius = checkInt(L, k, 0, 100);
        else if (k == "shadow") {
            if (lua_type(L, -1) == LUA_TBOOLEAN) {
                s.shadow.enabled = lua_toboolean(L, -1);
                return;
            }
            checkTable(L, k);
            s.shadow.enabled = true;
            forEachField(L, [&](const std::string& f) {
                const std::string n = "shadow." + f;
                if (f == "enabled") s.shadow.enabled = checkBool(L, n);
                else if (f == "size") s.shadow.size = checkInt(L, n, 1, 200);
                else if (f == "color") s.shadow.color = checkColor(L, n);
                else if (f == "power") s.shadow.power = checkNumber(L, n, 0.25, 10);
                else unknown("shadow.", f);
            });
        } else unknown("", k);
    });
}

std::string expandHome(std::string path)
{
    if (path.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        path = std::string(home ? home : "") + path.substr(1);
    }
    return path;
}

// The `windows` table of dragon.set, and the overrides of dragon.rule.
void readWindowStyle(lua_State* L, WindowStyle& w, const std::string& prefix)
{
    forEachField(L, [&](const std::string& k) {
        const std::string name = prefix + k;
        if (k == "corner_radius") w.cornerRadius = checkInt(L, name, 0, 100);
        else if (k == "opacity") {
            // A number for both states, or { active = .., inactive = .. }.
            if (lua_type(L, -1) == LUA_TNUMBER) {
                w.activeOpacity = w.inactiveOpacity = checkNumber(L, name, 0.05, 1);
                return;
            }
            checkTable(L, name);
            forEachField(L, [&](const std::string& state) {
                if (state == "active") w.activeOpacity = checkNumber(L, name + ".active", 0.05, 1);
                else if (state == "inactive") w.inactiveOpacity = checkNumber(L, name + ".inactive", 0.05, 1);
                else if (state == "fullscreen") w.fullscreenOpacity = checkNumber(L, name + ".fullscreen", 0.05, 1);
                else unknown(name + ".", state);
            });
        } else if (k == "border") {
            checkTable(L, name);
            forEachField(L, [&](const std::string& f) {
                const std::string n = name + "." + f;
                if (f == "width") w.border.width = checkInt(L, n, 0, 50);
                else if (f == "active") w.border.active = checkPaint(L, n);
                else if (f == "inactive") w.border.inactive = checkPaint(L, n);
                else if (f == "gradient_angle") w.border.gradientAngle = checkNumber(L, n, -360, 360);
                else unknown(name + ".", f);
            });
        } else if (k == "shadow") {
            checkTable(L, name);
            forEachField(L, [&](const std::string& f) {
                const std::string n = name + "." + f;
                if (f == "enabled") w.shadow.enabled = checkBool(L, n);
                else if (f == "size") w.shadow.size = checkInt(L, n, 1, 200);
                else if (f == "color") w.shadow.color = checkColor(L, n);
                else if (f == "inactive_color") w.shadow.inactiveColor = checkColor(L, n);
                else if (f == "power") w.shadow.power = checkNumber(L, n, 0.25, 10);
                else if (f == "offset") {
                    if (lua_type(L, -1) != LUA_TTABLE || lua_rawlen(L, -1) != 2)
                        fail("'%s' must be { x, y }", n.c_str());
                    lua_rawgeti(L, -1, 1);
                    w.shadow.offsetX = checkInt(L, n, -200, 200);
                    lua_pop(L, 1);
                    lua_rawgeti(L, -1, 2);
                    w.shadow.offsetY = checkInt(L, n, -200, 200);
                    lua_pop(L, 1);
                } else unknown(name + ".", f);
            });
        } else if (k == "glow") {
            checkTable(L, name);
            forEachField(L, [&](const std::string& f) {
                const std::string n = name + "." + f;
                if (f == "enabled") w.glow.enabled = checkBool(L, n);
                else if (f == "size") w.glow.size = checkInt(L, n, 1, 200);
                else if (f == "active") w.glow.active = checkColor(L, n);
                else if (f == "inactive") w.glow.inactive = checkColor(L, n);
                else if (f == "power") w.glow.power = checkNumber(L, n, 0.25, 10);
                else unknown(name + ".", f);
            });
        } else if (k == "blur") {
            readBlur(L, w.blur, name);
        } else if (k == "dim") {
            // A strength, or { strength = .., color = .. }.
            if (lua_type(L, -1) == LUA_TNUMBER) {
                w.dim.strength = checkNumber(L, name, 0, 1);
                return;
            }
            checkTable(L, name);
            forEachField(L, [&](const std::string& f) {
                if (f == "strength") w.dim.strength = checkNumber(L, name + ".strength", 0, 1);
                else if (f == "color") w.dim.color = checkColor(L, name + ".color");
                else unknown(name + ".", f);
            });
        } else if (k == "titlebar") {
            checkTable(L, name);
            readTitlebar(L, w.titlebar, name + ".");
        } else unknown(prefix, k);
    });
}

void readKeyboard(lua_State* L, KeyboardSettings& kb)
{
    forEachField(L, [&](const std::string& k) {
        const std::string name = "keyboard." + k;
        if (k == "layout") kb.layout = checkString(L, name);
        else if (k == "variant") kb.variant = checkString(L, name);
        else if (k == "options") kb.options = checkString(L, name);
        else if (k == "model") kb.model = checkString(L, name);
        else if (k == "repeat_rate") kb.repeatRate = checkInt(L, name, 0, 200);
        else if (k == "repeat_delay") kb.repeatDelay = checkInt(L, name, 50, 5000);
        else unknown("keyboard.", k);
    });
}

// ------------------------------------------------------------ dragon.* api --

int luaSet(lua_State* L)
{
    return guarded(L, [L] {
        auto& state = Config::State::from(L);
        if (lua_type(L, 1) != LUA_TTABLE)
            fail("dragon.set expects a table: dragon.set{ terminal = \"foot\" }");
        lua_settop(L, 1);
        forEachField(L, [&](const std::string& k) {
            auto& s = state.settings;
            if (k == "modifier") {
                s.modifier = checkChoice(L, k, {"auto", "super", "alt", "ctrl"});
            } else if (k == "terminal") s.terminal = checkString(L, k);
            else if (k == "background") s.background = checkColor(L, k);
            else if (k == "keyboard") {
                checkTable(L, k);
                readKeyboard(L, s.keyboard);
            } else if (k == "wallpaper") {
                // A path, or { path = .., mode = .. }.
                if (lua_type(L, -1) == LUA_TSTRING) {
                    s.wallpaper.path = expandHome(checkString(L, k));
                } else {
                    checkTable(L, k);
                    forEachField(L, [&](const std::string& f) {
                        if (f == "path") s.wallpaper.path = expandHome(checkString(L, "wallpaper.path"));
                        else if (f == "mode")
                            s.wallpaper.mode = checkChoice(L, "wallpaper.mode", {"fill", "fit", "center", "stretch", "tile"});
                        else unknown("wallpaper.", f);
                    });
                }
            } else if (k == "windows") {
                checkTable(L, k);
                readWindowStyle(L, s.windows, "windows.");
            } else unknown("", k);
        });
        return 0;
    });
}

const char* argString(lua_State* L, int index, const char* what)
{
    if (lua_type(L, index) != LUA_TSTRING)
        fail("%s must be a string", what);
    return lua_tostring(L, index);
}

KeyCombo checkCombo(lua_State* L, int index)
{
    const char* text = argString(L, index, "the shortcut");
    const auto combo = KeyCombo::parse(text);
    if (!combo)
        fail("invalid shortcut '%s' (e.g. \"Mod+Shift+Return\")", text);
    return *combo;
}

int luaBind(lua_State* L)
{
    return guarded(L, [L] {
        auto& state = Config::State::from(L);
        const KeyCombo combo = checkCombo(L, 1);
        Config::State::Binding binding;
        if (lua_isfunction(L, 2)) {
            lua_pushvalue(L, 2);
            binding.functionRef = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            const std::string action = argString(L, 2, "the action");
            if (!kBuiltins.contains(action))
                fail("unknown action '%s' (terminal, close, quit, minimize, restore, maximize, fullscreen, "
                     "reload) - or pass a function",
                     action.c_str());
            binding.builtin = action;
        }
        if (auto it = state.bindings.find(combo); it != state.bindings.end())
            luaL_unref(L, LUA_REGISTRYINDEX, it->second.functionRef);
        state.bindings[combo] = std::move(binding);
        return 0;
    });
}

int luaUnbind(lua_State* L)
{
    return guarded(L, [L] {
        auto& state = Config::State::from(L);
        const KeyCombo combo = checkCombo(L, 1);
        if (auto it = state.bindings.find(combo); it != state.bindings.end()) {
            luaL_unref(L, LUA_REGISTRYINDEX, it->second.functionRef);
            state.bindings.erase(it);
        }
        return 0;
    });
}

int luaRule(lua_State* L)
{
    return guarded(L, [L] {
        auto& state = Config::State::from(L);
        if (lua_type(L, 1) != LUA_TTABLE || lua_type(L, 2) != LUA_TTABLE)
            fail("dragon.rule expects two tables: dragon.rule({ app_id = \"kitty\" }, { opacity = 0.9 })");
        lua_settop(L, 2);
        Config::State::Rule rule;
        lua_pushvalue(L, 1);
        forEachField(L, [&](const std::string& k) {
            if (k == "app_id") rule.appId = checkString(L, "app_id");
            else if (k == "title") rule.title = checkString(L, "title");
            else fail("rules match on app_id and title, not '%s'", k.c_str());
        });
        lua_pop(L, 1);
        if (rule.appId.empty() && rule.title.empty())
            fail("a rule needs app_id or title to match");
        // Check the overrides now, so mistakes are reported at load time.
        WindowStyle scratch = state.settings.windows;
        readWindowStyle(L, scratch, "");
        lua_pushvalue(L, 2);
        rule.styleRef = luaL_ref(L, LUA_REGISTRYINDEX);
        state.rules.push_back(std::move(rule));
        return 0;
    });
}

int luaLayerRule(lua_State* L)
{
    return guarded(L, [L] {
        auto& state = Config::State::from(L);
        if (lua_type(L, 1) != LUA_TTABLE || lua_type(L, 2) != LUA_TTABLE)
            fail("dragon.layer_rule expects two tables: dragon.layer_rule({ namespace = \"waybar\" }, { blur = true })");
        lua_settop(L, 2);
        Config::State::Rule rule;
        lua_pushvalue(L, 1);
        forEachField(L, [&](const std::string& k) {
            if (k == "namespace") rule.appId = checkString(L, "namespace");
            else fail("layer rules match on namespace, not '%s'", k.c_str());
        });
        lua_pop(L, 1);
        if (rule.appId.empty())
            fail("a layer rule needs a namespace to match (see 'layer surface' in the log)");
        LayerStyle scratch;
        readLayerStyle(L, scratch);
        lua_pushvalue(L, 2);
        rule.styleRef = luaL_ref(L, LUA_REGISTRYINDEX);
        state.layerRules.push_back(std::move(rule));
        return 0;
    });
}

int luaSpawn(lua_State* L)
{
    return guarded(L, [L] {
        Config::State::from(L).spawn(argString(L, 1, "the command"));
        return 0;
    });
}

int luaLog(lua_State* L)
{
    // Joined on the Lua stack: __tostring may raise an error.
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1)
            lua_pushliteral(L, " ");
        luaL_tolstring(L, i, nullptr);
    }
    lua_concat(L, n > 0 ? 2 * n - 1 : 0);
    log::info("config", "{}", lua_tostring(L, -1));
    return 0;
}

int luaTerminal(lua_State* L)
{
    lua_pushstring(L, Config::State::from(L).settings.terminal.c_str());
    return 1;
}

// dragon.close(), dragon.quit(), ... run the matching built-in action.
int luaAction(lua_State* L)
{
    auto& state = Config::State::from(L);
    const char* action = lua_tostring(L, lua_upvalueindex(2));
    state.run(action);
    return 0;
}

int traceback(lua_State* L)
{
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

} // namespace

// ------------------------------------------------------------------ Config --

Config::Config(ConfigActions actions) : m_actions(std::move(actions))
{
    m_state = build("", "defaults");
}

Config::~Config() = default;

std::filesystem::path Config::defaultPath()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "dragon" / "config.lua";
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") / ".config" / "dragon" / "config.lua";
}

const Settings& Config::settings() const
{
    return m_state->settings;
}

WindowStyle Config::styleFor(std::string_view appId, std::string_view title) const
{
    WindowStyle style = m_state->settings.windows;
    lua_State* L = m_state->L;
    for (const auto& rule : m_state->rules) {
        if ((!rule.appId.empty() && !globMatch(rule.appId, appId)) || (!rule.title.empty() && !globMatch(rule.title, title)))
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, rule.styleRef);
        try {
            readWindowStyle(L, style, "");
        } catch (const LuaError& e) { // validated when the rule was made: only if the table changed since
            log::warn("config", "rule for '{}' '{}': {}", rule.appId, rule.title, e.message);
        }
        lua_settop(L, 0);
    }
    return style;
}

LayerStyle Config::layerStyleFor(std::string_view nameSpace) const
{
    LayerStyle style;
    lua_State* L = m_state->L;
    for (const auto& rule : m_state->layerRules) {
        if (!globMatch(rule.appId, nameSpace))
            continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, rule.styleRef);
        try {
            readLayerStyle(L, style);
        } catch (const LuaError& e) {
            log::warn("config", "layer rule for '{}': {}", rule.appId, e.message);
        }
        lua_settop(L, 0);
    }
    return style;
}

std::string Config::modifier() const
{
    const auto& m = m_state->settings.modifier;
    return m == "auto" ? m_autoModifier : m;
}

bool Config::loadFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) {
        log::info("config", "no {} - using defaults", path.string());
        return loadString("", "defaults");
    }
    std::stringstream source;
    source << in.rdbuf();
    return loadString(source.str(), "@" + path.string());
}

bool Config::loadString(std::string_view source, std::string_view chunkName)
{
    auto state = build(source, chunkName);
    if (!state) {
        log::error("config", "{}", m_error);
        return false;
    }
    m_state = std::move(state);
    m_error.clear();
    changed.emit();
    return true;
}

std::unique_ptr<Config::State> Config::build(std::string_view source, std::string_view chunkName)
{
    auto state = std::make_unique<State>();
    state->owner = this;
    for (const auto& [combo, action] : kDefaultBindings)
        state->bindings[*KeyCombo::parse(combo)] = {.builtin = action};

    lua_State* L = luaL_newstate();
    state->L = L;
    luaL_openlibs(L);

    // The `dragon` table: every function gets the State as upvalue 1.
    lua_newtable(L);
    auto add = [&](const char* name, lua_CFunction fn) {
        lua_pushlightuserdata(L, state.get());
        lua_pushcclosure(L, fn, 1);
        lua_setfield(L, -2, name);
    };
    add("set", luaSet);
    add("bind", luaBind);
    add("unbind", luaUnbind);
    add("rule", luaRule);
    add("layer_rule", luaLayerRule);
    add("spawn", luaSpawn);
    add("log", luaLog);
    add("terminal", luaTerminal);
    for (const char* action : {"close", "quit", "minimize", "restore", "maximize", "fullscreen", "reload"}) {
        lua_pushlightuserdata(L, state.get());
        lua_pushstring(L, action);
        lua_pushcclosure(L, luaAction, 2);
        lua_setfield(L, -2, action);
    }
    lua_setglobal(L, "dragon");

    if (source.empty())
        return state;

    lua_pushcfunction(L, traceback);
    const std::string name(chunkName);
    if (luaL_loadbuffer(L, source.data(), source.size(), name.c_str()) != LUA_OK ||
        lua_pcall(L, 0, 0, -2) != LUA_OK) {
        m_error = lua_tostring(L, -1);
        return nullptr;
    }
    lua_settop(L, 0);
    return state;
}

void Config::runBuiltin(const std::string& action)
{
    auto call = [](const std::function<void()>& f) {
        if (f)
            f();
    };
    if (action == "terminal") {
        if (m_actions.spawn)
            m_actions.spawn(settings().terminal);
    } else if (action == "close") call(m_actions.close);
    else if (action == "quit") call(m_actions.quit);
    else if (action == "minimize") call(m_actions.minimize);
    else if (action == "restore") call(m_actions.restore);
    else if (action == "maximize") call(m_actions.toggleMaximize);
    else if (action == "fullscreen") call(m_actions.toggleFullscreen);
    else if (action == "reload") call(m_actions.reload);
}

bool Config::trigger(const KeyCombo& combo)
{
    const std::uint32_t modBit = modifierBit(modifier());
    for (const auto& [bound, binding] : m_state->bindings) {
        if (bound.resolved(modBit) != combo)
            continue;
        if (binding.functionRef != LUA_NOREF) {
            lua_State* L = m_state->L;
            lua_pushcfunction(L, traceback);
            lua_rawgeti(L, LUA_REGISTRYINDEX, binding.functionRef);
            if (lua_pcall(L, 0, 0, -2) != LUA_OK)
                log::error("config", "shortcut {}: {}", bound.toString(), lua_tostring(L, -1));
            lua_settop(L, 0);
        } else {
            runBuiltin(binding.builtin);
        }
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> Config::describeBindings() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [combo, binding] : m_state->bindings)
        out.emplace_back(combo.toString(), binding.functionRef != LUA_NOREF ? "(function)" : binding.builtin);
    return out;
}

} // namespace dragon
