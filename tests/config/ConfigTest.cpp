// config.lua: settings, shortcuts, and what happens with bad input.

#include "config/Config.h"
#include "desktop/Titlebar.h"

#include <gtest/gtest.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <filesystem>
#include <fstream>

using namespace dragon;

TEST(KeyComboTest, Parses)
{
    auto c = KeyCombo::parse("Mod+Shift+Return");
    ASSERT_TRUE(c);
    EXPECT_EQ(c->modifiers, mods::Mod | mods::Shift);
    EXPECT_EQ(c->keysym, std::uint32_t(XKB_KEY_Return));

    EXPECT_EQ(KeyCombo::parse("super+enter"), KeyCombo::parse("Super+Return"));
    EXPECT_EQ(KeyCombo::parse("Ctrl+Alt+T")->keysym, std::uint32_t(XKB_KEY_t)) << "letters are lowercase";
    EXPECT_EQ(KeyCombo::parse("Mod++")->keysym, std::uint32_t(XKB_KEY_plus));
    EXPECT_EQ(KeyCombo::parse("F11")->modifiers, 0u);
    EXPECT_FALSE(KeyCombo::parse("Mod+NoSuchKey"));
    EXPECT_FALSE(KeyCombo::parse("Hyper+a"));
    EXPECT_FALSE(KeyCombo::parse(""));
    EXPECT_FALSE(KeyCombo::parse("Mod+"));
}

TEST(KeyComboTest, ResolvesMod)
{
    const auto c = *KeyCombo::parse("Mod+Q");
    EXPECT_EQ(c.resolved(mods::Alt), *KeyCombo::parse("Alt+Q"));
    EXPECT_EQ(c.resolved(mods::Super).toString(), "Super+q");
}

TEST(ColorTest, Parses)
{
    EXPECT_EQ(Color::parse("#ff0000"), (Color{1, 0, 0, 1}));
    EXPECT_EQ(Color::parse("#f00"), (Color{1, 0, 0, 1}));
    EXPECT_EQ(Color::parse("#00000080")->a, 128 / 255.0f);
    EXPECT_FALSE(Color::parse("red"));
    EXPECT_FALSE(Color::parse("#12345"));
    EXPECT_FALSE(Color::parse("#gg0000"));
}

TEST(ConfigTest, DefaultsWithoutAFile)
{
    Config config({});
    EXPECT_TRUE(config.loadFile("/nonexistent/dragon/config.lua"));
    EXPECT_EQ(config.settings().terminal, "kitty");
    EXPECT_EQ(config.modifier(), "super");
    config.setAutoModifier("alt");
    EXPECT_EQ(config.modifier(), "alt");
    EXPECT_FALSE(config.describeBindings().empty());
}

