#pragma once

#include "cells.h"

inline void mergeCellWires(Cell& cell) {
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

inline void extractFETs(Cell& cell) {
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

inline void processFETLayers(Cell& cell) {
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

inline void resolveLabels(Cell& cell) {
    const std::vector<std::string> gndLabels = {"GND", "VGND", "VSS"};
    const std::vector<std::string> pwrLabels = {"PWR", "VPWR", "VDPWR", "VDD"};

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
inline void forEachRefInstance(const Reference& ref, Func func) {
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

inline void buildCellConnections(Cell& parent, CellLibrary& lib) {
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

inline void computeCellStats(Cell& cell, const CellLibrary& lib) {
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

inline void tryProcessCell(Cell& cell, CellLibrary& lib) {
    if (cell.isProcessed) return;

    if (!cell.tempRects.empty()) {
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
    }

    // try to finalize cell
    for (const auto & ref : cell.references) {
        auto it = lib.cellMap.find(ref.cellName);
        if (it == lib.cellMap.end() || !lib.cells[it->second].isProcessed) {
            return; // ref not ready, finish cell later
        }
    }
    
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