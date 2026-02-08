#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <tuple>
#ifndef WASM
#include <fstream>
#include <sstream>
#endif

#include "gdstk/cell.hpp"
#include "gdstk/reference.hpp"
#include "gdstk/utils.hpp"
#include <sys/stat.h>

#include "geom.h"
#include "cells.h"
#include "cell_processing.h"

#include <gdstk/gdstk.hpp>

#ifdef WASM
extern "C" {
    void __cxa_throw(void* ex, void* info, void (*dest)(void*)) { abort(); }
    void* __cxa_allocate_exception(size_t size) { return malloc(size); }
    void __cxa_free_exception(void* p) { free(p); }
}
#endif

extern "C" {
void wasm_arena_init(uint32_t mode);
uint64_t wasm_arena_get_usage();
void* wasm_malloc(size_t size);
void wasm_free(void* ptr);
}

using CellID = int;
using InstID = int;
struct Instance : Rect {
    CellID cell_id;
    InstID inst_id;
    Transform tform;
};

struct RectWire : Rect { int wire; };
constexpr int RECT_WIRE_FIELDS = 5; // x1, y1, x2, y2, wire

struct WireLink {
    InstID inst_a, inst_b;
    int wire_a, wire_b;
    auto tie() const { return std::tie(inst_a, inst_b, wire_a, wire_b); }
    bool operator<(const WireLink& o) const { return tie() < o.tie(); }
    bool operator==(const WireLink& o) const { return tie() == o.tie(); }
};

struct CircuitExtractor {
    gdstk::Library glib;

    std::vector<Cell> cells;
    std::string pdk = "sky130A"; // default
    std::string topOverride;
    std::map<const gdstk::Cell*, int> gcell2id;

    std::vector<Instance> instances; // just for viz

    struct InstLayer {
        std::vector<Instance> instances;
        std::vector<BVHNode> bvh;
    };
    std::array<InstLayer, L_COUNT> instLayers;
    Circuit circuit;

    std::vector<int> instOffsets;
    std::vector<int> segment2flat;
    DSU globalDSU;
    std::vector<std::pair<int, std::string>> labeledWires;

    std::vector<RectWire> flatRects;
    std::array<uint32_t, L_COUNT + 1> flatLayerOffsets;

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
        preprocessCells();
        const Cell topCell = cells[gcell2id[top]];
        if (topCell.groundRect == -1 || topCell.powerRect == -1) {
            fprintf(stderr, "Error: missing power/ground labels in top cell\n");
            return false;
        }
        
        walkRefs(top, Transform());
        wireCells();
        buildNetlist();

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

