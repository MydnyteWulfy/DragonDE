#include "core/Png.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace dragon::png {

namespace {

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0xffffffffu)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    for (std::uint8_t b : data)
        crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.insert(out.end(), {std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)});
}

void chunk(std::vector<std::uint8_t>& out, const char* type, std::span<const std::uint8_t> data)
{
    put32(out, std::uint32_t(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32({out.data() + start, out.size() - start}) ^ 0xffffffffu);
}

} // namespace

bool writeRgb(const std::string& path, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb)
{
    // Raw scanlines: filter byte 0 (none) + RGB.
    const std::size_t rowBytes = std::size_t(width) * 3;
    std::vector<std::uint8_t> raw;
    raw.reserve((rowBytes + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto* row = rgb.data() + y * rowBytes;
        raw.insert(raw.end(), row, row + rowBytes);
    }

    // zlib stream of stored blocks (max 65535 bytes each) + Adler-32.
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;) {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.insert(z.end(), {std::uint8_t(len), std::uint8_t(len >> 8), std::uint8_t(~len), std::uint8_t(~len >> 8)});
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(pos), raw.begin() + std::ptrdiff_t(pos + len));
        pos += len;
        if (last)
            break;
    }
    put32(z, (b << 16) | a);

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32(ihdr, width);
    put32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit, truecolor RGB, deflate, no filter, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

} // namespace dragon::png
