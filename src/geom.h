#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>


struct Point { 
    int32_t x, y; 
};

struct Rect { 
    int32_t x1, y1, x2, y2; 
};

struct BVHNode {
    Rect bbox;
    int32_t offset; // if count > 0, index into rects. else, index of right child.
    int32_t count;  // 0 if internal
};

struct DSU {
    std::vector<int> p;
    DSU() = default;
    void reset(size_t n) { p.resize(n); std::iota(p.begin(), p.end(), 0); }
    int find(int i) {
        int root = i;
        while (p[root] != root) root = p[root];
        while (p[i] != root) { int next = p[i]; p[i] = root; i = next; }
        return root;
    }
    void unite(int i, int j) {
        int ri = find(i), rj = find(j);
        if (ri != rj) p[ri] = rj;
    }
    uint32_t count() const {
        uint32_t c = 0;
        for (int i = 0; i < (int)p.size(); ++i) if (p[i] == i) c++;
        return c;
    }
};

template <typename T>
void unique_sort(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x1 < b.x2 && a.x2 > b.x1 && a.y1 < b.y2 && a.y2 > b.y1;
}

inline bool contains(const Rect& a, const Rect& b) {
    return a.x1 <= b.x1 && a.y1 <= b.y1 && a.x2 >= b.x2 && a.y2 >= b.y2;
}

inline bool touches(const Rect& a, const Rect& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

inline void fracture(const std::vector<Point>& points, std::vector<Rect>& out) {
    if (points.size() < 4) return;

    // Fast track for simple rectangles
    if (points.size() == 4 || (points.size() == 5 && points[0].x == points[4].x && points[0].y == points[4].y)) {
        int32_t x1 = points[0].x, x2 = points[0].x;
        int32_t y1 = points[0].y, y2 = points[0].y;
        for (size_t i = 1; i < 4; ++i) {
            const auto& p = points[i];
            x1 = std::min(x1, p.x);
            y1 = std::min(y1, p.y);
            x2 = std::max(x2, p.x);
            y2 = std::max(y2, p.y);
        }
        out.push_back({x1, y1, x2, y2});
        return;
    }
    
    // Collect all unique Y coordinates
    std::vector<int32_t> ys;
    for (const auto& p : points) ys.push_back(p.y);
    unique_sort(ys);

    std::vector<Rect> activeRects;

    // For each horizontal strip [ys[i], ys[i+1]]
    for (size_t i = 0; i + 1 < ys.size(); ++i) {
        int32_t yLow = ys[i];
        int32_t yHigh = ys[i+1];
        
        // Find all vertical segments that cross this strip
        std::vector<int32_t> xs;
        for (size_t j = 0; j < points.size(); ++j) {
            const auto& p1 = points[j];
            const auto& p2 = points[(j + 1) % points.size()];
            if (p1.x == p2.x) { // Vertical segment
                if (std::min(p1.y, p2.y) <= yLow && std::max(p1.y, p2.y) >= yHigh) {
                    xs.push_back(p1.x);
                }
            }
        }
        std::sort(xs.begin(), xs.end());
        
        std::vector<Rect> nextActiveRects;
        size_t u = 0; // index for activeRects
        size_t v = 0; // index for xs (processed in pairs)

        while (u < activeRects.size() || v + 1 < xs.size()) {
            int32_t cx1 = (v + 1 < xs.size()) ? xs[v] : 2147483647;
            int32_t cx2 = (v + 1 < xs.size()) ? xs[v+1] : 2147483647;

            // Check if activeRects[u] matches current interval [cx1, cx2]
            if (u < activeRects.size()) {
                const auto& r = activeRects[u];
                
                if (cx1 == r.x1 && cx2 == r.x2) {
                    // Match! Extend
                    activeRects[u].y2 = yHigh;
                    nextActiveRects.push_back(activeRects[u]);
                    u++;
                    v += 2;
                } else if (r.x1 < cx1 || (r.x1 == cx1 && r.x2 < cx2)) {
                    // Active rect ends here (lexicographically smaller, or just disjoint)
                    out.push_back(r);
                    u++;
                } else {
                    // New interval starts here
                    nextActiveRects.push_back({cx1, yLow, cx2, yHigh});
                    v += 2;
                }
            } else {
                // No more active rects, just add new intervals
                nextActiveRects.push_back({cx1, yLow, cx2, yHigh});
                v += 2;
            }
        }
        activeRects = std::move(nextActiveRects);
    }
    
    // Flush remaining
    for (const auto& r : activeRects) out.push_back(r);
}

inline void convertPathToRects(const std::vector<Point>& points, int32_t width, int16_t type, std::vector<Rect>& out) {
    if (points.size() < 2) return;
    int32_t h = width / 2;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        int32_t x1 = points[i].x, y1 = points[i].y;
        int32_t x2 = points[i+1].x, y2 = points[i+1].y;
        
        if (type == 2) { // Half-width extension
            if (i == 0) {
                if (x1 < x2) x1 -= h; else if (x1 > x2) x1 += h;
                else if (y1 < y2) y1 -= h; else if (y1 > y2) y1 += h;
            }
            if (i + 2 == points.size()) {
                if (x2 > x1) x2 += h; else if (x2 < x1) x2 -= h;
                else if (y2 > y1) y2 += h; else if (y2 < y1) y2 -= h;
            }
        }

        int32_t rx1 = std::min(x1, x2), rx2 = std::max(x1, x2);
        int32_t ry1 = std::min(y1, y2), ry2 = std::max(y1, y2);
        if (x1 == x2) { rx1 -= h; rx2 += h; }
        else if (y1 == y2) { ry1 -= h; ry2 += h; }
        
        out.push_back({rx1, ry1, rx2, ry2});
    }
}

