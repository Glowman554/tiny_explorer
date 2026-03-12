#pragma once

#include "cell_processing.h"
#include "cells.h"
#include "geom.h"
#include <vector>

using CellID = int;
using InstID = int;
struct Instance : Rect {
    CellID cell_id;
    InstID inst_id;
    Transform tform;
};

struct RectWire : Rect { int wire, tree; };
constexpr int RECT_WIRE_FIELDS = 6; // x1, y1, x2, y2, wire, tree

struct CircuitExtractor {
    gdstk::Library glib;

    std::vector<Cell> cells;
    std::string pdk; // empty -> autodetect
    std::string topOverride;
    std::map<const gdstk::Cell*, int> gcell2id;

    std::vector<Instance> instances; // just for viz

    struct InstLayer {
        std::vector<Instance> instances;
        std::vector<BVHNode> bvh;
    };
    std::array<InstLayer, L_COUNT> instLayers;
    CircuitBuilder builder;
    Circuit circuit;

    std::vector<int> instOffsets;
    std::vector<int> segment2flat;
    DSU segemntDSU;
    struct LabeledWire { int id; std::string name; LayerID layer; };
    std::vector<LabeledWire> labeledWires;
    std::vector<int> wire2root;

    std::vector<RectWire> flatRects;
    std::array<uint32_t, L_COUNT + 1> flatLayerOffsets;
    std::array<std::vector<BVHNode>, L_COUNT> flatBVHs;


    bool run(const char * path) {
        if (!load(path)) return false;
        return process();
    }

    bool load(const char * path) {
        return loadLib(path);
    }

    bool process() {
        gdstk::Cell * top = getTop();
        if (!top) return false;
        detectPDK();
        preprocessCells();
        const Cell topCell = cells[gcell2id[top]];
        if (topCell.groundRect == -1 || topCell.powerRect == -1) {
            fprintf(stderr, "Error: missing power/ground labels in top cell\n");
            return false;
        }
        
        walkRefs(top, Transform());
        wireCells();
        buildNetlist();
        exportRects();

        printStats();
        return true;
    }

    bool loadLib(const char * path) {
        gdstk::ErrorCode err = gdstk::ErrorCode::NoError;
        std::string s_path = path;
        const double unit = 1e-9;
        if (s_path.size() >= 4 && s_path.substr(s_path.size() - 4) == ".oas") {
            glib = gdstk::read_oas(path, unit, 0, &err);
        } else {
            glib = gdstk::read_gds(path, unit, 0, nullptr, &err);
        }
        if (err != gdstk::ErrorCode::NoError) {
            printf("Error: unable to load library %s (gdstk ErrorCode %d)\n", path, (int)err);
            return false;
        }
        return true;
    }

    gdstk::Cell * getTop() {
        if (!topOverride.empty()) {
            for (uint32_t i = 0; i < glib.cell_array.count; ++i) {
                if (topOverride == glib.cell_array[i]->name) {
                    gdstk::Cell * top = glib.cell_array[i];
                    printf("Top cell (override): %s\n", top->name);
                    return top;
                }
            }
            printf("Warning: override top cell '%s' not found, falling back to auto-discovery.\n", topOverride.c_str());
        }

        gdstk::Array<gdstk::Cell*> tops = {};
        gdstk::Array<gdstk::RawCell*> raw_tops = {};
        glib.top_level(tops, raw_tops);
        if (tops.count == 0) {
            printf("Error: top cell not found.");
            return nullptr;
        }
        gdstk::Cell * top = tops[0];
        printf("Top cell: %s\n", top->name);
        tops.clear(); raw_tops.clear();
        return top;
    }

    void detectPDK() {
        if (!pdk.empty()) {
            printf("PDK: %s\n", pdk.c_str());
            return;
        }

        const auto& pdkMaps = getPdkMaps();
        struct Hits {int count=0; int mask=0; };
        std::map<std::string, Hits> pdkHits;
        pdk = "sky130A";  // fallback
        bool done=false;
        int maxHits = 0;

        for (uint64_t i = 0; i < glib.cell_array.count && !done; ++i) {
            gdstk::Cell* cell = glib.cell_array[i];
            for (uint64_t j = 0; j < cell->polygon_array.count && !done; ++j) {
                const  gdstk::Polygon* poly = cell->polygon_array[j];
                for (const auto& [name, tagMap] : pdkMaps) {
                    LayerID layer = tag2id(poly->tag, name);
                    if (layer == L_COUNT) continue;
                    if (pdkHits[name].mask & (1<<layer)) continue;
                    pdkHits[name].mask |= 1<<layer;
                    maxHits = std::max(++pdkHits[name].count, maxHits);
                    if (maxHits > 5) {
                        done = true;
                        pdk = name;
                    }
                }
            }
        }
        if (done) {
            printf("Auto-detected PDK: %s\n", pdk.c_str());
        } else {
            printf("PDK detection uncertain, defaulting to %s\n", pdk.c_str());    
        }
    }

