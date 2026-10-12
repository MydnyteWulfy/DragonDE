#pragma once

#include "core/Geometry.h"

#include <climits>
#include <pixman.h>
#include <vector>

namespace dragon {

// A set of non-overlapping rectangles (pixman region): damage, opaque and
// input regions.
class Region {
public:
    Region() { pixman_region32_init(&m_region); }
    explicit Region(Rect r)
    {
        if (r.empty())
            pixman_region32_init(&m_region);
        else
            pixman_region32_init_rect(&m_region, r.x, r.y, unsigned(r.width), unsigned(r.height));
    }
    ~Region() { pixman_region32_fini(&m_region); }

    Region(const Region& other) : Region() { pixman_region32_copy(&m_region, other.raw()); }
    Region& operator=(const Region& other)
    {
        if (this != &other)
            pixman_region32_copy(&m_region, other.raw());
        return *this;
    }

    // Covers every reachable coordinate (the default input region).
    static Region infinite() { return Region(Rect{INT32_MIN / 2, INT32_MIN / 2, INT32_MAX, INT32_MAX}); }

    bool empty() const { return !pixman_region32_not_empty(raw()); }
    void clear() { pixman_region32_clear(&m_region); }

    void add(Rect r)
    {
        if (!r.empty())
            pixman_region32_union_rect(&m_region, &m_region, r.x, r.y, unsigned(r.width), unsigned(r.height));
    }
    void subtract(Rect r)
    {
        Region tmp(r);
        pixman_region32_subtract(&m_region, &m_region, tmp.raw());
    }
    void unite(const Region& other) { pixman_region32_union(&m_region, &m_region, other.raw()); }
    void intersect(const Region& other) { pixman_region32_intersect(&m_region, &m_region, other.raw()); }
    void intersect(Rect r) { intersect(Region(r)); }
    void translate(Point d) { pixman_region32_translate(&m_region, d.x, d.y); }

    bool contains(Point p) const { return pixman_region32_contains_point(raw(), p.x, p.y, nullptr); }

    Rect extents() const
    {
        const auto* b = pixman_region32_extents(raw());
        return {b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1};
    }

    std::vector<Rect> rects() const
    {
        int n = 0;
        const auto* boxes = pixman_region32_rectangles(raw(), &n);
        std::vector<Rect> out;
        out.reserve(std::size_t(n));
        for (int i = 0; i < n; ++i)
            out.push_back({boxes[i].x1, boxes[i].y1, boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1});
        return out;
    }

    friend bool operator==(const Region& a, const Region& b) { return pixman_region32_equal(a.raw(), b.raw()); }

    // pixman's API isn't const-correct; reads through this are safe.
    pixman_region32_t* raw() const { return const_cast<pixman_region32_t*>(&m_region); }

private:
    pixman_region32_t m_region;
};

} // namespace dragon
