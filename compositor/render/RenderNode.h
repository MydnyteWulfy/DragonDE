#pragma once

#include "core/UniqueFd.h"

#include <optional>
#include <string>
#include <sys/types.h>

namespace dragon {

// A DRM render node (/dev/dri/renderD*): unprivileged GPU access for
// allocation and rendering, no modesetting.
struct RenderNode {
    UniqueFd fd;
    dev_t device = 0; // used to match the Vulkan physical device
    std::string path;
};

// Opens the render node of the GPU that `preferred` (any of its DRM nodes)
// belongs to, e.g. the parent compositor's main device; otherwise the first
// render node found.
std::optional<RenderNode> openRenderNode(std::optional<dev_t> preferred = std::nullopt);

} // namespace dragon
