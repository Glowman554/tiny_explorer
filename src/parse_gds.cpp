// g++ -std=c++17 -O3 parse_gds.cpp -o parse_gds && ./parse_gds gds/09_tt_um_znah_vga_ca.gds

#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <cmath>
#include <map>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <numeric>
#include <tuple>
#include <set>


#include "fetsim.h"

// MARK: - Data Structures

struct Point { 
    int32_t x, y; 
};

struct Rect { 
    int32_t x1, y1, x2, y2; 
};

struct RectWire : Rect { int wire; };
constexpr int RECT_WIRE_FIELDS = 5; // x1, y1, x2, y2, wire


static inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x1 < b.x2 && a.x2 > b.x1 && a.y1 < b.y2 && a.y2 > b.y1;
}

static inline bool contains(const Rect& a, const Rect& b) {
    return a.x1 <= b.x1 && a.y1 <= b.y1 && a.x2 >= b.x2 && a.y2 >= b.y2;
}

static inline bool touches(const Rect& a, const Rect& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

struct BVHNode {
    Rect bbox;
    int32_t offset; // if count > 0, index into rects. else, index of right child.
    int32_t count;  // 0 if internal
};

struct Reference {
    enum Type { SREF, AREF } type;
    std::string cellName;
    std::vector<Point> points; // insertion points
    int16_t cols = 1, rows = 1;
    bool reflection = false;
    double mag = 1.0;
    double angle = 0.0;
};

struct Text {
    int16_t layer = 0;
    int16_t texttype = 0;
    Point point;
    std::string content;
    bool reflection = false;
    double mag = 1.0;
    double angle = 0.0;
};

struct QuadLayer {
    uint32_t rectStart = 0;
    uint32_t rectCount = 0;
    std::vector<BVHNode> bvh;
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

enum LayerID {
    L_ERROR = 0,
    L_NWELL,
    L_DIFF,
    L_CHANNEL,  // created from L_DIFF & L_POLY
    L_TERMINAL, // created from L_DIFF - L_POLY
    L_N_TERM, L_P_TERM, // for flattened geom only
    L_POLY,     // FET Gates
    L_LICON,    // connects L_LI1 to both L_POLY and L_TERMINAL
    L_LI1,  L_MCON,
    L_MET1, L_VIA1,
    L_MET2, L_VIA2,
    L_MET3, L_VIA3,
    L_MET4, L_VIA4,
    L_MET5,
    L_COUNT
};

using GDSLayerID = std::pair<int16_t, int16_t>;

static LayerID getLayerID(int16_t layer, int16_t datatype) {
    static const std::map<std::pair<int16_t, int16_t>, LayerID> Map = {
        // Sky130
        {{64,20}, L_NWELL},
        {{65,20}, L_DIFF},
        {{66,20}, L_POLY}, {{66,44}, L_LICON},
        {{67,20}, L_LI1 }, {{67,44}, L_MCON},
        {{67, 5}, L_LI1},   // labels
        {{68,20}, L_MET1}, {{68,44}, L_VIA1},
        {{68, 5}, L_MET1},  // labels
        {{69,20}, L_MET2}, {{69,44}, L_VIA2},
        {{69, 5}, L_MET2},  // labels
        {{70,20}, L_MET3}, {{70,44}, L_VIA3},
        {{70, 5}, L_MET3},  // labels
        {{71,20}, L_MET4}, {{71,44}, L_VIA4},
        {{71, 5}, L_MET4},  // labels
        {{72,20}, L_MET5},

        // IHP130
        {{31, 0}, L_NWELL},
        {{ 1, 0}, L_DIFF},
        {{ 5, 0}, L_POLY}, {{ 6, 0}, L_LICON},
        {{ 8, 0}, L_LI1},  {{19, 0}, L_MCON},
        {{ 8, 1}, L_LI1},  // labels
        {{ 8, 2}, L_LI1},  // labels
        {{ 8,25}, L_LI1},  // labels
        {{10, 0}, L_MET1}, {{29, 0}, L_VIA1},
        {{10,25}, L_MET1}, // labels
        {{30, 0}, L_MET2}, {{49, 0}, L_VIA2},
        {{50, 0}, L_MET3}, {{66, 0}, L_VIA3},
        {{67, 0}, L_MET4}, 
        {{67,25}, L_MET4}  // labels
    };
    auto it = Map.find({layer, datatype});
    return (it != Map.end()) ? it->second : L_COUNT;
}

struct ChildConnection {
    int32_t parentIdx;
    int32_t childRefIdx;
    int32_t childIdx;
    auto tie() const { return std::tie(parentIdx, childRefIdx, childIdx); }
    bool operator<(const ChildConnection& o) const { return tie() < o.tie(); }
    bool operator==(const ChildConnection& o) const { return tie() == o.tie(); }
};

struct Cell {
    std::string name;
    std::vector<Rect> rects;
    std::array<QuadLayer, L_COUNT> layers; // Indexed by LayerID
    std::vector<Reference> references;
    std::vector<Text> texts;
    DSU wireDSU;
    std::map<std::string, int32_t> label2rect;
    std::vector<ChildConnection> connectionsR2R; // parent rect to child rect
    std::vector<ChildConnection> connectionsR2W; // parent rect to child wire
    std::vector<FET> fets;

    std::set<int32_t> bridgingRefs;
    std::vector<std::vector<Rect>> tempRects; // Temporary storage during parsing

    bool isProcessed = false;
    bool isTop = true;

    // Stats
    size_t flatRects = 0;
    size_t flatFETs = 0;
    size_t totalInstances = 0;

    // Special Nets
    int32_t groundRect = -1;
    int32_t powerRect = -1;
};


struct GdsLibrary {
    std::string name;
    double userUnit;
    double dbUnit;
    std::vector<Cell> cells;
    std::map<std::string, size_t> cellMap;
    std::string topCellName;
};


// MARK: - Geometry Helpers

template <typename T>
static void unique_sort(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

static void fracture(const std::vector<Point>& points, std::vector<Rect>& out) {
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

static void convertPathToRects(const std::vector<Point>& points, int32_t width, int16_t type, std::vector<Rect>& out) {
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

static bool getIntersection(const Rect& a, const Rect& b, Rect& out) {
    out.x1 = std::max(a.x1, b.x1);
    out.y1 = std::max(a.y1, b.y1);
    out.x2 = std::min(a.x2, b.x2);
    out.y2 = std::min(a.y2, b.y2);
    return overlaps(a, b);
}

static void subtractRect(const Rect& a, const Rect& b, std::vector<Rect>& results) {
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

// MARK: - BVH Builder

template<class Rect>
static void buildBVHRecursive(std::vector<Rect>& rects, int first, int count, std::vector<BVHNode>& nodes, int nodeIdx) {
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
static void buildLayerBVH(std::vector<Rect>& allRects, uint32_t start, uint32_t count, std::vector<BVHNode>& nodes) {
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
static void queryBVHRecursive(const std::vector<BVHNode>& nodes, int nodeIdx, const std::vector<Rect>& allRects, const Rect& q, Visitor& visitor, F pred) {
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
static void queryBVH(const std::vector<BVHNode>& nodes, const std::vector<Rect>& allRects, const Rect& q, Visitor visitor, F pred) {
    if (nodes.empty()) return;
    queryBVHRecursive(nodes, 0, allRects, q, visitor, pred);
}

// MARK: - Cell analysis

static void mergeCellWires(Cell& cell) {
    cell.wireDSU.reset(cell.rects.size());
    
    auto mergeLayers = [&](QuadLayer& ql1, QuadLayer& ql2) {
        if (ql1.rectCount == 0 || ql2.rectCount == 0) return;
        for (uint32_t i = 0; i < ql1.rectCount; ++i) {
            int idx1 = ql1.rectStart + i;
            queryBVH(ql2.bvh, cell.rects, cell.rects[idx1], [&](int idx2) {
                cell.wireDSU.unite(idx1, idx2);
            }, touches);
        }
    };

    // Intra-layer merging
    for (auto& layer : cell.layers) {
        mergeLayers(layer, layer);
    }

    // Inter-layer merging based on LayerStack (consecutive enum values)
    for (int i = L_POLY; i+1 <= L_MET5; ++i) {
        mergeLayers(cell.layers[i], cell.layers[i+1]);
    }
    mergeLayers(cell.layers[L_LICON], cell.layers[L_TERMINAL]);

    // Through-child merging
    if (!cell.connectionsR2W.empty()) {
        std::map<std::pair<int32_t, int32_t>, int32_t> childToParent;
        for (const auto& cw : cell.connectionsR2W) {
            auto key = std::make_pair(cw.childRefIdx, cw.childIdx);
            auto it = childToParent.find(key);
            if (it != childToParent.end()) {
                cell.wireDSU.unite(cw.parentIdx, it->second);
                cell.bridgingRefs.insert(cw.childRefIdx);
            } else {
                childToParent[key] = cw.parentIdx;
            }
        }
    }
}

static void extractFETs(Cell& cell) {
    const auto& channels = cell.layers[L_CHANNEL];
    if (channels.rectCount == 0) return;
    
    std::set<FET> uniqueFETs;
    const auto& nwellLayer = cell.layers[L_NWELL];
    const auto& gateLayer = cell.layers[L_POLY];
    const auto& termLayer = cell.layers[L_TERMINAL];
    size_t malformedCount = 0;

    for (uint32_t i = 0; i < channels.rectCount; ++i) {
        int idx = channels.rectStart + i;
        Rect channel = cell.rects[idx];

        // 1. Identify Type (N/P)
        bool isPType = false;
        // Check if overlaps NWELL
        if (nwellLayer.rectCount > 0) {
            queryBVH(nwellLayer.bvh, cell.rects, channel, [&](int) { isPType = true; }, overlaps);
        }

        // 2. Identify Gate
        int32_t gateWire = -1;
        {
            queryBVH(gateLayer.bvh, cell.rects, channel, [&](int idx) {
                if (gateWire == -1) gateWire = cell.wireDSU.find(idx);
            }, overlaps);
        }

        // 3. Identify Terminals
        std::set<int32_t> uniqueTerminals;
        queryBVH(termLayer.bvh, cell.rects, channel, [&](int idx) {
            uniqueTerminals.insert(cell.wireDSU.find(idx));
        }, touches);

        if (uniqueTerminals.size() != 2) {
             // Suppress warning if gate is tied to PWR/GND (Fill/Decap)
             int32_t gndRoot = (cell.groundRect != -1) ? cell.wireDSU.find(cell.groundRect) : -2;
             int32_t pwrRoot = (cell.powerRect != -1) ? cell.wireDSU.find(cell.powerRect) : -2;
             
             if (gateWire != -1 && (gateWire == gndRoot || gateWire == pwrRoot)) {
                 // Likely a fill/decap cell, ignore
             } else {
                 malformedCount++;
             }
             continue;
        }
        
        auto it = uniqueTerminals.begin();
        int32_t t1 = *it++;
        int32_t t2 = *it;

        FET fet;
        fet.type = isPType ? FET::P : FET::N;
        fet.gate = gateWire;
        fet.term[0] = t1;
        fet.term[1] = t2;
        
        uniqueFETs.insert(fet);
    }
    
    if (malformedCount > 0) {
        fprintf(stderr, "Warning: %zu malformed FETs in cell %s (skipped)\n", malformedCount, cell.name.c_str());
    }
    
    cell.fets.assign(uniqueFETs.begin(), uniqueFETs.end());
}

static void processFETLayers(Cell& cell) {
    const auto& diffLayer = cell.layers[L_DIFF];
    const auto& gateLayer = cell.layers[L_POLY];
    
    if (diffLayer.rectCount == 0 || gateLayer.rectCount == 0) return;

    std::vector<Rect> channels, terminals;
    for (uint32_t i = 0; i < diffLayer.rectCount; ++i) {
        Rect dr = cell.rects[diffLayer.rectStart + i];
        std::vector<int> overlaps_indices;
        queryBVH(gateLayer.bvh, cell.rects, dr, [&](int idx) { overlaps_indices.push_back(idx); }, overlaps);
        
        // Channels: Intersection
        for (int o : overlaps_indices) {
            Rect inter;
            if (getIntersection(dr, cell.rects[o], inter)) {
                channels.push_back(inter);
            }
        }
        
        // Terminals: Difference
        std::vector<Rect> current = {dr};
        for (int o : overlaps_indices) {
            std::vector<Rect> next;
            for (const auto& r : current) subtractRect(r, cell.rects[o], next);
            current = std::move(next);
        }
        terminals.insert(terminals.end(), current.begin(), current.end());
    }
    
    auto& chLayer = cell.layers[L_CHANNEL];
    chLayer.rectStart = (uint32_t)cell.rects.size();
    chLayer.rectCount = (uint32_t)channels.size();
    cell.rects.insert(cell.rects.end(), channels.begin(), channels.end());
    buildLayerBVH(cell.rects, chLayer.rectStart, chLayer.rectCount, chLayer.bvh);
    
    auto& termLayer = cell.layers[L_TERMINAL];
    termLayer.rectStart = (uint32_t)cell.rects.size();
    termLayer.rectCount = (uint32_t)terminals.size();
    cell.rects.insert(cell.rects.end(), terminals.begin(), terminals.end());
    buildLayerBVH(cell.rects, termLayer.rectStart, termLayer.rectCount, termLayer.bvh);
}

static void resolveLabels(Cell& cell) {
    const std::vector<std::string> gndLabels = {"GND", "VGND", "VSS", "gnd"};
    const std::vector<std::string> pwrLabels = {"PWR", "VPWR", "VDD", "pwr"};

    for (const auto& text : cell.texts) {
        LayerID targetLid = getLayerID(text.layer, text.texttype);
        if (targetLid != L_COUNT) {
            const auto& layer = cell.layers[targetLid];
            if (layer.rectCount > 0) {
                Rect pRect = {text.point.x, text.point.y, text.point.x, text.point.y};
                int32_t foundRectIdx = -1;
                
                queryBVH(layer.bvh, cell.rects, pRect, [&](int idx) {
                    if (foundRectIdx == -1) foundRectIdx = idx;
                }, touches);

                if (foundRectIdx != -1) {
                    if (cell.label2rect.count(text.content)) {
                        cell.wireDSU.unite(cell.label2rect[text.content], foundRectIdx);
                    } else {
                        cell.label2rect[text.content] = foundRectIdx;
                    }
                    
                    // Power/Ground Detection
                    for (const auto& l : gndLabels) {
                        if (text.content == l) { cell.groundRect = foundRectIdx; break; }
                    }
                    for (const auto& l : pwrLabels) {
                        if (text.content == l) { cell.powerRect = foundRectIdx; break; }
                    }
                }
            }
        }
    }
}

struct Transformer {
    double m00=1, m01=0, tx=0;
    double m10=0, m11=1, ty=0;

    Transformer() = default;

    Transformer(double angle, double mag, bool reflection) {
        double rad = angle * M_PI / 180.0;
        double c = cos(rad) * mag;
        double s = sin(rad) * mag;

        if (reflection) {
            m00 = c; m01 = s;
            m10 = s; m11 = -c;
        } else {
            m00 = c; m01 = -s;
            m10 = s; m11 = c;
        }
    }
    
    // Construct from raw matrix
    Transformer(double a, double b, double x, double c, double d, double y)
        : m00(a), m01(b), tx(x), m10(c), m11(d), ty(y) {}
    
    Transformer compose(const Reference& ref) const {
        Transformer child(ref.angle, ref.mag, ref.reflection);
        if (!ref.points.empty()) {
            child.tx = ref.points[0].x;
            child.ty = ref.points[0].y;
        }
        
        return Transformer(
            m00*child.m00 + m01*child.m10,
            m00*child.m01 + m01*child.m11,
            m00*child.tx  + m01*child.ty + tx,
            
            m10*child.m00 + m11*child.m10,
            m10*child.m01 + m11*child.m11,
            m10*child.tx  + m11*child.ty + ty
        );
    }

    void apply(double x, double y, double offX, double offY, double& ox, double& oy) const {
        ox = m00 * x + m01 * y + tx + offX;
        oy = m10 * x + m11 * y + ty + offY;
    }
    
    // Apply full accumulated transform to a Point
    // Point p' = M * p
    Point apply(int32_t x, int32_t y) const {
        return {
            (int32_t)round(m00 * x + m01 * y + tx),
            (int32_t)round(m10 * x + m11 * y + ty)
        };
    }

    // Apply internal transform + extra translation to a Rect (returns AABB)
    Rect applyToRect(const Rect& r, double offX, double offY) const {
        double corners[4][2] = {
            {(double)r.x1, (double)r.y1}, {(double)r.x2, (double)r.y1},
            {(double)r.x1, (double)r.y2}, {(double)r.x2, (double)r.y2}
        };
        
        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        
        for (int i = 0; i < 4; ++i) {
            double ox, oy;
            apply(corners[i][0], corners[i][1], offX, offY, ox, oy);
            if (ox < minX) minX = ox;
            if (ox > maxX) maxX = ox;
            if (oy < minY) minY = oy;
            if (oy > maxY) maxY = oy;
        }
        
        return {
            (int32_t)floor(minX), (int32_t)floor(minY),
            (int32_t)ceil(maxX),  (int32_t)ceil(maxY)
        };
    }
};

template<typename Func>
static void forEachRefInstance(const Reference& ref, Func func) {
    if (ref.type == Reference::SREF && !ref.points.empty()) {
        func(ref.points[0].x, ref.points[0].y);
    } else if (ref.type == Reference::AREF && ref.points.size() >= 3) {
        double dx1 = (double)(ref.points[1].x - ref.points[0].x) / ref.cols;
        double dy1 = (double)(ref.points[1].y - ref.points[0].y) / ref.cols;
        double dx2 = (double)(ref.points[2].x - ref.points[0].x) / ref.rows;
        double dy2 = (double)(ref.points[2].y - ref.points[0].y) / ref.rows;
        for (int r = 0; r < ref.rows; ++r) {
            for (int c = 0; c < ref.cols; ++c) {
                double ox = ref.points[0].x + c * dx1 + r * dx2;
                double oy = ref.points[0].y + c * dy1 + r * dy2;
                func(ox, oy);
            }
        }
    }
}

static void buildCellConnections(Cell& parent, GdsLibrary& lib) {
    for (size_t refIdx = 0; refIdx < parent.references.size(); ++refIdx) {
        const auto& ref = parent.references[refIdx];
        if (lib.cellMap.find(ref.cellName) == lib.cellMap.end()) continue;
        Cell& child = lib.cells[lib.cellMap[ref.cellName]];
        child.isTop = false;

        // Collect common layers
        std::vector<LayerID> commonLayers;
        for (int i = 0; i < L_COUNT; ++i) {
            if (parent.layers[i].rectCount > 0 && child.layers[i].rectCount > 0) {
                commonLayers.push_back((LayerID)i);
            }
        }

        Transformer trans(ref.angle, ref.mag, ref.reflection);

        forEachRefInstance(ref, [&](double tx, double ty) {
            for (LayerID lid : commonLayers) {
                const auto& gridParent = parent.layers[lid];
                const auto& gridChild = child.layers[lid];

                for (uint32_t i = 0; i < gridChild.rectCount; ++i) {
                    int32_t childIdx = gridChild.rectStart + i;
                    Rect tr = trans.applyToRect(child.rects[childIdx], tx, ty);

                    queryBVH(gridParent.bvh, parent.rects, tr, [&](int parentIdx) {
                        // Store rect connection
                        parent.connectionsR2R.push_back({(int32_t)parentIdx, (int32_t)refIdx, childIdx});
                        
                        // Resolve wires (parent wire will be resolved later in mergeCellWires)
                        int32_t childWire = child.wireDSU.find(childIdx);
                        parent.connectionsR2W.push_back({(int32_t)parentIdx, (int32_t)refIdx, childWire});
                    }, touches);
                }
            }
        });
    }
    
    // Remove duplicates and sort for both connections and wireConnections
    unique_sort(parent.connectionsR2R);
    unique_sort(parent.connectionsR2W);
}

static void computeCellStats(Cell& cell, const GdsLibrary& lib) {
    cell.flatRects = cell.rects.size();
    cell.flatFETs = cell.fets.size();
    cell.totalInstances = 0;
    
    for (const auto& ref : cell.references) {
        size_t count = ref.cols * ref.rows;
        cell.totalInstances += count;
        
        auto it = lib.cellMap.find(ref.cellName);
        if (it != lib.cellMap.end()) {
            const Cell& child = lib.cells[it->second];
            
            cell.flatRects += child.flatRects * count;
            cell.flatFETs += child.flatFETs * count;
            cell.totalInstances += child.totalInstances * count;
        }
    }
}

static void finishCellAnalysis(Cell& cell, GdsLibrary& lib) {
    buildCellConnections(cell, lib); 
    mergeCellWires(cell);
    resolveLabels(cell);
    extractFETs(cell);
    computeCellStats(cell, lib);
    cell.isProcessed = true;
    printf("Cell: %s (%zu quads, %u wires, %zu refs, %zu FETs)\n", 
        cell.name.c_str(), cell.rects.size(), cell.wireDSU.count(), 
        cell.references.size(), cell.fets.size());
}

static void processCell(Cell& cell, GdsLibrary& lib) {
    for (int i = 0; i < L_COUNT; i++) {
        if (cell.tempRects[i].empty()) continue;
        auto& layer = cell.layers[i];
        layer.rectStart = (uint32_t)cell.rects.size();
        layer.rectCount = (uint32_t)cell.tempRects[i].size();
        cell.rects.insert(cell.rects.end(), cell.tempRects[i].begin(), cell.tempRects[i].end());
        buildLayerBVH(cell.rects, layer.rectStart, layer.rectCount, layer.bvh);
    }
    cell.tempRects.clear();
    cell.tempRects.shrink_to_fit();

    processFETLayers(cell);

    // try to finalize cell
    for (const auto & ref : cell.references) {
        auto it = lib.cellMap.find(ref.cellName);
        if (it == lib.cellMap.end() || !lib.cells[it->second].isProcessed) {
            return; // ref not ready, finish cell later
        }
    }
    finishCellAnalysis(cell, lib);
}

// MARK: - Parser

class GdsReader {
    const uint8_t* buffer = nullptr;
    const uint8_t* end = nullptr;
    const uint8_t* pos = nullptr;
    size_t totalRead = 0;

public:
    GdsReader(const uint8_t* buf, size_t len) 
        : buffer(buf), end(buf + len), pos(buf) {}

    uint8_t readU8() {
        totalRead++;
        if (pos >= end) return 0;
        return *pos++;
    }

    size_t tell() const { return totalRead; }

    uint16_t readU16() {
        uint16_t val = (uint16_t)readU8() << 8;
        val |= readU8();
        return val;
    }

    int16_t readI16() { return (int16_t)readU16(); }

    int32_t readI32() {
        uint32_t val = (uint32_t)readU8() << 24;
        val |= (uint32_t)readU8() << 16;
        val |= (uint32_t)readU8() << 8;
        val |= (uint32_t)readU8();
        return (int32_t)val;
    }

    double readReal8() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | readU8();
        int sign = (v >> 63) ? -1 : 1;
        int exponent = (int)((v >> 56) & 0x7f) - 64;
        uint64_t mantissa = v & 0x00ffffffffffffffULL;
        return sign * (double)mantissa * pow(16.0, exponent - 14);
    }

    std::string readString(int len) {
        std::string s;
        bool done = false;
        for (int i = 0; i < len; ++i) {
            uint8_t c = readU8();
            if (c == 0) done = true;
            if (!done) s += (char)c;
        }
        return s;
    }

    void skip(int len) {
        if (len <= 0) return;
        totalRead += len;
        size_t avail = (pos < end) ? (size_t)(end - pos) : 0;
        pos += std::min((size_t)len, avail);
    }
};

namespace Record {
    const uint8_t HEADER = 0x00, BGNLIB = 0x01, LIBNAME = 0x02, UNITS = 0x03,
                  ENDLIB = 0x04, BGNSTR = 0x05, STRNAME = 0x06, ENDSTR = 0x07,
                  BOUNDARY = 0x08, PATH = 0x09, SREF = 0x0A, AREF = 0x0B,
                  TEXT = 0x0C, LAYER = 0x0D, DATATYPE = 0x0E, WIDTH = 0x0F,
                  XY = 0x10, ENDEL = 0x11, SNAME = 0x12, COLROW = 0x13,
                  TEXTNODE = 0x14, NODE = 0x15, TEXTTYPE = 0x16, PRESENTATION = 0x17,
                  STRING = 0x19, STRANS = 0x1A, MAG = 0x1B, ANGLE = 0x1C,
                  PATHTYPE = 0x21;
}


struct ParserState {
    GdsLibrary lib;
    Cell* currentCell = nullptr;
    int16_t currentLayer = 0;
    int16_t currentDatatype = 0;
    std::vector<Point> currentPoints;
    bool isBoundary = false;
    bool isPath = false;
    int32_t currentWidth = 0;
    int16_t currentPathType = 0;
    Reference* currentRef = nullptr;
    Text* currentText = nullptr;
    std::vector<uint8_t> backlog;

    void processRecord(uint8_t type, GdsReader& reader, int dataLen) {
        if (type == Record::ENDLIB) return;
        size_t start = reader.tell();
        switch (type) {
            case Record::LIBNAME: lib.name = reader.readString(dataLen); break;
            case Record::UNITS:
                lib.userUnit = reader.readReal8();
                lib.dbUnit = reader.readReal8();
                break;
            case Record::BGNSTR:
                lib.cells.emplace_back();
                currentCell = &lib.cells.back();
                currentCell->tempRects.resize(L_COUNT);
                break;
            case Record::STRNAME:
                if (currentCell) {
                    currentCell->name = reader.readString(dataLen);
                    lib.cellMap[currentCell->name] = lib.cells.size() - 1;
                }
                break;
            case Record::ENDSTR:
                if (currentCell) processCell(*currentCell, lib);
                currentCell = nullptr; currentRef = nullptr; currentText = nullptr;
                break;

            case Record::BOUNDARY:
                isBoundary = true; isPath = false;
                currentPoints.clear(); currentLayer = 0; currentDatatype = 0;
                break;
            case Record::PATH:
                isPath = true; isBoundary = false;
                currentPoints.clear(); currentLayer = 0; currentDatatype = 0;
                currentWidth = 0; currentPathType = 0;
                break;
            case Record::SREF:
            case Record::AREF:
                if (currentCell) {
                    currentCell->references.emplace_back();
                    currentRef = &currentCell->references.back();
                    currentRef->type = (type == Record::SREF) ? Reference::SREF : Reference::AREF;
                }
                break;
            case Record::TEXT:
                if (currentCell) {
                    currentCell->texts.emplace_back();
                    currentText = &currentCell->texts.back();
                }
                break;
            case Record::LAYER:
                if (isBoundary || isPath) currentLayer = reader.readI16();
                else if (currentText) currentText->layer = reader.readI16();
                break;
            case Record::WIDTH: currentWidth = reader.readI32(); break;
            case Record::PATHTYPE: currentPathType = reader.readI16(); break;
            case Record::DATATYPE:
            case Record::TEXTTYPE:
                if (isBoundary || isPath) currentDatatype = reader.readI16();
                else if (currentText) currentText->texttype = reader.readI16();
                break;
            case Record::XY:
                if (isBoundary || isPath) {
                    int n = dataLen / 4;
                    for (int i = 0; i < n / 2; ++i) currentPoints.push_back({reader.readI32(), reader.readI32()});
                } else if (currentRef) {
                    int n = dataLen / 4;
                    for (int i = 0; i < n / 2; ++i) currentRef->points.push_back({reader.readI32(), reader.readI32()});
                } else if (currentText) {
                    currentText->point = {reader.readI32(), reader.readI32()};
                }
                break;
            case Record::COLROW:
                if (currentRef) { currentRef->cols = reader.readI16(); currentRef->rows = reader.readI16(); }
                break;
            case Record::SNAME: if (currentRef) currentRef->cellName = reader.readString(dataLen); break;
            case Record::STRING: if (currentText) currentText->content = reader.readString(dataLen); break;
            case Record::STRANS:
                if (currentRef) { uint16_t f = reader.readU16(); currentRef->reflection = (f & 0x8000) != 0; }
                else if (currentText) { uint16_t f = reader.readU16(); currentText->reflection = (f & 0x8000) != 0; }
                break;
            case Record::MAG:
                if (currentRef) currentRef->mag = reader.readReal8();
                else if (currentText) currentText->mag = reader.readReal8();
                break;
            case Record::ANGLE:
                if (currentRef) currentRef->angle = reader.readReal8();
                else if (currentText) currentText->angle = reader.readReal8();
                break;
            case Record::ENDEL:
                if (isBoundary && currentCell) {
                    LayerID lid = getLayerID(currentLayer, currentDatatype);
                    if (lid != L_COUNT) fracture(currentPoints, currentCell->tempRects[lid]);
                } else if (isPath && currentCell) {
                    LayerID lid = getLayerID(currentLayer, currentDatatype);
                    if (lid != L_COUNT) convertPathToRects(currentPoints, currentWidth, currentPathType, currentCell->tempRects[lid]);
                }
                isBoundary = false; isPath = false; currentRef = nullptr; currentText = nullptr;
                break;
            default: break;
        }
        size_t consumed = reader.tell() - start;
        if (consumed < (size_t)dataLen) reader.skip(dataLen - consumed);
    }

    void consumeChunk(const uint8_t* data, size_t len) {
        if (len == 0) return;
        const uint8_t* p = data;
        const uint8_t* end = data + len;

        // Combine with backlog if necessary
        if (!backlog.empty()) {
            while (p < end && backlog.size() < 2) backlog.push_back(*p++);
            if (backlog.size() < 2) return;
            uint16_t rlen = ((uint16_t)backlog[0] << 8) | backlog[1];
            while (p < end && backlog.size() < rlen) backlog.push_back(*p++);
            if (backlog.size() < rlen) return;

            GdsReader r(backlog.data(), backlog.size());
            r.readU16(); // Skip len
            uint16_t head = r.readU16();
            processRecord(head >> 8, r, rlen - 4);
            backlog.clear();
        }

        while (p + 4 <= end) {
            uint16_t rlen = ((uint16_t)p[0] << 8) | p[1];
            if (rlen < 4) { p += 2; continue; }
            if (p + rlen > end) break;

            GdsReader r(p, rlen);
            r.readU16(); // Skip len
            uint16_t head = r.readU16();
            processRecord(head >> 8, r, rlen - 4);
            p += rlen;
        }

        if (p < end) {
            backlog.assign(p, end);
        }
    }

    void finalize() {
        if (currentCell) {
            fprintf(stderr, "Error: GDS truncated (cell '%s' not closed)\n", currentCell->name.c_str());
        }

        std::vector<int> visited(lib.cells.size(), 0);
        std::function<void(size_t)> visit = [&](size_t idx) {
            if (visited[idx] != 0 || lib.cells[idx].isProcessed) return;
            visited[idx] = 1;
            for (const auto& ref : lib.cells[idx].references) {
                auto it = lib.cellMap.find(ref.cellName);
                if (it != lib.cellMap.end()) {
                    visit(it->second);
                }
            }
            visited[idx] = 2;
            
            if (lib.cells[idx].tempRects.empty()) {
                finishCellAnalysis(lib.cells[idx], lib);
            }
        };

        for (size_t i = 0; i < lib.cells.size(); ++i) {
            if (!lib.cells[i].isProcessed) visit(i);
        }

        for (const auto& cell : lib.cells) {
            if (!cell.isProcessed) {
                fprintf(stderr, "Error: Cell '%s' was not processed (truncated stream?)\n", cell.name.c_str());
            }
        }

        for (const auto& cell : lib.cells) {
            if (cell.isTop) {
                lib.topCellName = cell.name;
                break;
            }
        }
        if (lib.topCellName.empty() && !lib.cells.empty()) {
             lib.topCellName = lib.cells.back().name;
        }
    }
};


// MARK: - Circuit Export

struct FlattenedData {
    std::vector<RectWire> rectData; // Flattened [x1, y1, x2, y2, wire, ...]
    uint32_t layerOffsets[L_COUNT + 1];
    CircuitBuilder builder;
};
struct CircuitFlattener {
    GdsLibrary& lib;
    CircuitBuilder& builder;
    std::vector<RectWire> flatLayers[L_COUNT];

    CircuitFlattener(GdsLibrary& lib, CircuitBuilder& builder) : lib(lib), builder(builder) {}

    void flatten(Cell& cell, std::map<int32_t, int> externalWires, const std::string& prefix, Transformer t) {
        CircuitBuilder::Scope scope(builder, prefix.empty() ? nullptr : prefix.c_str());

        // wire_dsu_root -> builder_wire_id
        std::map<int32_t, int> localWireMap = externalWires;

        auto getLocalWire = [&](int32_t wireRoot) {
            auto it = localWireMap.find(wireRoot);
            if (it != localWireMap.end()) return it->second;
            return localWireMap[wireRoot] = builder.add_wire();
        };

        // Determine active wires (connected to I/O, FETs, or Sub-cells)
        std::set<int32_t> activeWires;
        for (const auto& [root, _] : externalWires) activeWires.insert(root);
        for (const auto& fet : cell.fets) {
            activeWires.insert(fet.gate);
            activeWires.insert(fet.term[0]);
            activeWires.insert(fet.term[1]);
        }
        for (const auto& cw : cell.connectionsR2W) {
             activeWires.insert(cell.wireDSU.find(cw.parentIdx));
        }

        // Pre-allocate IDs for all active wires to ensure consistent numbering
        for (int32_t root : activeWires) getLocalWire(root);

        // Collect Rects
        for (int l = 0; l < L_COUNT; ++l) {
            if (l == L_DIFF || l == L_CHANNEL || l == L_N_TERM || l == L_P_TERM) continue;

            const auto& layer = cell.layers[l];
            for (size_t i = 0; i < layer.rectCount; ++i) {
                int rectIdx = layer.rectStart + i;
                const Rect& r = cell.rects[rectIdx];
                int wireRoot = cell.wireDSU.find(rectIdx);
                Rect tr = t.applyToRect(r, 0, 0);

                int globalWire = -1;
                if (l == L_NWELL || l == L_ERROR) {
                    globalWire = -1;
                } else if (activeWires.count(wireRoot)) {
                    globalWire = getLocalWire(wireRoot);
                } else {
                    flatLayers[L_ERROR].push_back({tr, -1});
                    continue;
                }

                LayerID targetL = (LayerID)l;
                if (l == L_TERMINAL) {
                    bool isP = false;
                    const auto& nwellL = cell.layers[L_NWELL];
                    if (nwellL.rectCount > 0) {
                        queryBVH(nwellL.bvh, cell.rects, r, [&](int) { isP = true; }, overlaps);
                    }
                    targetL = isP ? L_P_TERM : L_N_TERM;
                }
                flatLayers[targetL].push_back({tr, globalWire});
            }
        }

        // Flatten FETs
        for (const auto& fet : cell.fets) {
            int g = getLocalWire(cell.wireDSU.find(fet.gate));
            int t0 = getLocalWire(cell.wireDSU.find(fet.term[0]));
            int t1 = getLocalWire(cell.wireDSU.find(fet.term[1]));
            builder.add_fet(g, t0, t1, fet.type);
        }
        if (builder.fets.size() % 50000 < cell.fets.size()) {
            printf("Exported %zu fets...\n", builder.fets.size());
        }

        // Traverse children
        for (size_t refIdx = 0; refIdx < cell.references.size(); ++refIdx) {
            const auto& ref = cell.references[refIdx];
            if (ref.type == Reference::AREF) continue; // Ignore AREFs for now
            auto it = lib.cellMap.find(ref.cellName);
            if (it == lib.cellMap.end()) continue;
            Cell& child = lib.cells[it->second];

            if (child.flatFETs == 0 && cell.bridgingRefs.find((int32_t)refIdx) == cell.bridgingRefs.end()) {
                continue;
            }

            std::map<int32_t, int> childExternalWires;
            for (const auto& cw : cell.connectionsR2W) {
                if (cw.childRefIdx == (int32_t)refIdx) {
                    int32_t parentRoot = cell.wireDSU.find(cw.parentIdx);
                    childExternalWires[cw.childIdx] = getLocalWire(parentRoot);
                }
            }
            
            std::string subPrefix = ref.cellName + "_" + std::to_string(refIdx);
            flatten(child, childExternalWires, subPrefix, t.compose(ref));
        }
    }
};

static void optimizeFlattenedGeometry(std::vector<RectWire> rawLayers[L_COUNT], FlattenedData& out) {
    printf("Optimizing geometry... ");
    size_t totalBefore = 0, totalDiscarded = 0;

    for (int l = 0; l < L_COUNT; ++l) {
        out.layerOffsets[l] = (uint32_t)out.rectData.size();
        auto& rects = rawLayers[l];
        if (rects.empty()) continue;
        totalBefore += rects.size();

        std::vector<BVHNode> nodes;
        buildLayerBVH(rects, 0, (uint32_t)rects.size(), nodes);

        std::vector<bool> discarded(rects.size(), false);
        for (size_t i = 0; i < rects.size(); ++i) {
            queryBVH(nodes, rects, rects[i], [&](int j) {
                if (i == (size_t)j || discarded[j]) return;
                if (contains(rects[j], rects[i])) discarded[i] = true;
            }, overlaps);
            if (discarded[i]) totalDiscarded++;
        }

        for (size_t i = 0; i < rects.size(); ++i) {
            if (!discarded[i]) out.rectData.push_back(rects[i]);
        }
    }
    out.layerOffsets[L_COUNT] = (uint32_t)out.rectData.size();
    if (totalDiscarded) printf("discarded %zu of %zu rects\n", totalDiscarded, totalBefore);
}

static std::map<int32_t, int> initializeTopWires(Cell& topCell, CircuitBuilder& builder) {
    std::map<int32_t, int> topWireMap;
    int32_t gndRoot = (topCell.groundRect != -1) ? topCell.wireDSU.find(topCell.groundRect) : -1;
    int32_t pwrRoot = (topCell.powerRect != -1) ? topCell.wireDSU.find(topCell.powerRect) : -1;

    if (gndRoot != -1) topWireMap[gndRoot] = 0; 
    if (pwrRoot != -1) topWireMap[pwrRoot] = 1;

    while (builder.wire_names.size() < 2) builder.add_wire();

    for (auto const& [label, rectIdx] : topCell.label2rect) {
        int32_t root = topCell.wireDSU.find(rectIdx);
        if (root == gndRoot || root == pwrRoot || topWireMap.count(root)) continue;
        topWireMap[root] = builder.add_wire(label.c_str());
    }
    return topWireMap;
}

FlattenedData flattenCircuit(GdsLibrary& lib, const std::string& topCellName) {
    FlattenedData out;
    auto it = lib.cellMap.find(topCellName);
    if (it == lib.cellMap.end()) {
        fprintf(stderr, "Top cell '%s' not found for circuit export\n", topCellName.c_str());
        return out;
    }
    Cell& topCell = lib.cells[it->second];

    CircuitFlattener flattener(lib, out.builder);
    flattener.flatten(topCell, initializeTopWires(topCell, out.builder), "", Transformer());
    optimizeFlattenedGeometry(flattener.flatLayers, out);

    return out;
}

void exportCircuit(GdsLibrary& lib, const std::string& topCellName, const std::string& baseFilename) {
    FlattenedData out = flattenCircuit(lib, topCellName);
    if (out.layerOffsets[L_COUNT] == 0 && out.builder.fets.empty()) return;

    std::string rectsFilename = baseFilename + ".rects";
    std::string txtFilename = baseFilename + ".txt";
    
    FILE* fRects = fopen(rectsFilename.c_str(), "w");
    if (!fRects) {
        fprintf(stderr, "Failed to open %s for writing\n", rectsFilename.c_str());
    } else {
        printf("Exporting rects to %s\n", rectsFilename.c_str());
        fprintf(fRects, "net layer x y w h\n");
        for (int l = 0; l < L_COUNT; ++l) {
            for (uint32_t i = out.layerOffsets[l]; i < out.layerOffsets[l+1]; ++i) {
                const auto& fr = out.rectData[i];
                fprintf(fRects, "%d %d %d %d %d %d\n", fr.wire, l, fr.x1, fr.y1, fr.x2 - fr.x1, fr.y2 - fr.y1);
            }
        }
        fclose(fRects);
    }


    FILE* f = fopen(txtFilename.c_str(), "w");
    if (!f) return;
    
    // First line: Top-level wire names (no slashes)
    bool first = true;
    for (size_t i = 0; i < out.builder.wire_names.size(); ++i) {
        const std::string& name = out.builder.wire_names[i];
        if (name.find('/') == std::string::npos) {
            // Skip anonymous wires like "w123"
            if (name.size() > 1 && name[0] == 'w' && isdigit(name[1])) continue;
            if (!first) fprintf(f, " ");
            fprintf(f, "%s", name.c_str());
            first = false;
        }
    }
    fprintf(f, "\n");

    // FET lines: TYPE GATE T1 T2
    for (size_t i = 0; i < out.builder.fets.size(); ++i) {
        const auto& fet = out.builder.fets[i];
        int t0 = fet.term[0], t1 = fet.term[1];
        if (t0 > t1) std::swap(t0, t1);
        fprintf(f, "%c %d %d %d\n", "PN"[fet.type], fet.gate, t0, t1);
    }
    fclose(f);
    printf("Flattened netlist exported to %s\n", txtFilename.c_str());
    printf("Stats: NetlistWires=%d NetlistFETs=%zu\n", out.builder.wire_n, out.builder.fets.size());
}

static void printLibrarySummary(const GdsLibrary& lib, double msParse, double msExport) {
    printf("Library: %s\n", lib.name.c_str());
    printf("Units: User=%g, DB=%g\n", lib.userUnit, lib.dbUnit);
    printf("Total Cells: %zu\n\n", lib.cells.size());

    std::set<std::string> referencedCells;
    for (const auto& cell : lib.cells) {
        for (const auto& ref : cell.references) referencedCells.insert(ref.cellName);
    }

    size_t totalFlatRects = 0;
    size_t totalFlatFETs = 0;

    std::string topName = lib.topCellName;
    if (!topName.empty()) {
        const Cell& top = lib.cells[lib.cellMap.at(topName)];
        totalFlatRects = top.flatRects;
        totalFlatFETs = top.flatFETs;
    }

    if (msParse >= 0 || msExport >= 0) {
        printf("\nPerformance: GDS Parse: %.2f ms, Export (Netlist): %.2f ms\n", 
               msParse >= 0 ? msParse : 0.0, 
               msExport >= 0 ? msExport : 0.0);
    }
    printf("Stats: FlatRects=%zu FlatFETs=%zu\n", totalFlatRects, totalFlatFETs);
    fflush(stdout);
}




// MARK: - WASM

#ifdef __wasm__
#define WASM_EXPORT(name) __attribute__((export_name(name)))
ParserState* wasm_state = nullptr;
FlattenedData* global_flat_data = nullptr;

extern "C" {
    WASM_EXPORT("wasm_malloc") void* wasm_malloc(size_t size) { return malloc(size); }
    WASM_EXPORT("wasm_free") void wasm_free(void* ptr) { free(ptr); }

    WASM_EXPORT("wasm_init") void wasm_init() {
        if (wasm_state) delete wasm_state;
        wasm_state = new ParserState();
        if (global_flat_data) { delete global_flat_data; global_flat_data = nullptr; }
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    WASM_EXPORT("wasm_push_chunk") void wasm_push_chunk(const uint8_t* data, size_t len) {
        if (wasm_state) wasm_state->consumeChunk(data, len);
    }

    WASM_EXPORT("wasm_finalize") void wasm_finalize() {
        if (!wasm_state) return;
        wasm_state->finalize();
        printLibrarySummary(wasm_state->lib, -1, -1);
        std::string top = wasm_state->lib.topCellName;
        if (!top.empty()) {
            printf("Auto-detected top cell: %s\n", top.c_str());
            if (global_flat_data) delete global_flat_data;
            global_flat_data = new FlattenedData(flattenCircuit(wasm_state->lib, top));
        } else {
            printf("ERROR: No top cell detected in GDS library.\n");
        }
    }

    WASM_EXPORT("wasm_get_rect_data_ptr") int32_t* wasm_get_rect_data_ptr() {
        return global_flat_data ? (int32_t*)global_flat_data->rectData.data() : nullptr;
    }
    WASM_EXPORT("wasm_get_rect_data_size") uint32_t wasm_get_rect_data_size() {
        return global_flat_data ? global_flat_data->rectData.size() * RECT_WIRE_FIELDS : 0;
    }
    WASM_EXPORT("wasm_get_layer_offsets_ptr") uint32_t* wasm_get_layer_offsets_ptr() {
        return global_flat_data ? (uint32_t*)global_flat_data->layerOffsets : nullptr;
    }
    WASM_EXPORT("wasm_get_layer_offsets_size") uint32_t wasm_get_layer_offsets_size() {
        return global_flat_data ? L_COUNT + 1 : 0;
    }
}


#else

// MARK: - Main

GdsLibrary parseGDS(const std::string& filename) {
    bool isBrotli = (filename.size() > 3 && filename.compare(filename.size() - 3, 3, ".br") == 0);
    FILE* file = isBrotli ? popen(("brotli -d -c " + filename).c_str(), "r") : fopen(filename.c_str(), "rb");
    if (!file) throw std::runtime_error("Cannot open file: " + filename);

    static uint8_t chunk[256 * 1024];
    ParserState state;
    while (size_t n = fread(chunk, 1, sizeof(chunk), file)) {
        state.consumeChunk(chunk, n);
    }

    if (isBrotli) pclose(file);
    else fclose(file);

    state.finalize();
    return state.lib;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./gds_parser <file.gds>\n");
        return 1;
    }

    try {
        auto t0 = std::chrono::high_resolution_clock::now();
        GdsLibrary lib = parseGDS(argv[1]);
        auto t1 = std::chrono::high_resolution_clock::now();
        double msParse = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::string baseName = argv[1];
        if (baseName.size() > 3 && baseName.compare(baseName.size() - 3, 3, ".br") == 0) baseName = baseName.substr(0, baseName.size() - 3);
        if (baseName.size() > 4 && baseName.compare(baseName.size() - 4, 4, ".gds") == 0) baseName = baseName.substr(0, baseName.size() - 4);

        // Find top cells
        std::string topCellName;
        if (argc > 2) {
            topCellName = argv[2];
            printf("Using requested top cell: %s\n", topCellName.c_str());
        } else {
            topCellName = lib.topCellName;
            if (!topCellName.empty()) {
                printf("Auto-detected top cell: %s\n", topCellName.c_str());
            }
        }

        auto t_export0 = std::chrono::high_resolution_clock::now();
        if (!topCellName.empty()) {
            exportCircuit(lib, topCellName, baseName);
        }
        auto t_export1 = std::chrono::high_resolution_clock::now();
        double msExport = std::chrono::duration<double, std::milli>(t_export1 - t_export0).count();

        printLibrarySummary(lib, msParse, msExport);


    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
#endif