TEST(ConfigTest, AppliesSettings)
{
    Config config({});
    ASSERT_TRUE(config.loadString(R"(
        dragon.set{
            modifier = "ctrl",
            terminal = "foot",
            background = "#102030",
            keyboard = { layout = "us,de", options = "grp:alt_shift_toggle", repeat_rate = 30 },
            windows = {
                corner_radius = 4,
                opacity = 0.9,
                border = { width = 3, active = { "#ff0000", "#0000ff" } },
                shadow = { offset = { 2, 6 } },
                titlebar = {
                    height = 24,
                    buttons = { "close" },
                    buttons_side = "left",
                    colors = { active = { background = "#000000" } },
                },
            },
        }
        dragon.set{ terminal = "alacritty" } -- later calls override
    )")) << config.lastError();
    const auto& s = config.settings();
    EXPECT_EQ(config.modifier(), "ctrl");
    EXPECT_EQ(s.terminal, "alacritty");
    EXPECT_EQ(s.background, *Color::parse("#102030"));
    EXPECT_EQ(s.keyboard.layout, "us,de");
    EXPECT_EQ(s.keyboard.repeatRate, 30);
    EXPECT_EQ(s.keyboard.repeatDelay, 600) << "untouched settings keep their defaults";
    const auto& w = s.windows;
    EXPECT_EQ(w.cornerRadius, 4);
    EXPECT_FLOAT_EQ(w.activeOpacity, 0.9);
    EXPECT_FLOAT_EQ(w.inactiveOpacity, 0.9);
    EXPECT_EQ(w.border.width, 3);
    EXPECT_EQ(w.border.active.color, (Color{1, 0, 0, 1}));
    EXPECT_EQ(w.border.active.end, (Color{0, 0, 1, 1}));
    EXPECT_FALSE(w.border.inactive.end);
    EXPECT_EQ(w.shadow.offsetX, 2);
    EXPECT_EQ(w.shadow.offsetY, 6);
    EXPECT_EQ(w.titlebar.height, 24);
    EXPECT_EQ(w.titlebar.buttons, (std::vector<std::string>{"close"}));
    EXPECT_EQ(w.titlebar.buttonsSide, "left");
    EXPECT_EQ(w.titlebar.active.background, (Color{0, 0, 0, 1}));
}

TEST(ConfigTest, ErrorsKeepThePreviousConfig)
{
    Config config({});
    ASSERT_TRUE(config.loadString(R"(dragon.set{ terminal = "foot" })"));
    int changes = 0;
    auto c = config.changed.connect([&] { ++changes; });

    for (const char* bad : {
             R"(dragon.set{ terminl = "x" })",                         // typo
             R"(dragon.set{ background = "blue" })",                   // bad color
             R"(dragon.set{ keyboard = { repeat_rate = "fast" } })",   // wrong type
             R"(dragon.set{ windows = { titlebar = { buttons = { "shade" } } } })", // unknown button
             R"(dragon.set{ windows = { opacity = 2 } })",                      // out of range
             R"(dragon.rule({ app_id = "kitty" }, { opacty = 1 }))",           // typo in a rule
             R"(dragon.rule({ class = "kitty" }, { opacity = 1 }))",           // unknown match
             R"(dragon.rule({}, { opacity = 1 }))",                            // matches nothing
             R"(dragon.bind("Mod+Nope", "close"))",                    // bad key
             R"(dragon.bind("Mod+x", "explode"))",                     // bad action
             R"(this is not lua)",
             R"(error("on purpose"))",
         }) {
        EXPECT_FALSE(config.loadString(bad)) << bad;
        EXPECT_FALSE(config.lastError().empty()) << bad;
        EXPECT_EQ(config.settings().terminal, "foot") << bad;
    }
    EXPECT_EQ(changes, 0);
    EXPECT_NE(config.lastError().find("on purpose"), std::string::npos) << config.lastError();
}

TEST(ConfigTest, RulesOverrideTheWindowStyle)
{
    Config config({});
    ASSERT_TRUE(config.loadString(R"(
        dragon.set{ windows = { corner_radius = 12, opacity = { inactive = 0.9 } } }
        dragon.rule({ app_id = "kitty" }, { opacity = { active = 0.8 }, titlebar = { enabled = false } })
        dragon.rule({ app_id = "org.gnome.*" }, { corner_radius = 0 })
        dragon.rule({ app_id = "kitty", title = "*vim*" }, { border = { width = 0 } })
    )")) << config.lastError();

    const auto plain = config.styleFor("foot", "~");
    EXPECT_EQ(plain.cornerRadius, 12);
    EXPECT_TRUE(plain.titlebar.enabled);
    EXPECT_FLOAT_EQ(plain.activeOpacity, 1.0);

    const auto kitty = config.styleFor("kitty", "~");
    EXPECT_FLOAT_EQ(kitty.activeOpacity, 0.8);
    EXPECT_FLOAT_EQ(kitty.inactiveOpacity, 0.9) << "the rest comes from dragon.set";
    EXPECT_FALSE(kitty.titlebar.enabled);
    EXPECT_EQ(kitty.border.width, 2);

    const auto vim = config.styleFor("kitty", "nvim config.lua");
    EXPECT_EQ(vim.border.width, 0) << "rules stack";
    EXPECT_FALSE(vim.titlebar.enabled);

    EXPECT_EQ(config.styleFor("org.gnome.Nautilus", "").cornerRadius, 0);
}

TEST(GlobTest, Matches)
{
    EXPECT_TRUE(globMatch("kitty", "kitty"));
    EXPECT_TRUE(globMatch("Kitty", "kitty"));
    EXPECT_FALSE(globMatch("kitty", "kitty2"));
    EXPECT_TRUE(globMatch("org.*", "org.gnome.Nautilus"));
    EXPECT_TRUE(globMatch("*fire*", "Mozilla Firefox"));
    EXPECT_TRUE(globMatch("f?ot", "foot"));
    EXPECT_TRUE(globMatch("*", ""));
    EXPECT_FALSE(globMatch("a*b", "acd"));
    EXPECT_TRUE(globMatch("a*b*c", "aXbYbZc"));
}

TEST(ConfigTest, ShortcutsRunBuiltinsAndFunctions)
{
    std::vector<std::string> log;
    Config config({.spawn = [&](const std::string& cmd) { log.push_back("spawn " + cmd); },
                   .close = [&] { log.push_back("close"); },
                   .reload = [&] { log.push_back("reload"); }});
    ASSERT_TRUE(config.loadString(R"(
        dragon.set{ modifier = "super", terminal = "foot" }
        local count = 0
        dragon.bind("Mod+b", function()
            count = count + 1
            dragon.spawn("browser " .. count)
        end)
        dragon.bind("Mod+e", function() error("broken shortcut") end)
        dragon.bind("Mod+w", dragon.close)
    )")) << config.lastError();

    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+Return")));
    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+b")));
    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+b")));
    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+e"))) << "a failing function is still the shortcut";
    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+w")));
    EXPECT_TRUE(config.trigger(*KeyCombo::parse("Super+Shift+r")));
    EXPECT_FALSE(config.trigger(*KeyCombo::parse("Alt+Return"))) << "Mod is Super here";
    EXPECT_FALSE(config.trigger(*KeyCombo::parse("Super+z")));
    EXPECT_EQ(log, (std::vector<std::string>{"spawn foot", "spawn browser 1", "spawn browser 2", "close", "reload"}));
}

TEST(ConfigTest, LoadsAFile)
{
    const auto dir = std::filesystem::temp_directory_path() / ("dragon-config-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "config.lua";
    std::ofstream(path) << "dragon.set{ terminal = dragon.terminal() .. '-x' }\n";
    Config config({});
    EXPECT_TRUE(config.loadFile(path)) << config.lastError();
    EXPECT_EQ(config.settings().terminal, "kitty-x");

    std::ofstream(path) << "dragon.set{ nope = 1 }\n";
    EXPECT_FALSE(config.loadFile(path));
    EXPECT_NE(config.lastError().find("config.lua:1"), std::string::npos) << config.lastError();
    std::filesystem::remove_all(dir);
}

TEST(ConfigTest, TheExampleConfigLoads)
{
    Config config({});
    EXPECT_TRUE(config.loadFile(std::filesystem::path(DRAGON_DATA_DIR) / "config" / "config.lua"))
        << config.lastError();
}

TEST(TitlebarLayoutTest, PlacesButtonsAndTitle)
{
    TitlebarStyle s; // buttons 18px, 6 apart, 8 from the edge, on the right
    s.height = 30;
    auto l = layoutTitlebar(s, 200);
    ASSERT_EQ(l.buttons.size(), 3u);
    EXPECT_EQ(l.buttons[0].action, "minimize");
    EXPECT_EQ(l.buttons[0].rect, (Rect{126, 6, 18, 18}));
    EXPECT_EQ(l.buttons[2].rect, (Rect{174, 6, 18, 18}));
    // Centered titles stay centered on the window: equal room both sides.
    EXPECT_EQ(l.text.x, 200 - (l.text.x + l.text.width));
    EXPECT_LE(l.text.x + l.text.width, 126);

    s.buttonsSide = "left";
    s.align = "left";
    s.buttons = {"close"};
    l = layoutTitlebar(s, 200);
    EXPECT_EQ(l.buttons[0].rect, (Rect{8, 6, 18, 18}));
    EXPECT_GE(l.text.x, 26);
    EXPECT_EQ(l.text.x + l.text.width, 192);
}

TEST(ConfigTest, EffectsWallpaperAndLayerRules)
{
    Config config({});
    ASSERT_TRUE(config.loadString(R"(
        dragon.set{
            wallpaper = { path = "~/bg.png", mode = "tile" },
            windows = {
                opacity = { fullscreen = 0.9 },
                glow = { enabled = true, size = 20, active = "#ff00ff80" },
                blur = { passes = 4, xray = true },
                dim = { strength = 0.3, color = "#101010" },
                shadow = { power = 3, inactive_color = "#00000020" },
            },
        }
        dragon.rule({ app_id = "mpv" }, { blur = false, dim = 0 })
        dragon.layer_rule({ namespace = "waybar" }, { blur = { size = 4 }, ignore_alpha = 0.2, opacity = 0.8 })
        dragon.layer_rule({ namespace = "wofi" }, { blur = true, corner_radius = 12, shadow = true })
    )")) << config.lastError();
    const auto& s = config.settings();
    EXPECT_EQ(s.wallpaper.path, std::string(std::getenv("HOME")) + "/bg.png");
    EXPECT_EQ(s.wallpaper.mode, "tile");
    EXPECT_FLOAT_EQ(s.windows.fullscreenOpacity, 0.9);
    EXPECT_TRUE(s.windows.glow.enabled);
    EXPECT_EQ(s.windows.glow.size, 20);
    EXPECT_EQ(s.windows.blur.passes, 4);
    EXPECT_TRUE(s.windows.blur.xray);
    EXPECT_FLOAT_EQ(s.windows.dim.strength, 0.3);
    EXPECT_FLOAT_EQ(s.windows.shadow.power, 3);
    EXPECT_TRUE(s.windows.shadow.inactiveColor);

    const auto mpv = config.styleFor("mpv", "");
    EXPECT_FALSE(mpv.blur.enabled);
    EXPECT_FLOAT_EQ(mpv.dim.strength, 0);

    const auto bar = config.layerStyleFor("waybar");
    EXPECT_TRUE(bar.blur.enabled) << "a blur table turns it on";
    EXPECT_EQ(bar.blur.size, 4);
    EXPECT_FLOAT_EQ(bar.ignoreAlpha, 0.2);
    EXPECT_FLOAT_EQ(bar.opacity, 0.8);
    EXPECT_EQ(config.layerStyleFor("wofi").cornerRadius, 12);
    EXPECT_TRUE(config.layerStyleFor("wofi").shadow.enabled);
    EXPECT_FALSE(config.layerStyleFor("mako").blur.enabled) << "layers are untouched without a rule";

    EXPECT_FALSE(config.loadString(R"(dragon.layer_rule({ app_id = "x" }, { blur = true }))"));
    EXPECT_FALSE(config.loadString(R"(dragon.set{ wallpaper = { mode = "zoom" } })"));
    EXPECT_FALSE(config.loadString(R"(dragon.set{ windows = { blur = { passes = 9 } } })"));
}