    void preprocessCells() {
        fracture_cell_func fracture_polygons;
        cells.resize(glib.cell_array.count);
        for (CellID cell_id=0; cell_id<cells.size(); ++cell_id) {
            gdstk::Cell* gcell = glib.cell_array[cell_id];
            gcell2id[gcell] = cell_id;
            Cell & cell = cells[cell_id];
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
        unique_sort(wires);
    }

    std::vector<WireLink> wires;

    void wirePair(int lid, int idxA, int idxB) {
        const auto &instA = instLayers[lid].instances[idxA];
        const auto &instB = instLayers[lid].instances[idxB];
        Cell &cellA = cells[instA.cell_id];
        Cell &cellB = cells[instB.cell_id];
        const auto &qA = cellA.layers[lid];
        const auto &qB = cellB.layers[lid];

        auto pred = [&](const Rect& ra, const Rect& rb) {
            return touches(instA.tform.apply(ra), instB.tform.apply(rb));
        };

        collideTrees(qA.bvh, cellA.rects, qB.bvh, cellB.rects, [&](int rA, int rB) {
            int wA = cellA.rect2wire[rA];
            int wB = cellB.rect2wire[rB];
            
            WireLink link = {instA.inst_id, instB.inst_id, wA, wB};
            if (link.inst_a > link.inst_b || (link.inst_a == link.inst_b && link.wire_a > link.wire_b)) {
                std::swap(link.inst_a, link.inst_b);
                std::swap(link.wire_a, link.wire_b);
            }
            wires.push_back(link);
        }, pred);
    }

    void buildNetlist() {
        instOffsets.resize(instances.size());
        int totalSegments = 0;
        for (size_t i = 0; i < instances.size(); i++) {
            instOffsets[i] = totalSegments;
            totalSegments += (int)cells[instances[i].cell_id].wireCount;
        }

        globalDSU = DSU(totalSegments);
        for (const auto& link : wires) {
            globalDSU.unite(instOffsets[link.inst_a] + link.wire_a,
                           instOffsets[link.inst_b] + link.wire_b);
        }

        segment2flat.assign(globalDSU.p.size(), DSU::NeedsID);
        segment2flat[globalDSU.find(0)] = 0; // GND
        segment2flat[globalDSU.find(1)] = 1; // PWR
        
        int next_id = 2;
        const Cell& top = cells[instances[0].cell_id];
        for (auto const& [name, rIdx] : top.label2rect) {
            int root = globalDSU.find(top.rect2wire[rIdx]);
            if (segment2flat[root] == DSU::NeedsID) {
                segment2flat[root] = next_id++;
            }
            labeledWires.push_back({segment2flat[root], name});
        }

        int wireCount = globalDSU.assign_ids(segment2flat, next_id);

        CircuitBuilder builder;
        for (size_t i = 0; i < instances.size(); i++) {
            const Cell& cell = cells[instances[i].cell_id];
            int offset = instOffsets[i];
            for (const auto& fet : cell.fets) {
                builder.add_fet(segment2flat[offset + fet.gate],
                               segment2flat[offset + fet.term[0]],
                               segment2flat[offset + fet.term[1]], fet.type);
            }
        }

        circuit = builder.build();
        for (auto const& lw : labeledWires) {
            printf("Net %d (%s): %zu gates, %zu terms\n", lw.first, lw.second.c_str(), 
                   circuit.gate_to_nets[lw.first].size(), circuit.net_connectivity[lw.first].size());
        }
        printf("Global Netlist Statistics:\n  Segments: %d\n  Wires: %d\n  FETs: %zu\n", 
               totalSegments, wireCount, circuit.fet_n());

        exportRects();
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
                    int localWire = cell.rect2wire[rectIdx];
                    if (localWire >= 0) {
                        flatWire = segment2flat[instOffsets[ii] + localWire];
                    }
                    Rect r = inst.tform.apply(cell.rects[rectIdx]);
                    layerTemp.push_back({r, flatWire});
                }
            }
            
            if (!layerTemp.empty()) {
                int discarded = optimizeRects(layerTemp);
                int initial = (int)layerTemp.size() + discarded;
                float pct = initial > 0 ? (float)discarded * 100.0f / initial : 0.0f;
                flatRects.insert(flatRects.end(), layerTemp.begin(), layerTemp.end());
                printf("  %-10s: %zu rects (%d discarded, %.1f%%)\n", getLayerName((LayerID)li), layerTemp.size(), discarded, pct);
            }
        }
        flatLayerOffsets[L_COUNT] = (uint32_t)flatRects.size();
    }


    void printStats() {
        printf("Total flat rects: %zu\n", flatRects.size());
        printf("Total instances: %zu\n", instances.size());
        printf("Total crosscell wires: %zu\n", wires.size());
    }    

    ~CircuitExtractor() {
        glib.free_all();
    }
};



CircuitExtractor* g_extractor = nullptr;

