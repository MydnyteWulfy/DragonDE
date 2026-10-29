#include "config/Settings.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <xkbcommon/xkbcommon.h>

namespace dragon {

namespace {

std::string lower(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

} // namespace

std::optional<Color> Color::parse(std::string_view text)
{
    if (text.empty() || text[0] != '#')
        return std::nullopt;
    text.remove_prefix(1);
    std::string hex;
    if (text.size() == 3) {
        for (char c : text)
            hex += {c, c};
    } else if (text.size() == 6 || text.size() == 8) {
        hex = text;
    } else {
        return std::nullopt;
    }
    if (hex.size() == 6)
        hex += "ff";
    float channels[4];
    for (int i = 0; i < 4; ++i) {
        const int hi = hexDigit(hex[std::size_t(i) * 2]), lo = hexDigit(hex[std::size_t(i) * 2 + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        channels[i] = float(hi * 16 + lo) / 255.0f;
    }
    return Color{channels[0], channels[1], channels[2], channels[3]};
}

std::uint32_t modifierBit(std::string_view name)
{
    const auto n = lower(name);
    if (n == "super" || n == "logo" || n == "win" || n == "meta")
        return mods::Super;
    if (n == "alt")
        return mods::Alt;
    if (n == "ctrl" || n == "control")
        return mods::Ctrl;
    if (n == "shift")
        return mods::Shift;
    if (n == "mod")
        return mods::Mod;
    return 0;
}

std::optional<KeyCombo> KeyCombo::parse(std::string_view text)
{
    // "A+B+key"; the key may itself be "+" ("Ctrl++").
    std::string key;
    if (text.ends_with("++")) {
        key = "+";
        text.remove_suffix(2);
    } else {
        const auto plus = text.rfind('+');
        key = plus == std::string_view::npos ? text : text.substr(plus + 1);
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(0, plus);
    }
    if (key.empty())
        return std::nullopt;

    KeyCombo combo;
    while (!text.empty()) {
        const auto plus = text.find('+');
        const std::uint32_t bit = modifierBit(text.substr(0, plus));
        if (!bit)
            return std::nullopt;
        combo.modifiers |= bit;
        text = plus == std::string_view::npos ? std::string_view{} : text.substr(plus + 1);
    }

    static const std::pair<std::string_view, std::string_view> aliases[] = {
        {"enter", "Return"}, {"esc", "Escape"}, {"del", "Delete"}, {"backspace", "BackSpace"},
        {"pgup", "Prior"},   {"pgdn", "Next"},  {"+", "plus"},     {"space", "space"},
    };
    std::string name = key;
    for (const auto& [alias, real] : aliases) {
        if (lower(key) == alias)
            name = real;
    }
    const xkb_keysym_t sym = xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol)
        return std::nullopt;
    combo.keysym = xkb_keysym_to_lower(sym);
    return combo;
}

KeyCombo KeyCombo::resolved(std::uint32_t modBit) const
{
    KeyCombo c = *this;
    if (c.modifiers & mods::Mod) {
        c.modifiers &= ~mods::Mod;
        c.modifiers |= modBit;
    }
    return c;
}

std::string KeyCombo::toString() const
{
    std::string out;
    for (auto [bit, name] : {std::pair{mods::Mod, "Mod"}, std::pair{mods::Super, "Super"},
                             std::pair{mods::Ctrl, "Ctrl"}, std::pair{mods::Alt, "Alt"},
                             std::pair{mods::Shift, "Shift"}}) {
        if (modifiers & bit)
            out += std::format("{}+", name);
    }
    char buf[64] = {};
    xkb_keysym_get_name(keysym, buf, sizeof(buf));
    return out + buf;
}

bool globMatch(std::string_view pattern, std::string_view text)
{
    auto lower = [](char c) { return char(std::tolower(static_cast<unsigned char>(c))); };
    // Iterative with backtracking to the last '*'.
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(text[t]))) {
            ++p, ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

} // namespace dragon
