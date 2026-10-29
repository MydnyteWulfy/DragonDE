#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dragon {

// sRGB color, 0..1 per channel.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    // "#rgb", "#rrggbb" or "#rrggbbaa".
    static std::optional<Color> parse(std::string_view text);
    std::array<float, 4> rgba() const { return {r, g, b, a}; }
    friend bool operator==(const Color&, const Color&) = default;
};

struct KeyboardSettings {
    // XKB names; empty: the system default (or, nested, the parent's keymap).
    std::string layout, variant, options, model;
    std::int32_t repeatRate = 25;   // keys per second
    std::int32_t repeatDelay = 600; // ms
};

struct TitlebarColors {
    Color background{0.16f, 0.17f, 0.21f};
    Color text{0.90f, 0.91f, 0.94f};
    Color button{0.75f, 0.77f, 0.82f};      // icons (or the circles with button_style = "circle")
    Color buttonHover{0.30f, 0.32f, 0.38f}; // behind a hovered icon
    Color closeHover{0.85f, 0.25f, 0.25f};
    friend bool operator==(const TitlebarColors&, const TitlebarColors&) = default;
};

// The title bar DragonDE draws for apps that don't draw their own.
struct TitlebarStyle {
    bool enabled = true;
    std::int32_t height = 30;
    std::string font = "sans-serif";
    double fontSize = 10.5; // points
    bool bold = true;
    std::string align = "center"; // left | center | right
    std::vector<std::string> buttons{"minimize", "maximize", "close"}; // in order
    std::string buttonsSide = "right";  // left | right
    std::string buttonStyle = "icon";   // icon | circle
    std::int32_t buttonSize = 18;
    std::int32_t buttonSpacing = 6;
    std::int32_t padding = 8;           // between the edges and the buttons/title
    TitlebarColors active;
    TitlebarColors inactive{.background{0.12f, 0.13f, 0.16f}, .text{0.55f, 0.57f, 0.62f},
                            .button{0.50f, 0.52f, 0.57f}, .buttonHover{0.24f, 0.26f, 0.31f},
                            .closeHover{0.70f, 0.22f, 0.22f}};
    friend bool operator==(const TitlebarStyle&, const TitlebarStyle&) = default;
};

// A solid color, or a gradient from `color` to `end` along `angle`.
struct Paint {
    Color color;
    std::optional<Color> end;
    friend bool operator==(const Paint&, const Paint&) = default;
};

struct BorderStyle {
    std::int32_t width = 2; // outside the window
    Paint active{{0.37f, 0.51f, 0.67f}};
    Paint inactive{{0.23f, 0.26f, 0.32f}};
    double gradientAngle = 45; // degrees, 0 = left to right
    friend bool operator==(const BorderStyle&, const BorderStyle&) = default;
};

struct ShadowStyle {
    bool enabled = true;
    std::int32_t size = 20; // blur distance
    Color color{0, 0, 0, 0.45f};
    std::optional<Color> inactiveColor; // unfocused windows (default: color)
    double power = 2.0;     // falloff: higher is a tighter, harder shadow
    std::int32_t offsetX = 0, offsetY = 4;
    friend bool operator==(const ShadowStyle&, const ShadowStyle&) = default;
};

// A colored halo around the window (a shadow without an offset, in color).
struct GlowStyle {
    bool enabled = false;
    std::int32_t size = 14;
    Color active{0.53f, 0.75f, 0.82f, 0.55f};
    Color inactive{0, 0, 0, 0};
    double power = 2.5;
    friend bool operator==(const GlowStyle&, const GlowStyle&) = default;
};

// Frosted glass: what's behind a translucent window, blurred (dual Kawase).
struct BlurStyle {
    bool enabled = true;
    std::int32_t size = 8;   // spread per pass
    std::int32_t passes = 2; // 1-6: each doubles the reach
    double noise = 0.0117;   // grain, hides banding
    double contrast = 0.8916;
    double brightness = 0.8172;
    double vibrancy = 0.1696; // saturation boost
    bool xray = false;       // blur only the wallpaper and bottom layers (cheaper, sees through windows)
    friend bool operator==(const BlurStyle&, const BlurStyle&) = default;
};