extern "C" {
    void wasm_init() {
        if (!g_extractor) {
            g_extractor = new CircuitExtractor();
        }
    }

    bool wasm_load_file(const char* path, const char* pdk) {
        if (!g_extractor) return false;
        g_extractor->pdk = pdk;
        return g_extractor->load(path);
    }

    bool wasm_process() {
        if (!g_extractor) return false;
        return g_extractor->process();
    }

    void* wasm_get_rect_data_ptr() {
        return g_extractor ? g_extractor->flatRects.data() : nullptr;
    }

    uint32_t wasm_get_rect_data_size() {
        return g_extractor ? (uint32_t)(g_extractor->flatRects.size() * sizeof(RectWire)) : 0;
    }

    void* wasm_get_layer_offsets_ptr() {
        return g_extractor ? (void*)g_extractor->flatLayerOffsets.data() : nullptr;
    }

    uint32_t wasm_get_layer_offsets_size() {
        return g_extractor ? (uint32_t)(g_extractor->flatLayerOffsets.size() * sizeof(uint32_t)) : 0;
    }

    // Circuit Simulation API
    uint32_t wasm_circuit_get_wire_count() {
        return g_extractor ? (uint32_t)g_extractor->circuit.wire_n() : 0;
    }

    uint32_t wasm_circuit_get_labeled_count() {
        return g_extractor ? (uint32_t)g_extractor->labeledWires.size() : 0;
    }

    int wasm_circuit_get_labeled_id(uint32_t idx) {
        if (g_extractor && idx < g_extractor->labeledWires.size()) {
            return g_extractor->labeledWires[idx].first;
        }
        return -1;
    }

    const char* wasm_circuit_get_labeled_name(uint32_t idx) {
        if (g_extractor && idx < g_extractor->labeledWires.size()) {
            return g_extractor->labeledWires[idx].second.c_str();
        }
        return nullptr;
    }

    uint32_t wasm_circuit_get_fet_count() {
        return g_extractor ? (uint32_t)g_extractor->circuit.fet_n() : 0;
    }

    void wasm_circuit_set_input(uint32_t wire, uint32_t val) {
        if (g_extractor && wire < g_extractor->circuit.wire_n()) {
            g_extractor->circuit.set_input(wire, (uint8_t)val);
        }
    }

    int wasm_circuit_run_wave() {
        return g_extractor ? g_extractor->circuit.run_wave() : 0;
    }

    void* wasm_circuit_get_wire_data_ptr() {
        return g_extractor ? g_extractor->circuit.wire_data.data() : nullptr;
    }

    void* wasm_circuit_get_fet_on_ptr() {
        return nullptr; // fet_on array removed
    }

    int wasm_circuit_get_short_count() {
        return g_extractor ? g_extractor->circuit.short_count : 0;
    }
}

#ifdef WASM
int main() { return 0; }
#endif




#ifndef WASM
void save_dot(const Circuit& c, const std::string& filename, const std::vector<std::pair<int, std::string>>& labels) {
    std::ofstream f(filename);
    f << "digraph G {\n";
    f << "  rankdir=LR;\n";
    f << "  nodesep=0.4; ranksep=0.6;\n";
    f << "  node [fontname=\"Inter\", fontsize=10, style=filled, fillcolor=\"#161a21\", color=\"#2a2f3a\", fontcolor=\"#e0e0e0\"];\n";
    f << "  edge [color=\"#444\", penwidth=1.0];\n";

    std::map<int, std::string> wire_labels;
    for (const auto& l : labels) wire_labels[l.first] = l.second;

    // Define Wires
    for (int i = 0; i < c.wire_n(); ++i) {
        if (i < 2) continue; // Skip VGND/VPWR
        std::string label = wire_labels.count(i) ? wire_labels[i] : "w" + std::to_string(i);
        bool is_io = wire_labels.count(i) > 0;
        
        std::string shape = is_io ? "square" : "circle";
        std::string extras = "";
        if (is_io) extras = ", fillcolor=\"#161a21\", color=\"#3d5afe\", fontcolor=\"#e0e0e0\", penwidth=2";
        else extras = ", fillcolor=\"#161a21\", color=\"#2a2f3a\", fontcolor=\"#808080\", width=0.2, height=0.2, fixedsize=true, fontsize=6";
        
        f << "  w" << i << " [label=\"" << label << "\", shape=" << shape << extras << ", id=\"w" << i << "\"];\n";
    }


    // Define FETs
    for (size_t i = 0; i < c.fets.size(); ++i) {
        const auto& fet = c.fets[i];
        bool is_n = (fet.type == FET::N);
        std::string color = is_n ? "#4caf50" : "#ff5252";
        
        f << "  f" << i << " [shape=circle, label=\"\", fillcolor=\"" << color << "\", color=\"" << color << "\", width=0.15, height=0.15, id=\"f" << i << "\"];\n";
        
        // Terminals
        if (fet.term[0] >= 2)
            f << "  f" << i << " -> w" << fet.term[0] << " [arrowhead=none];\n";
        if (fet.term[1] >= 2)
            f << "  f" << i << " -> w" << fet.term[1] << " [arrowhead=none];\n";
            
        // Gate
        if (fet.gate >= 2)
            f << "  w" << fet.gate << " -> f" << i << " [dir=both, arrowhead=tee, arrowtail=none, color=\"#888\", weight=2];\n";
    }

    f << "}\n";
}
#endif


struct VGASimulator {