    void preprocessCells() {
        CellProcessor processor;
        cells.resize(glib.cell_array.count);
        for (CellID cell_id=0; cell_id<cells.size(); ++cell_id) {
            gdstk::Cell* gcell = glib.cell_array[cell_id];
            gcell2id[gcell] = cell_id;
            Cell & cell = cells[cell_id];
            processor.run(gcell, cell, pdk);
        }
    }

    void walkRefs(gdstk::Cell* gcell, const Transform & tform) {
        CellID cell_id = gcell2id[gcell];
        const Cell & cell = cells[cell_id];
        InstID inst_id = instances.size();
        instances.push_back({tform.apply(cell.bbox), cell_id, inst_id, tform});
        // these will be used for per-layer instance intersection queries
        for (int li=0; li<L_COUNT; ++li) {
            if (cell.layers[li].rectCount == 0) continue;
            Rect bbox = cell.layers[li].bvh[0].bbox;
            instLayers[li].instances.push_back({tform.apply(bbox), cell_id, inst_id, tform});
        }

        for (int i=0; i<gcell->reference_array.count; ++i) {
            const gdstk::Reference * ref = gcell->reference_array[i];
            if (ref->type != gdstk::ReferenceType::Cell) continue;
            
            if (!Transform::isSupported(*ref)) {
                printf("Warning: skipping unsupported reference in cell %s (rotation=%.2f, mag=%.2f, origin=(%.2f, %.2f))\n",
                    gcell->name, ref->rotation, ref->magnification, ref->origin.x, ref->origin.y);
                continue;
            }

            Transform child_tform(*ref);
            walkRefs(ref->cell, tform.compose(child_tform));
        }
    }

    void wireCells() {
        instOffsets.resize(instances.size());
        int totalSegments = 0;
        for (size_t i = 0; i < instances.size(); i++) {
            instOffsets[i] = totalSegments;
            totalSegments += (int)cells[instances[i].cell_id].wireCount;
        }
        segemntDSU.reset(totalSegments);

        for (int i=L_N_TERM; i<L_COUNT; ++i) {
            auto & layer = instLayers[i];
            if (layer.instances.empty()) continue;
            buildLayerBVH(layer.instances, 0, layer.instances.size(), layer.bvh);
            int overlapCount = 0;
            collideSelf(layer.bvh, layer.instances, [&](int inst_a, int inst_b) {
                ++overlapCount;
                wirePair(i, inst_a, inst_b);
            });
            printf("%9s - instN: %zu, overlapCount: %d\n", getLayerName((LayerID)i), layer.instances.size(), overlapCount);
        }
    }

    void wirePair(int lid, int idxA, int idxB) {
        const auto &instA = instLayers[lid].instances[idxA];
        const auto &instB = instLayers[lid].instances[idxB];
        Cell &cellA = cells[instA.cell_id];
        Cell &cellB = cells[instB.cell_id];
        const auto &qA = cellA.layers[lid];
        const auto &qB = cellB.layers[lid];
        const int offsA = instOffsets[instA.inst_id];
        const int offsB = instOffsets[instB.inst_id];

        auto pred = [&](const Rect& ra, const Rect& rb) {
            return touches(instA.tform.apply(ra), instB.tform.apply(rb));
        };

        collideTrees(qA.bvh, cellA.rects, qB.bvh, cellB.rects, [&](int rA, int rB) {
            int wireA = cellA.rect2wire[rA];
            int wireB = cellB.rect2wire[rB];
            segemntDSU.unite(offsA + wireA, offsB + wireB);
        }, pred);
    }

    void buildNetlist() {
        segment2flat.assign(segemntDSU.p.size(), DSU::NeedsID);
        if (segemntDSU.find(0) == segemntDSU.find(1)) {
            printf("WARNING: GND and PWR are merged in DSU!\n");
        }
        segment2flat[segemntDSU.find(0)] = 0; // GND
        segment2flat[segemntDSU.find(1)] = 1; // PWR
        
        int next_id = 2;
        const Cell& top = cells[instances[0].cell_id];  
        for (auto const& [name, l] : top.labels) {
            int root = segemntDSU.find(top.rect2wire[l.rectIdx]);
            if (segment2flat[root] == DSU::NeedsID) {
                segment2flat[root] = next_id++;
            }
            // User requested: Only top cell labels MET3 and above propagate to UI
            if (l.layerId >= L_MET2) {
                labeledWires.push_back({segment2flat[root], name, l.layerId});
            }
        }

        int wireCount = segemntDSU.assign_ids(segment2flat, next_id);
        for (size_t inst_id = 0; inst_id < instances.size(); inst_id++) {
            const Cell& cell = cells[instances[inst_id].cell_id];
            int offset = instOffsets[inst_id];
            for (const auto& fet : cell.fets) {
                int gate = segment2flat[offset + fet.gate];
                int t0 = segment2flat[offset + fet.term[0]];
                int t1 = segment2flat[offset + fet.term[1]];
                builder.add_fet(gate, t0, t1, fet.type, inst_id);
            }
        }

        circuit = builder.build();
        wire2root = builder.treeDSU.p;
    }