// Darkening for windows without focus.
struct DimStyle {
    double strength = 0;  // 0 (off) to 1
    Color color{0, 0, 0};
    friend bool operator==(const DimStyle&, const DimStyle&) = default;
};

// How a window looks. Applies to every window, whether DragonDE draws its
// title bar or the app draws its own (then the window is clipped to its
// rounded shape and keeps the border, shadow and opacity).
struct WindowStyle {
    std::int32_t cornerRadius = 10;
    BorderStyle border;
    ShadowStyle shadow;
    GlowStyle glow;
    BlurStyle blur;
    DimStyle dim;
    double activeOpacity = 1.0, inactiveOpacity = 1.0, fullscreenOpacity = 1.0;
    TitlebarStyle titlebar;

    // Nothing at all: windows drawn exactly as the client made them.
    static WindowStyle plain()
    {
        WindowStyle s;
        s.cornerRadius = 0;
        s.border.width = 0;
        s.shadow.enabled = false;
        s.blur.enabled = false;
        s.titlebar.enabled = false;
        return s;
    }
    // Does the compositor shape this window (clip, border, shadow, title bar)?
    bool styled() const
    {
        return cornerRadius > 0 || border.width > 0 || shadow.enabled || glow.enabled || titlebar.enabled ||
               dim.strength > 0 || activeOpacity < 1 || inactiveOpacity < 1;
    }
    friend bool operator==(const WindowStyle&, const WindowStyle&) = default;
};

// How layer surfaces (bars, launchers, notifications) are drawn: off by
// default, turned on per namespace with dragon.layer_rule.
struct LayerStyle {
    BlurStyle blur{.enabled = false};
    double opacity = 1.0;
    // Blur only where the surface's alpha is above this (0: its whole
    // rectangle). Lets a bar with transparent gaps blur just its pieces.
    double ignoreAlpha = 0;
    std::int32_t cornerRadius = 0;
    ShadowStyle shadow{.enabled = false};
    friend bool operator==(const LayerStyle&, const LayerStyle&) = default;
};

struct WallpaperSettings {
    std::string path;            // empty: just the background color
    std::string mode = "fill";   // fill | fit | center | stretch | tile
    friend bool operator==(const WallpaperSettings&, const WallpaperSettings&) = default;
};

struct Settings {
    // super | alt | ctrl: the "Mod" in shortcuts and Mod+drag. auto: Super,
    // except Alt when nested (the parent desktop keeps Super shortcuts).
    std::string modifier = "auto";
    std::string terminal = "kitty";
    Color background{0.08f, 0.09f, 0.12f};
    WallpaperSettings wallpaper;
    KeyboardSettings keyboard;
    WindowStyle windows;
};

// Modifier bits for shortcuts.
namespace mods {
constexpr std::uint32_t Shift = 1 << 0;
constexpr std::uint32_t Ctrl = 1 << 1;
constexpr std::uint32_t Alt = 1 << 2;
constexpr std::uint32_t Super = 1 << 3;
// The configurable "Mod" (resolved through Settings::modifier when matching).
constexpr std::uint32_t Mod = 1 << 4;
} // namespace mods

// A shortcut: modifiers + a key, by its unshifted symbol in the active layout.
struct KeyCombo {
    std::uint32_t modifiers = 0;
    std::uint32_t keysym = 0; // lowercase xkb keysym

    // "Mod+Shift+Return", "Ctrl+Alt+t", "Super+F11" (case-insensitive).
    static std::optional<KeyCombo> parse(std::string_view text);
    std::string toString() const;
    // With "Mod" replaced by the real modifier.
    KeyCombo resolved(std::uint32_t modBit) const;
    friend auto operator<=>(const KeyCombo&, const KeyCombo&) = default;
};

// Shell-style match, ignoring case: '*' any run of characters, '?' any one.
bool globMatch(std::string_view pattern, std::string_view text);

// The modifier bit for Settings::modifier ("super", "alt", "ctrl"); 0 if unknown.
std::uint32_t modifierBit(std::string_view name);

} // namespace dragon