inline bool getIntersection(const Rect& a, const Rect& b, Rect& out) {
    out.x1 = std::max(a.x1, b.x1);
    out.y1 = std::max(a.y1, b.y1);
    out.x2 = std::min(a.x2, b.x2);
    out.y2 = std::min(a.y2, b.y2);
    return overlaps(a, b);
}

inline void subtractRect(const Rect& a, const Rect& b, std::vector<Rect>& results) {
    Rect inter;
    if (!getIntersection(a, b, inter)) {
        results.push_back(a);
        return;
    }
    if (a.y1 < inter.y1) results.push_back({a.x1, a.y1, a.x2, inter.y1});
    if (a.y2 > inter.y2) results.push_back({a.x1, inter.y2, a.x2, a.y2});
    if (a.x1 < inter.x1) results.push_back({a.x1, inter.y1, inter.x1, inter.y2});
    if (a.x2 > inter.x2) results.push_back({inter.x2, inter.y1, a.x2, inter.y2});
}

template<class Rect>
void buildBVHRecursive(std::vector<Rect>& rects, int first, int count, std::vector<BVHNode>& nodes, int nodeIdx) {
    int32_t minX = 2147483647, minY = 2147483647, maxX = -2147483648, maxY = -2147483648;
    for (int i = 0; i < count; ++i) {
        const auto& r = rects[first + i];
        minX = std::min({minX, r.x1, r.x2});
        minY = std::min({minY, r.y1, r.y2});
        maxX = std::max({maxX, r.x1, r.x2});
        maxY = std::max({maxY, r.y1, r.y2});
    }
    
    nodes[nodeIdx].bbox = {minX, minY, maxX, maxY};

    if (count <= 4) {
        nodes[nodeIdx].offset = first;
        nodes[nodeIdx].count = count;
    } else {
        int32_t dx = maxX - minX;
        int32_t dy = maxY - minY;
        int axis = (dx > dy) ? 0 : 1;
        int32_t mid = (axis == 0) ? (minX + maxX) / 2 : (minY + maxY) / 2;

        auto it = std::partition(rects.begin() + first, rects.begin() + first + count, [&](const Rect& r) {
            int32_t center = (axis == 0) ? (r.x1 + r.x2) / 2 : (r.y1 + r.y2) / 2;
            return center < mid;
        });
        
        int leftCount = std::distance(rects.begin() + first, it);
        if (leftCount == 0 || leftCount == count) leftCount = count / 2;

        int leftIdx = (int)nodes.size();
        nodes.emplace_back(); // left child
        nodes.emplace_back(); // right child
        nodes[nodeIdx].count = 0;
        nodes[nodeIdx].offset = leftIdx;

        buildBVHRecursive(rects, first, leftCount, nodes, leftIdx);
        buildBVHRecursive(rects, first + leftCount, count - leftCount, nodes, leftIdx + 1);
    }
}

template<class Rect>
void buildLayerBVH(std::vector<Rect>& allRects, uint32_t start, uint32_t count, std::vector<BVHNode>& nodes) {
    if (count == 0) {
        nodes.clear();
        return;
    }
    nodes.clear();
    nodes.reserve(count * 2);
    nodes.emplace_back(); // root
    buildBVHRecursive(allRects, (int)start, (int)count, nodes, 0);
}

template<typename F, typename Visitor, typename Rect>
void queryBVHRecursive(const std::vector<BVHNode>& nodes, int nodeIdx, const std::vector<Rect>& allRects, const Rect& q, Visitor& visitor, F pred) {
    const auto& node = nodes[nodeIdx];
    if (!pred(node.bbox, q)) return;

    if (node.count > 0) { // Leaf
        for (int i = 0; i < node.count; ++i) {
            const auto& r = allRects[node.offset + i];
            if (pred(r, q)) {
                visitor(node.offset + i);
            }
        }
    } else { // Internal
        queryBVHRecursive(nodes, node.offset, allRects, q, visitor, pred);
        queryBVHRecursive(nodes, node.offset + 1, allRects, q, visitor, pred);
    }
}

template<typename F, typename Visitor, typename Rect>
void queryBVH(const std::vector<BVHNode>& nodes, const std::vector<Rect>& allRects, const Rect& q, Visitor visitor, F pred) {
    if (nodes.empty()) return;
    queryBVHRecursive(nodes, 0, allRects, q, visitor, pred);
}

template<typename Rect>
int optimizeRects(std::vector<Rect> & input, std::vector<Rect> & output) {
    if (input.empty()) return 0;

    std::vector<BVHNode> nodes;
    std::vector<uint8_t> discarded(input.size(), 0);
    buildLayerBVH(input, 0, (uint32_t)input.size(), nodes);

    int totalDiscarded = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        queryBVH(nodes, input, input[i], [&](int j) {
            if (i == (size_t)j || discarded[j]) return;
            if (contains(input[j], input[i])) discarded[i] = true;
        }, overlaps);
        if (discarded[i]) totalDiscarded++;
    }

    for (size_t i = 0; i < input.size(); ++i) {
        if (!discarded[i]) output.push_back(input[i]);
    }
    return totalDiscarded;
}