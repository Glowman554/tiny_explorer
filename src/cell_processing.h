#pragma once

#include "geom.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "cells.h"


// Normalize cell by converting paths to polygons and applying repetitions
// TODO: apply polygon repetitions manually after fracturing
template <class S, class T> void to_polygons(const S& src, T & dst) {
    for (uint64_t i = 0; i < src.count; i++) {
        src[i]->to_polygons(false, 0, dst);
    }
}
template <class T> void apply_repetition(T & arr) {
    const int finish = arr.count;
    for (uint64_t i = 0; i < finish; i++) {
        arr[i]->apply_repetition(arr);
    }
}
inline void normalize_gcell(gdstk::Cell * gcell) {
    to_polygons(gcell->flexpath_array,   gcell->polygon_array);
    to_polygons(gcell->robustpath_array, gcell->polygon_array);
    apply_repetition(gcell->polygon_array);
    apply_repetition(gcell->reference_array);
    apply_repetition(gcell->label_array);
}

inline void resolveLabelsGdstk(const gdstk::Cell* gcell, Cell& cell, const std::string& pdk) {

    for (int i=0; i<gcell->label_array.count; ++i) {
        const auto * label = gcell->label_array[i];
        LayerID targetLid = tag2id(label->tag, pdk);
        if (targetLid == L_COUNT) continue;

        const auto& layer = cell.layers[targetLid];
        if (layer.rectCount == 0) continue;

        const auto & p = label->origin;
        const int32_t x = std::round(p.x), y = std::round(p.y);
        Rect pRect = {x, y, x, y};
        int32_t foundRectIdx = -1;
        queryBVH(layer.bvh, cell.rects, pRect, [&](int idx) {
            foundRectIdx = idx;
            return false; // stop search
        }, touches);
        if (foundRectIdx == -1) {
            printf("  Label '%s' at (%.2f, %.2f) on layer %d:%d - NO RECT FOUND\n", 
                   label->text, label->origin.x, label->origin.y, 
                   gdstk::get_layer(label->tag), gdstk::get_type(label->tag));
            continue;
        }

        if (cell.label2rect.count(label->text)) {
            cell.wireDSU.unite(cell.label2rect[label->text], foundRectIdx);
        } else {
            cell.label2rect[label->text] = foundRectIdx;
        }
        
        // Power/Ground Detection
        if (isGroundLabel(label->text)) cell.groundRect = foundRectIdx;
        if (isPowerLabel(label->text)) cell.powerRect = foundRectIdx;
    }
}

struct Transform {
    int32_t m00=1, m01=0, tx=0;
    int32_t m10=0, m11=1, ty=0;

    Transform() = default;

    static bool isSupported(const gdstk::Reference& ref) {
        auto is_int = [](double v) { return std::abs(v - std::round(v)) < 1e-4; };
        double ang_90 = ref.rotation / (M_PI / 2.0);
        if (!is_int(ang_90)) return false;
        if (!is_int(ref.magnification)) return false;
        if (!is_int(ref.origin.x) || !is_int(ref.origin.y)) return false;
        return true;
    }

    Transform(const gdstk::Reference& ref) {
        int mag = (int)std::round(ref.magnification);
        int rot_quadrant = (int)std::round(ref.rotation / (M_PI / 2.0)) % 4;
        if (rot_quadrant < 0) rot_quadrant += 4;

        int mc = (rot_quadrant == 0) ? mag : (rot_quadrant == 2 ? -mag : 0);
        int ms = (rot_quadrant == 1) ? mag : (rot_quadrant == 3 ? -mag : 0);

        if (ref.x_reflection) {
            m00 = mc; m01 = ms;
            m10 = ms; m11 = -mc;
        } else {
            m00 = mc; m01 = -ms;
            m10 = ms; m11 = mc;
        }
        tx = (int32_t)std::round(ref.origin.x);
        ty = (int32_t)std::round(ref.origin.y);
    }
    
    Transform(int32_t a, int32_t b, int32_t x, int32_t c, int32_t d, int32_t y)
        : m00(a), m01(b), tx(x), m10(c), m11(d), ty(y) {}