    void exportRects() {
        printf("Flattening and optimizing layers...\n");
        flatRects.clear();
        flatLayerOffsets.fill(0);
        
        std::vector<RectWire> layerTemp;
        for (int li = 0; li < L_COUNT; li++) {
            flatLayerOffsets[li] = (uint32_t)flatRects.size();
            if (li == L_DIFF || li == L_NWELL) continue;
            
            layerTemp.clear();
            for (size_t ii = 0; ii < instances.size(); ii++) {
                const auto& inst = instances[ii];
                const auto& cell = cells[inst.cell_id];
                if (cell.isFiller) continue;
                const auto& layer = cell.layers[li];
                if (layer.rectCount == 0) continue;
                
                for (uint32_t ri = 0; ri < layer.rectCount; ri++) {
                    uint32_t rectIdx = layer.rectStart + ri;
                    int flatWire = -1; // some rects don't have wire_id (NWELL)
                    int treeId = -1;
                    int localWire = cell.rect2wire[rectIdx];
                    if (localWire >= 0) {
                        flatWire = segment2flat[instOffsets[ii] + localWire];
                        treeId = wire2root[flatWire];
                    }
                    Rect r = inst.tform.apply(cell.rects[rectIdx]);
                    layerTemp.push_back({r, flatWire, treeId});
                }
            }
            
            if (!layerTemp.empty()) {
                int discarded = optimizeRects(layerTemp);
                int initial = (int)layerTemp.size() + discarded;
                float pct = initial > 0 ? (float)discarded * 100.0f / initial : 0.0f;
                printf("  %-10s: %zu rects (%d discarded, %.1f%%)\n", getLayerName((LayerID)li), layerTemp.size(), discarded, pct);
                flatRects.insert(flatRects.end(), layerTemp.begin(), layerTemp.end());
                buildLayerBVH(flatRects, flatLayerOffsets[li], layerTemp.size(), flatBVHs[li]);
            }
        }
        flatLayerOffsets[L_COUNT] = (uint32_t)flatRects.size();
    }


    void printStats() {
        printf("Total flat rects: %zu\n", flatRects.size());
        printf("Total instances: %zu\n", instances.size());
        printf("Global Netlist Statistics:\n  Segments: %zu\n  Wires: %zu\n  FETs: %zu\n  INVs: %d\n", 
               segment2flat.size(), circuit.wire_n(), builder.fets.size(), builder.inverter_n);
    }

    std::vector<std::pair<InstID, int>> wire_to_instances(int globalWire) {
        std::vector<std::pair<InstID, int>> results;
        if (globalWire < 0) return results;
        for (int i = 0; i < (int)instances.size(); ++i) {
            int start = instOffsets[i];
            int count = (int)cells[instances[i].cell_id].wireCount;
            for (int local = 0; local < count; ++local) {
                if (segment2flat[start + local] == globalWire) {
                    results.push_back({(InstID)i, local});
                }
            }
        }
        return results;
    }

    void printWire(int globalWire) {
        auto instances_info = wire_to_instances(globalWire);
        printf("Wire %d connections (%zu segments):\n", globalWire, 
               std::count(segment2flat.begin(), segment2flat.end(), globalWire));
        for (const auto& [inst_id, local_wire] : instances_info) {
            const Instance& inst = instances[inst_id];
            const Cell& cell = cells[inst.cell_id];
            
            std::string label = "";
            for (const auto& [name, lbl] : cell.labels) {
                if (cell.rect2wire[lbl.rectIdx] == local_wire) {
                    if (!label.empty()) label += ", ";
                    label += name;
                }
            }
            
            if (label.empty()) {
                printf("  Inst %d (%s): wire %d\n", inst_id, cell.name.c_str(), local_wire);
            } else {
                printf("  Inst %d (%s): %s\n", inst_id, cell.name.c_str(), label.c_str());
            }
        }
    }


    ~CircuitExtractor() {
        glib.free_all();
    }
};