    VGASimulator(Circuit& c, const std::vector<std::pair<int, std::string>>& labels) 
        : circuit(c) {
        for (const auto& [id, name] : labels) {
            name2id[name] = id;
        }
        
        // Find essential pins
        clk = getPin("clk");
        rst_n = getPin("rst_n");
        ena = getPin("ena");
        
        for (int i = 0; i < 8; ++i) {
            out_pins[i] = getPin("uo_out[" + std::to_string(i) + "]");
        }

        vga_buffer.assign(width * height * 3, 0);
    }

    bool isValid() const { return clk != -1 && rst_n != -1; }

    void run(int max_ticks = 1000000) {
        if (!isValid()) return;
        
        printf("VGA Sim: clk=%d, rst_n=%d, outputs=[", clk, rst_n);
        for(int i=0; i<8; ++i) printf("%d%s", out_pins[i], i==7 ? "]\n" : ",");

        if (ena != -1) circuit.set_input(ena, 1);
        
        printf("Resetting...\n");
        circuit.set_input(rst_n, 0);
        for (int i = 0; i < 10; ++i) tick();
        
        printf("Running simulation...\n");
        circuit.set_input(rst_n, 1);
        
        auto t_start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < max_ticks; ++i) {
            tick();
            if ((i + 1) % 10000 == 0) {
                auto t_now = std::chrono::high_resolution_clock::now();
                double elapsed = std::chrono::duration<double>(t_now - t_start).count();
                uint8_t out = 0;
                for (int j=0; j<8; j++) {
                    if (out_pins[j] != -1 && (circuit.wire_data[out_pins[j]] & Circuit::V_MASK)) out |= (1 << j);
                }
                printf("  Tick %d (%.0f px/s), ray at %d,%d, out=%02x  shorts=%d\n",
                    i + 1, (i + 1) / elapsed, ray_x, ray_y, out, circuit.short_count);
                savePPM("vga.ppm");
            }
        }
        savePPM("vga.ppm");
    }

    int getPin(const std::string& name) {
        return name2id.count(name) ? name2id.at(name) : -1;
    }

    bool settle(int max_settle_waves=100) {
        int wave = 0;
        while (!circuit.is_settled() && wave < max_settle_waves) {
            int sn = circuit.run_wave();
            ++wave;
        }
        //circuit.validate();
        return circuit.is_settled();
    }


    void tick() {
        circuit.set_input(clk, 0); settle();
        circuit.set_input(clk, 1); settle();

        // Sample outputs (assuming TinyTapeout pinout)
        // uo_out[0..2] = R1, G1, B1
        // uo_out[3] = VSync
        // uo_out[4..6] = R0, G0, B0
        // uo_out[7] = HSync

        auto val = [&](int pin_idx) {
            if (out_pins[pin_idx] == -1) return false;
            return (bool)(circuit.wire_data[out_pins[pin_idx]] & Circuit::V_MASK);
        };

        bool r1 = val(0), g1 = val(1), b1 = val(2), vsync = val(3);
        bool r0 = val(4), g0 = val(5), b0 = val(6), hsync = val(7);

        // Sync logic
        if (!hsync && last_hsync) { ray_x = 0; ray_y++; }
        if (!vsync && last_vsync) { ray_x = 0; ray_y = 0; }
        
        if (ray_x < width && ray_y < height) {
            int idx = (ray_y * width + ray_x) * 3;
            vga_buffer[idx + 0] = (r1 * 2 + r0) * 85;
            vga_buffer[idx + 1] = (g1 * 2 + g0) * 85;
            vga_buffer[idx + 2] = (b1 * 2 + b0) * 85;
        }
        
        ray_x++;
        last_hsync = hsync;
        last_vsync = vsync;
    }

    void savePPM(const char* filename) {
        FILE* f = fopen(filename, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        fwrite(vga_buffer.data(), 1, vga_buffer.size(), f);
        fclose(f);
    }

    Circuit& circuit;
    std::map<std::string, int> name2id;
    int clk, rst_n, ena;
    int out_pins[8];
    
    int width = 1000, height = 600;
    int ray_x = 0, ray_y = 0;
    bool last_hsync = false, last_vsync = false;
    std::vector<uint8_t> vga_buffer;
};

struct SimStep {
    std::vector<uint8_t> wires;
    std::vector<uint8_t> fets;
    std::vector<int> flipped;
    std::string description;
};