    void apply(int32_t x, int32_t y, int32_t& ox, int32_t& oy) const {
        ox = m00 * x + m01 * y + tx;
        oy = m10 * x + m11 * y + ty;
    }

    Transform compose(const Transform & child) const {
        return Transform(
            m00*child.m00 + m01*child.m10,
            m00*child.m01 + m01*child.m11,
            m00*child.tx  + m01*child.ty + tx,
            
            m10*child.m00 + m11*child.m10,
            m10*child.m01 + m11*child.m11,
            m10*child.tx  + m11*child.ty + ty
        );
    }

    Rect apply(const Rect& r) const {
        int32_t x1, y1, x2, y2;
        apply(r.x1, r.y1, x1, y1);
        apply(r.x2, r.y2, x2, y2);
        return {
            std::min(x1, x2), std::min(y1, y2),
            std::max(x1, x2), std::max(y1, y2)
        };
    }
};


inline void mergeCellWires(Cell& cell) {
    cell.wireDSU.reset(cell.rects.size());
    const auto & layers = cell.layers;
    auto joinWires = [&](int i, int k) { cell.wireDSU.unite(i, k); };
    // Intra-layer merging
    for (auto& layer : layers) {
        collideSelf(layer.bvh, cell.rects, joinWires);
    }
    // Inter-layer merging based on LayerStack (consecutive enum values)
    auto mergeLayers = [&](int i, int j) {
        collideTrees(layers[i].bvh, cell.rects, layers[j].bvh, cell.rects, joinWires);
    };
    for (int i = L_POLY; i+1 <= L_MET5; ++i) {
        mergeLayers(i, i+1);
    }
    mergeLayers(L_LICON, L_N_TERM);
    mergeLayers(L_LICON, L_P_TERM);
}

inline void assignWireIDs(Cell& cell) {
    if (cell.rects.empty()) return;

    // 1. Force IDs for special nets
    cell.rect2wire.assign(cell.rects.size(), DSU::NeedsID);
    if (cell.groundRect != -1) {
        cell.rect2wire[cell.wireDSU.find(cell.groundRect)] = 0;
    }
    if (cell.powerRect != -1) {
        cell.rect2wire[cell.wireDSU.find(cell.powerRect)] = 1;
    }

    // 2. Assign IDs to labeled wires first to keep them stable
    int next_id = 2;
    for (auto const& [name, rIdx] : cell.label2rect) {
        int root = cell.wireDSU.find(rIdx);
        if (cell.rect2wire[root] == DSU::NeedsID) {
            cell.rect2wire[root] = next_id++;
        }
    }

    // 3. Prevent non-participating rects from getting IDs
    for (int i = L_NWELL; i <= L_CHANNEL; ++i) {
        const auto & l = cell.layers[i];
        auto begin = cell.rect2wire.begin() + l.rectStart;
        auto end = begin + l.rectCount;
        std::fill(begin, end, DSU::Skip);
    }

    // 4. Final mapping pass: fills rest of rect2wire and assigns IDs to unlabeled roots
    cell.wireCount = cell.wireDSU.assign_ids(cell.rect2wire, next_id);
}

