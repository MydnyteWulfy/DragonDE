#pragma once

#include <cstdint>

namespace dragon {

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend constexpr bool operator==(Point, Point) = default;
    constexpr Point operator+(Point o) const { return {x + o.x, y + o.y}; }
    constexpr Point operator-(Point o) const { return {x - o.x, y - o.y}; }
};

// Sub-pixel position (pointer coordinates).
struct PointF {
    double x = 0.0;
    double y = 0.0;

    friend constexpr bool operator==(PointF, PointF) = default;
    constexpr PointF operator+(PointF o) const { return {x + o.x, y + o.y}; }
    constexpr PointF operator-(PointF o) const { return {x - o.x, y - o.y}; }
    constexpr PointF operator-(Point o) const { return {x - o.x, y - o.y}; }
};

struct Size {
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend constexpr bool operator==(Size, Size) = default;
    constexpr bool empty() const { return width <= 0 || height <= 0; }
};

struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend constexpr bool operator==(Rect, Rect) = default;
    constexpr bool empty() const { return width <= 0 || height <= 0; }
    constexpr Point position() const { return {x, y}; }
    constexpr Size size() const { return {width, height}; }
    constexpr bool contains(Point p) const { return p.x >= x && p.y >= y && p.x < x + width && p.y < y + height; }
};

} // namespace dragon