#ifndef WASM
void save_html(const std::string& filename, const std::string& svg_content, const std::vector<SimStep>& trace, const std::vector<std::pair<int, std::string>>& labels, const std::vector<FET>& fets) {
    std::ofstream f(filename);
    f << "<html><head><title>Circuit Simulation Trace</title>";
    f << "<link href='https://fonts.googleapis.com/css2?family=Inter:wght@400;600&family=JetBrains+Mono&display=swap' rel='stylesheet'>\n";
    f << "<style>\n";
    f << "  :root { --bg: #0f1115; --sidebar: #161a21; --accent: #3d5afe; --text: #e0e0e0; --dim: #808080; --border: #2a2f3a; }\n";
    f << "  body { font-family: 'Inter', sans-serif; background: var(--bg); color: var(--text); display: flex; flex-direction: column; height: 100vh; margin: 0; overflow: hidden; }\n";
    f << "  header { background: var(--sidebar); border-bottom: 1px solid var(--border); padding: 10px 20px; display: flex; align-items: center; justify-content: space-between; z-index: 100; }\n";
    f << "  #container { display: flex; flex: 1; overflow: hidden; }\n";
    f << "  #svg-container { flex: 1; overflow: auto; padding: 40px; display: flex; align-items: flex-start; justify-content: center; background-image: radial-gradient(var(--border) 1px, transparent 1px); background-size: 20px 20px; }\n";
    f << "  #sidebar { width: 350px; background: var(--sidebar); border-left: 1px solid var(--border); display: flex; flex-direction: column; }\n";
    f << "  .sidebar-content { flex: 1; overflow-y: auto; padding: 20px; }\n";
    f << "  #controls { display: flex; gap: 12px; align-items: center; }\n";
    f << "  .btn { background: var(--accent); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; cursor: pointer; font-weight: 600; font-family: inherit; transition: opacity 0.2s; }\n";
    f << "  .btn:hover { opacity: 0.9; }\n";
    f << "  .btn:disabled { background: var(--border); color: var(--dim); cursor: not-allowed; }\n";
    f << "  h1 { font-size: 18px; margin: 0; }\n";
    f << "  h3 { font-size: 14px; color: var(--dim); text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 12px; }\n";
    f << "  table { width: 100%; border-collapse: collapse; font-family: 'JetBrains Mono', monospace; font-size: 12px; }\n";
    f << "  th, td { text-align: left; padding: 8px; border-bottom: 1px solid var(--border); }\n";
    f << "  .val { font-weight: bold; padding: 2px 6px; border-radius: 4px; }\n";
    f << "  .high { color: #ff5252; background: rgba(255, 82, 82, 0.1); }\n";
    f << "  .low { color: #448aff; background: rgba(68, 138, 255, 0.1); }\n";
    f << "  .flipped { background: rgba(255, 235, 59, 0.05); }\n";
    f << "  .flipped .net-name { color: #ffeb3b; font-weight: bold; }\n";
    f << "  svg { max-width: 100%; height: auto; }\n";
    f << "  .node ellipse, .node circle { transition: fill 0.3s, stroke 0.3s, stroke-width 0.3s; stroke-width: 1px; }\n";
    f << "  .node polygon, .node rect { transition: fill 0.3s, stroke 0.3s, stroke-width 0.3s; stroke-width: 1px; }\n";
    f << "  svg > g > polygon { fill: transparent !important; }\n";
    f << "  svg text { fill: var(--text) !important; font-family: 'Inter', sans-serif !important; font-size: 8px !important; pointer-events: none; }\n";
    f << "  #step-info-card { background: var(--border); padding: 15px; border-radius: 8px; margin-bottom: 20px; border-left: 4px solid var(--accent); }\n";

    f << "  #step-info-card div { font-size: 14px; margin-bottom: 4px; }\n";
    f << "</style></head><body>\n";

    f << "<header>\n";
    f << "  <h1>Circuit Simulation Trace</h1>\n";
    f << "  <div id='controls'>\n";
    f << "    <button id='prevBtn' class='btn' onclick='go(-1)'>&larr; Previous Wave</button>\n";
    f << "    <div style='min-width: 120px; text-align: center; font-weight: 600;' id='step-display'>Step 1 / 1</div>\n";
    f << "    <button id='nextBtn' class='btn' onclick='go(1)'>Next Wave &rarr;</button>\n";
    f << "  </div>\n";
    f << "</header>\n";

    f << "<div id='container'>\n";
    f << "  <div id='svg-container'>" << svg_content << "</div>\n";
    f << "  <div id='sidebar'>\n";
    f << "    <div class='sidebar-content'>\n";
    f << "      <div id='step-info-card'>\n";
    f << "        <div style='font-weight: 600; color: #fff;'>Description</div>\n";
    f << "        <div id='step-desc'>-</div>\n";
    f << "      </div>\n";
    f << "      <h3>Jumped Wires</h3>\n";
    f << "      <div id='jumped-container' style='margin-bottom: 30px; line-height: 1.6;'>\n";
    f << "        <div id='jumped-list' style='display: flex; flex-wrap: wrap; gap: 8px;'></div>\n";
    f << "      </div>\n";
    f << "      <h3>Net State</h3>\n";
    f << "      <table id='net-table'></table>\n";
    f << "    </div>\n";
    f << "  </div>\n";
    f << "</div>\n";

    f << "<script>\n";
    f << "const trace = " << [&]() {
        std::stringstream ss;
        ss << "[\n";
        for (const auto& step : trace) {
            ss << "  {desc:\"" << step.description << "\", wires:[";
            for (size_t i = 0; i < step.wires.size(); ++i) ss << (int)(step.wires[i] & 1) << (i == step.wires.size() - 1 ? "" : ",");
            ss << "], fets:[";
            for (size_t i = 0; i < step.fets.size(); ++i) ss << (int)step.fets[i] << (i == step.fets.size() - 1 ? "" : ",");
            ss << "], flipped:[";
            for (size_t i = 0; i < step.flipped.size(); ++i) ss << step.flipped[i] << (i == step.flipped.size() - 1 ? "" : ",");
            ss << "]},\n";
        }
        ss << "]";
        return ss.str();
    }() << ";\n";


    f << "const labels = " << [&]() {
        std::stringstream ss;
        ss << "{";
        for (const auto& l : labels) ss << l.first << ":\"" << l.second << "\",";
        ss << "}";
        return ss.str();
    }() << ";\n";

    f << "const fetTypes = " << [&]() {
        std::stringstream ss;
        ss << "[";
        for (const auto& fet : fets) ss << (int)fet.type << ",";
        ss << "]";
        return ss.str();
    }() << ";\n";

    f << "let currentStep = 0;\n";
    f << "function update() {\n";
    f << "  const step = trace[currentStep];\n";
    f << "  document.getElementById('step-display').innerText = `Step ${currentStep + 1} / ${trace.length}`;\n";
    f << "  document.getElementById('step-desc').innerText = step.desc;\n";
    f << "  document.getElementById('prevBtn').disabled = currentStep === 0;\n";
    f << "  document.getElementById('nextBtn').disabled = currentStep === trace.length - 1;\n";
    f << "  \n";
    f << "  // Update SVG\n";
    f << "  step.wires.forEach((v, i) => {\n";
    f << "    const el = document.getElementById('w' + i);\n";
    f << "    if (el) {\n";
    f << "      el.querySelectorAll('ellipse, circle, polygon, rect').forEach(shape => {\n";
    f << "        shape.style.fill = v ? '#ff5252' : '#448aff';\n";
    f << "        shape.style.stroke = v ? '#ff8a80' : '#82b1ff';\n";
    f << "        shape.style.strokeWidth = v ? '2px' : '1px';\n";
    f << "      });\n";
    f << "    }\n";
    f << "  });\n";

    f << "  step.fets.forEach((v, i) => {\n";
    f << "    const el = document.getElementById('f' + i);\n";
    f << "    if (el) {\n";
    f << "      const type = fetTypes[i];\n";
    f << "      el.querySelectorAll('ellipse, circle').forEach(shape => {\n";
    f << "        if (type === 1) { // N-Type\n";
    f << "          shape.style.fill = v ? '#4caf50' : '#1b3320';\n";
    f << "          shape.style.stroke = v ? '#81c784' : '#2d5a32';\n";
    f << "        } else { // P-Type\n";
    f << "          shape.style.fill = v ? '#ff5252' : '#3d1c1c';\n";
    f << "          shape.style.stroke = v ? '#ff8a80' : '#6b2d2d';\n";
    f << "        }\n";
    f << "        shape.style.strokeWidth = v ? '2px' : '1px';\n";
    f << "      });\n";
    f << "    }\n";
    f << "  });\n";
    f << "  \n";
    f << "  // Update Jumped\n";
    f << "  const jumpedList = document.getElementById('jumped-list');\n";
    f << "  jumpedList.innerHTML = step.flipped.map(i => `<span style='background:rgba(61,90,254,0.2); color:#fff; border:1px solid var(--accent); padding:2px 8px; border-radius:4px; font-size:11px; font-family:\"JetBrains Mono\"'>${labels[i] || 'w'+i}</span>`).join('');\n";
    f << "  \n";
    f << "  // Update Net Table\n";
    f << "  const table = document.getElementById('net-table');\n";
    f << "  let html = '<tr><th>Net</th><th>Val</th></tr>';\n";
    f << "  step.wires.forEach((v, i) => {\n";
    f << "    if (labels[i] || i < 15) {\n";
    f << "      const isFlipped = step.flipped.indexOf(i) !== -1;\n";
    f << "      const valClass = v ? 'high' : 'low';\n";
    f << "      html += `<tr class=\"${isFlipped ? 'flipped' : ''}\"><td class=\"net-name\">${labels[i] || 'w'+i}</td><td><span class=\"val ${valClass}\">${v}</span></td></tr>`;\n";
    f << "    }\n";
    f << "  });\n";
    f << "  table.innerHTML = html;\n";
    f << "}\n";
    f << "function go(dir) {\n";
    f << "  currentStep = Math.max(0, Math.min(trace.length - 1, currentStep + dir));\n";
    f << "  update();\n";
    f << "}\n";
    f << "window.addEventListener('keydown', e => {\n";
    f << "  if (e.key === 'ArrowLeft') go(-1);\n";
    f << "  if (e.key === 'ArrowRight') go(1);\n";
    f << "});\n";
    f << "update();\n";


    f << "</script></body></html>\n";
}
#endif


