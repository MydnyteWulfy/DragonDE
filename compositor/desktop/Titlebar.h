#pragma once

#include "config/Settings.h"
#include "core/Geometry.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dragon {

// Where things go in a title bar of `width` x style.height. Shared by
// drawing and hit testing so they can never disagree. Rects are relative to
// the title bar's top-left.
struct TitlebarLayout {
    struct Button {
        std::string action; // minimize | maximize | close
        Rect rect;
    };
    std::vector<Button> buttons;
    Rect text; // where the title may go (already clear of the buttons)
};

TitlebarLayout layoutTitlebar(const TitlebarStyle& style, std::int32_t width);

namespace vk {
class VulkanRenderer;
class VulkanTexture;
} // namespace vk

// Draws title bars (background, title, buttons) on the CPU with Cairo and
// Pango and keeps each one as a texture, redrawn only when something it
// shows changes. Rounded corners come later, from the GPU clip.
class TitlebarRenderer {
public:
    explicit TitlebarRenderer(vk::VulkanRenderer& renderer);
    ~TitlebarRenderer();
    TitlebarRenderer(const TitlebarRenderer&) = delete;
    TitlebarRenderer& operator=(const TitlebarRenderer&) = delete;

    struct State {
        std::int32_t width = 0;
        std::string title;
        bool active = false;
        bool maximized = false;
        int hovered = -1, pressed = -1;
        TitlebarStyle style;
        friend bool operator==(const State&, const State&) = default;
    };

    // The texture for `key`'s title bar looking like `state`.
    vk::VulkanTexture* get(const void* key, const State& state);
    // Frees title bars of windows not drawn since the last call.
    void sweep();

private:
    struct Entry;
    vk::VulkanRenderer& m_renderer;
    std::unordered_map<const void*, std::unique_ptr<Entry>> m_entries;
};

} // namespace dragon
