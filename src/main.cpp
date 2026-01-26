// g++ -std=c++17 -O3 main.cpp -o main && ./main gds/09_tt_um_znah_vga_ca.gds

#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <tuple>
#include <set>

#include "geom.h"
#include "cells.h"
#include "parser.h"
#include "fetsim.h"
#include "cell_processing.h"


// MARK: - Circuit Export

struct FlattenedData {
    std::vector<RectWire> rectData; // Flattened [x1, y1, x2, y2, wire, ...]
    uint32_t layerOffsets[L_COUNT + 1];
    std::vector<std::string> topWireLabels;
    Circuit circuit;
};

struct CircuitFlattener {
    CellLibrary& lib;
    CircuitBuilder builder;
    std::vector<RectWire> flatLayers[L_COUNT];

    CircuitFlattener(CellLibrary& lib) : lib(lib) {}

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

FlattenedData flattenCircuit(CellLibrary& lib) {
    FlattenedData out;
    auto it = lib.cellMap.find(lib.topCellName);
    Cell& topCell = lib.cells[it->second];
    
    CircuitFlattener flattener(lib);
    // DSU root -> export wire id
    auto topWireMap = initializeTopWires(topCell, flattener.builder);
    flattener.flatten(topCell, topWireMap, "", Transformer());
    optimizeFlattenedGeometry(flattener.flatLayers, out);
    out.circuit = flattener.builder.build();

    return out;
}

// MARK: - Main

struct ParserState {
    CellLibrary lib;
    FlattenedData flat;
    GDSParser parser;

    ParserState() : parser(lib, tryProcessCell) {}

    void push_chunk(const uint8_t* data, size_t len) {
        parser.consumeChunk(data, len);
    }

    void finalize() {
        if (!parser.isFinished) {
            fprintf(stderr, "Error: GDS truncated");
            return;
        }
        
        // process leftover cells
        std::vector<int> visited(lib.cells.size(), 0);
        std::function<void(size_t)> visit = [&](size_t idx) {
            if (lib.cells[idx].isProcessed) return;
            if (visited[idx] != 0) {
                fprintf(stderr, "Error: GDS reference loop detected!");
                return;
            } 
            visited[idx] = 1;
            for (const auto& ref : lib.cells[idx].references) {
                auto it = lib.cellMap.find(ref.cellName);
                if (it != lib.cellMap.end()) {
                    visit(it->second);
                }
            }
            tryProcessCell(lib.cells[idx], lib);
        };
        for (size_t i = 0; i < lib.cells.size(); ++i) {visit(i);}

        // find top cell
        for (const auto& cell : lib.cells) {
            if (cell.isTop) {
                lib.topCellName = cell.name;
                break;
            }
        }

        flat = flattenCircuit(lib);

        // print stats
        printf("Library: %s\n", lib.name.c_str());
        printf("Units: User=%g, DB=%g\n", lib.userUnit, lib.dbUnit);
        printf("Total Cells: %zu\n\n", lib.cells.size());
        printf("Top cell: %s\n\n", lib.topCellName.c_str());
        printf("Stats: FlatRects=%zu FlatFETs=%zu FlatWires=%zu\n",
            flat.rectData.size(), flat.circuit.fet_n(), flat.circuit.wire_n());
    }

};

ParserState * wasm_state = nullptr;

#ifdef __wasm__
#define WASM_EXPORT(name) __attribute__((export_name(name)))
#else 
#define WASM_EXPORT(name)
#endif

extern "C" {
    WASM_EXPORT("wasm_malloc") void* wasm_malloc(size_t size) { return malloc(size); }
    WASM_EXPORT("wasm_free") void wasm_free(void* ptr) { free(ptr); }

    WASM_EXPORT("wasm_init") void wasm_init() {
        if (wasm_state) delete wasm_state;
        wasm_state = new ParserState();
        setvbuf(stdout, NULL, _IONBF, 0);
    }

    WASM_EXPORT("wasm_push_chunk") void wasm_push_chunk(const uint8_t* data, size_t len) {
        if (wasm_state) wasm_state->push_chunk(data, len);
    }

    WASM_EXPORT("wasm_finalize") void wasm_finalize() {
        if (!wasm_state) return;
        wasm_state->finalize();
    }

    WASM_EXPORT("wasm_get_rect_data_ptr") int32_t* wasm_get_rect_data_ptr() {
        return wasm_state ? (int32_t*)wasm_state->flat.rectData.data() : nullptr;
    }
    WASM_EXPORT("wasm_get_rect_data_size") uint32_t wasm_get_rect_data_size() {
        return wasm_state ? wasm_state->flat.rectData.size() * RECT_WIRE_FIELDS : 0;
    }
    WASM_EXPORT("wasm_get_layer_offsets_ptr") uint32_t* wasm_get_layer_offsets_ptr() {
        return wasm_state ? (uint32_t*)wasm_state->flat.layerOffsets : nullptr;
    }
    WASM_EXPORT("wasm_get_layer_offsets_size") uint32_t wasm_get_layer_offsets_size() {
        return wasm_state ? L_COUNT + 1 : 0;
    }
}

