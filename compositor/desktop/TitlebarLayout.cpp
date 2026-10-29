#include "desktop/Titlebar.h"

#include <algorithm>

namespace dragon {

TitlebarLayout layoutTitlebar(const TitlebarStyle& style, std::int32_t width)
{
    TitlebarLayout layout;
    const std::int32_t size = std::min(style.buttonSize, style.height);
    const std::int32_t y = (style.height - size) / 2;
    const auto n = std::int32_t(style.buttons.size());
    const std::int32_t used = n > 0 ? n * size + (n - 1) * style.buttonSpacing + style.padding : 0;

    // Buttons keep their listed order left to right, on either side.
    std::int32_t x = style.buttonsSide == "left" ? style.padding : width - style.padding - (used - style.padding);
    for (const auto& action : style.buttons) {
        layout.buttons.push_back({action, {x, y, size, size}});
        x += size + style.buttonSpacing;
    }

    const std::int32_t gap = style.padding;
    std::int32_t left = style.padding, right = style.padding;
    (style.buttonsSide == "left" ? left : right) += used > 0 ? used - style.padding + gap : 0;
    if (style.align == "center")
        left = right = std::max(left, right); // centered on the window, not the leftover space
    layout.text = {left, 0, std::max(width - left - right, 0), style.height};
    return layout;
}

} // namespace dragon