inline void extractFETs(Cell& cell) {
    const auto& channels = cell.layers[L_CHANNEL];    
    const auto& nwellLayer = cell.layers[L_NWELL];
    const auto& gateLayer = cell.layers[L_POLY];
    if (channels.rectCount == 0) {
        return;
    }

    cell.fets.clear();
    size_t malformedCount = 0;
    size_t decapCount = 0;

    for (uint32_t i = 0; i < channels.rectCount; ++i) {
        int idx = channels.rectStart + i;
        Rect channelRect = cell.rects[idx];

        // 1. Identify Type (N/P)
        bool isPType = false;
        // Check if overlaps NWELL
        if (nwellLayer.rectCount > 0) {
            queryBVH(nwellLayer.bvh, cell.rects, channelRect, [&](int) { 
                isPType = true; 
                return false; // stop searh
            }, overlaps);
        }

        // 2. Identify Gate
        int32_t gateWire = -1;
        {
            queryBVH(gateLayer.bvh, cell.rects, channelRect, [&](int idx) {
                gateWire = cell.rect2wire[idx];
                return false; // stop search
            }, overlaps);
        }

        // 3. Identify Terminals
        int32_t terminals[2] = {-1, -1};
        int tCount = 0;
        const auto& tLayer = isPType ? cell.layers[L_P_TERM] : cell.layers[L_N_TERM];
        queryBVH(tLayer.bvh, cell.rects, channelRect, [&](int idx) {
            int32_t w = cell.rect2wire[idx];
            if (w == terminals[0] || w == terminals[1]) return true;
            if (tCount < 2) terminals[tCount] = w;
            tCount++;
            return true;
        }, touches);

        if (tCount != 2) {
            // Suppress warning if gate is tied to PWR/GND (Fill/Decap)
            if (gateWire == 0 || gateWire == 1) {
                decapCount++;
            } else {
                malformedCount++;
            }
            continue;
        }
        
        int32_t t1 = terminals[0];
        int32_t t2 = terminals[1];
        if (t1 > t2) std::swap(t1, t2);
        if (gateWire < 0 || t1 < 0 || t2 < 0) {
            printf("Warning: FET in cell %s has terminals with unassigned wire IDs: gate %d, t1 %d, t2 %d\n",
                   cell.name.c_str(), gateWire, t1, t2);
            continue;
        }

        FET fet;
        fet.type = isPType ? FET::P : FET::N;
        fet.gate = gateWire;
        fet.term[0] = t1;
        fet.term[1] = t2;
        
        cell.fets.push_back(fet);
    }
    
    if (malformedCount > 0) {
        printf("Warning: %zu malformed FETs in cell %s (skipped)\n", malformedCount, cell.name.c_str());
    }
    
    unique_sort(cell.fets);
}

inline void processFETLayers(Cell& cell) {
    const auto& diffLayer = cell.layers[L_DIFF];
    const auto& gateLayer = cell.layers[L_POLY];
    const auto& nwellLayer = cell.layers[L_NWELL];
    
    if (diffLayer.rectCount == 0 || gateLayer.rectCount == 0) return;

    std::vector<Rect> channels, n_terminals, p_terminals;
    std::vector<std::vector<int>> diff_overlaps(diffLayer.rectCount);

    collideTrees(diffLayer.bvh, cell.rects, gateLayer.bvh, cell.rects, [&](int i, int k) {
        diff_overlaps[i - diffLayer.rectStart].push_back(k);
        Rect inter;
        if (getIntersection(cell.rects[i], cell.rects[k], inter)) {
            channels.push_back(inter);
        }
    }, overlaps);

    for (uint32_t i = 0; i < diffLayer.rectCount; ++i) {
        Rect diffRect = cell.rects[diffLayer.rectStart + i];
        
        bool isPType = false;
        if (nwellLayer.rectCount > 0) {
            queryBVH(nwellLayer.bvh, cell.rects, diffRect, [&](int) {
                isPType = true;
                return false;
            }, overlaps);
        }

        std::vector<Rect> current = {diffRect};
        for (int o : diff_overlaps[i]) {
            std::vector<Rect> next;
            for (const auto& r : current) subtractRect(r, cell.rects[o], next);
            current = std::move(next);
        }
        
        auto& target = isPType ? p_terminals : n_terminals;
        target.insert(target.end(), current.begin(), current.end());
    }
    
    auto add_layer = [&](LayerID lid, std::vector<Rect>& src) {
        auto& l = cell.layers[lid];
        l.rectStart = (uint32_t)cell.rects.size();
        l.rectCount = (uint32_t)src.size();
        cell.rects.insert(cell.rects.end(), src.begin(), src.end());
        buildLayerBVH(cell.rects, l.rectStart, l.rectCount, l.bvh);
    };

    add_layer(L_CHANNEL, channels);
    add_layer(L_N_TERM, n_terminals);
    add_layer(L_P_TERM, p_terminals);
}

struct CellProcessor {
    struct VerticalEdge { int32_t x, y_min, y_max; };
    std::vector<int32_t> ys;
    std::vector<int32_t> xs;
    std::vector<VerticalEdge> v_edges;
    std::array<std::vector<Rect>, L_COUNT> tempRects; 