#ifndef WASM

int main_vga() {
    wasm_arena_init(1);
    //const char * path = "gds/ihp-25a/tt_um_znah_vga_ca.gds", *pdk = "ihp-sg13g2";
    //const char * path = "gds/sky-25b/tt_um_pongsagon_tinygpu_v2.oas", *pdk = "sky130A";
    //const char * path = "gds/09/tt_um_rejunity_atari2600.gds", *pdk = "sky130A";
    const char * path = "gds/09/tt_um_znah_vga_ca.gds", *pdk = "sky130A";
    //const char * path = "gds/gf-0p2/tt_um_2048_vga_game.oas", *pdk = "gf180mcuD";
    //const char * path = "gds/09/tt_um_a1k0n_nyancat.gds", *pdk = "sky130A";
    printf("Loading: %s\n", path);

    CircuitExtractor proc;
    proc.pdk = pdk;

    auto t0 = std::chrono::high_resolution_clock::now();
    if (!proc.load(path)) return 1;
    auto t1 = std::chrono::high_resolution_clock::now();
    
    if (!proc.process()) return 1;
    auto t2 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> d_load = t1 - t0;
    std::chrono::duration<double, std::milli> d_proc = t2 - t1;
    
    printf("GDSTK Load : %.2f ms\n", d_load.count());
    printf("Processing : %.2f ms\n", d_proc.count());
    printf("Total Time : %.2f ms\n", (d_load + d_proc).count());
    printf("Arena usage: %.2f MB\n", (float)wasm_arena_get_usage() / (1024*1024));

    VGASimulator sim(proc.circuit, proc.labeledWires);
    if (sim.isValid()) {
        sim.run(380000);
        //sim.run(10);
    } else {
        printf("No VGA pins detected, skipping simulation.\n");
    }

    return 0;
}

