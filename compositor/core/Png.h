#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace dragon::png {

// Writes 8-bit RGB pixels (`width * 3` bytes per row) as a PNG. Uncompressed
// (stored deflate blocks): no zlib dependency, fine for debug screenshots.
bool writeRgb(const std::string& path, std::uint32_t width, std::uint32_t height,
              std::span<const std::uint8_t> rgb);

} // namespace dragon::png