    CellProcessor() {
        ys.reserve(128);
        xs.reserve(128);
        v_edges.reserve(128);
    }

    void run(gdstk::Cell* gcell, Cell & cell, const std::string& pdk) {
        cell.name = gcell->name;
        normalize_gcell(gcell);
        fracture_polygons(gcell, cell, pdk);
        processFETLayers(cell);
        mergeCellWires(cell);
        resolveLabelsGdstk(gcell, cell, pdk);
        assignWireIDs(cell);
        extractFETs(cell);
        cell.isFiller = isFillerCell(cell.name) && gcell->reference_array.count == 0;
        if (cell.rects.size() > 1000) {
            printf("Big cell: %s (%zu rects, %u wires, %zu FETs)\n", 
                cell.name.c_str(), cell.rects.size(), cell.wireDSU.count(), cell.fets.size());
        }
    }

    void fracture_polygons(const gdstk::Cell* gcell, Cell & cell, const std::string& pdk) {
        for (auto& layer : tempRects) {layer.clear();}

        for (int i=0; i<gcell->polygon_array.count; ++i) {
            const auto & poly = gcell->polygon_array[i];
            LayerID lid = tag2id(poly->tag, pdk);
            if (lid != L_COUNT) {
                const auto & points = poly->point_array;
                fracture_polygon(points.count, points.items, tempRects[lid]);
            }
        }

        cell.bbox = Rect::empty();
        for (int i = 0; i < L_COUNT; i++) {
            optimizeRects(tempRects[i]);
            auto& layer = cell.layers[i];
            // make sure even empty layers have correct rectStart
            layer.rectStart = (uint32_t)cell.rects.size();
            layer.rectCount = (uint32_t)tempRects[i].size();
            if (tempRects[i].empty()) continue;
            cell.rects.insert(cell.rects.end(), tempRects[i].begin(), tempRects[i].end());
            buildLayerBVH(cell.rects, layer.rectStart, layer.rectCount, layer.bvh);
            cell.bbox = getUnion(cell.bbox, layer.bvh[0].bbox);
        }
    }

    void fracture_polygon(int n, const gdstk::Vec2* points, std::vector<Rect>& out) {
        if (n < 4) return;

        // 1. Fast path for simple rectangles
        if (n == 4) {
            int32_t x1 = (int32_t)std::round(points[0].x);
            int32_t y1 = (int32_t)std::round(points[0].y);
            int32_t x2 = x1, y2 = y1;
            for (uint64_t i = 1; i < 4; i++) {
                int32_t px = (int32_t)std::round(points[i].x);
                int32_t py = (int32_t)std::round(points[i].y);
                if (px < x1) x1 = px; if (px > x2) x2 = px;
                if (py < y1) y1 = py; if (py > y2) y2 = py;
            }
            out.push_back({x1, y1, x2, y2});
            return;
        }

        // 2. Prepare Y-coordinates and Vertical Edges
        ys.clear();
        v_edges.clear();
        for (uint64_t i = 0; i < n; i++) {
            uint64_t next = (i + 1 == n) ? 0 : i + 1;
            int32_t x1 = (int32_t)std::round(points[i].x);
            int32_t y1 = (int32_t)std::round(points[i].y);
            int32_t x2 = (int32_t)std::round(points[next].x);
            int32_t y2 = (int32_t)std::round(points[next].y);
            
            ys.push_back(y1);
            if (x1 == x2 && y1 != y2) {
                v_edges.push_back({x1, std::min(y1, y2), std::max(y1, y2)});
            }
        }
        
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

        // 3. Sweep-line across Y strips (no vertical merging)
        for (size_t i = 0; i + 1 < ys.size(); i++) {
            int32_t yLow = ys[i];
            int32_t yHigh = ys[i+1];

            xs.clear();
            for (const auto& edge : v_edges) {
                if (edge.y_min <= yLow && edge.y_max >= yHigh) {
                    xs.push_back(edge.x);
                }
            }
            std::sort(xs.begin(), xs.end());

            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                out.push_back({xs[k], yLow, xs[k+1], yHigh});
            }
        }
    }
};