int main_dff() {
    wasm_arena_init(1);
    const char * path = "gds/09/tt_um_znah_vga_ca.gds", *pdk = "sky130A";
    printf("Loading: %s\n", path);

    CircuitExtractor proc;
    proc.pdk = pdk;
    proc.topOverride = "sky130_fd_sc_hd__dfxtp_1";

    auto t0 = std::chrono::high_resolution_clock::now();
    if (!proc.load(path)) return 1;
    auto t1 = std::chrono::high_resolution_clock::now();
    
    if (!proc.process()) return 1;
    auto t2 = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double, std::milli> d_load = t1 - t0;
    std::chrono::duration<double, std::milli> d_proc = t2 - t1;
    
    printf("GDSTK Load : %.2f ms\n", d_load.count());
    printf("Processing : %.2f ms\n", d_proc.count());
    printf("Total Time : %.2f ms\n", (d_load + d_proc).count());
    printf("Arena usage: %.2f MB\n", (float)wasm_arena_get_usage() / (1024*1024));

    // implement test harness for simulated DFF cell
    auto find_net = [&](std::string_view name) -> int {
        for (auto const& lw : proc.labeledWires) {
            if (lw.second == name) return lw.first;
        }
        return -1;
    };

    int n_clk = find_net("CLK");
    int n_d = find_net("D");
    int n_q = find_net("Q");

    printf("Nets: CLK=%d, D=%d, Q=%d\n", n_clk, n_d, n_q);
    if (n_clk == -1 || n_d == -1 || n_q == -1) {
        printf("Error: required nets not found\n");
        return 1;
    }

    std::vector<SimStep> trace;
    auto record = [&](std::string desc) {
        SimStep s;
        s.wires = proc.circuit.wire_data;
        s.fets = proc.circuit.get_fet_states();
        s.description = desc;
        if (!trace.empty()) {
            for (int i = 0; i < (int)s.wires.size(); ++i) {
                if ((s.wires[i] & Circuit::V_MASK) != (trace.back().wires[i] & Circuit::V_MASK)) {
                    s.flipped.push_back(i);
                }
            }
        }
        trace.push_back(s);
    };

    auto settle = [&](std::string desc_prefix) {
        int waves = 0;
        record(desc_prefix + " (initial)");
        while (int steps = proc.circuit.run_wave()) {
            waves++;
            record(desc_prefix + " (wave " + std::to_string(waves) + ")");
            if (waves > 1000) {
                printf("Warning: possible oscillation detected (%d waves)\n", waves);
                break;
            }
        }
        if (proc.circuit.short_count) {
            printf("  shorts=%d\n", proc.circuit.short_count);
        }
    };

    auto set_val = [&](int net, int val, std::string name) {
        proc.circuit.set_input(net, val);
        settle("Set " + name + "=" + std::to_string(val));
    };

    auto get_val = [&](int net) {
        return proc.circuit.wire_data[net] & Circuit::V_MASK;
    };

    printf("Initial settle...\n");
    settle("Initial settle");

    printf("Testing DFF propagation:\n");
    
    // Set D=1, CLK=0
    printf("Setting D=1, CLK=0\n");
    set_val(n_d, 1, "D");
    set_val(n_clk, 0, "CLK");
    printf("Q = %d (expected 0/prev)\n", get_val(n_q));

    // Rising edge: CLK=1
    printf("Rising edge: CLK=1\n");
    set_val(n_clk, 1, "CLK");
    printf("Q = %d (expected 1)\n", get_val(n_q));

    // Change D=0, CLK=1 (should not change Q)
    printf("Setting D=0, CLK=1\n");
    set_val(n_d, 0, "D");
    printf("Q = %d (expected 1)\n", get_val(n_q));

    // Falling edge: CLK=0
    printf("Falling edge: CLK=0\n");
    set_val(n_clk, 0, "CLK");
    printf("Q = %d (expected 1)\n", get_val(n_q));

    // Rising edge: CLK=1 (should capture D=0)
    printf("Rising edge: CLK=1\n");
    set_val(n_clk, 1, "CLK");
    printf("Q = %d (expected 0)\n", get_val(n_q));

    // Generate Report
    printf("Generating report...\n");
    save_dot(proc.circuit, "circuit.dot", proc.labeledWires);
    
    int ret = system("dot -Tsvg circuit.dot -o circuit.svg");
    if (ret != 0) {
        printf("Error: dot failed\n");
    } else {
        std::ifstream f("circuit.svg");
        std::stringstream ss;
        ss << f.rdbuf();
        save_html("report.html", ss.str(), trace, proc.labeledWires, proc.circuit.fets);
        printf("Report saved to report.html\n");
    }


    return 0;
}

int main() {
#ifdef RUN_DFF
    return main_dff();
#else
    return main_vga();
#endif
}

#endif

