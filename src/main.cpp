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
        // for (auto const& lw : labeledWires) {
        //     printf("Net %d (%s): %zu gates, %zu terms\n", lw.first, lw.second.c_str(), 
        //            circuit.gate_to_nets[lw.first].size(), circuit.net_connectivity[lw.first].size());
        // }
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


struct VGASimulator {
    Circuit& circuit;
    std::map<std::string, int> name2id;
    int clk, rst_n, ena;
    int in_pins[8], out_pins[8];
    
    int width = 1000, height = 600;
    int ray_x = 0, ray_y = 0;
    bool last_hsync = false, last_vsync = false;
    std::vector<uint8_t> vga_buffer;

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
            in_pins[i] = getPin("ui_in[" + std::to_string(i) + "]");
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
        circuit.set_input(in_pins[1], 1);
        circuit.set_input(in_pins[4], 1);
        for (int i = 0; i < 100; ++i) tick();
        circuit.set_input(in_pins[1], 0);
        circuit.set_input(in_pins[4], 0);
        
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
};



#ifndef WASM

int main() {

    wasm_arena_init(1);
    //const char * path = "gds/ihp-25a/tt_um_znah_vga_ca.gds", *pdk = "ihp-sg13g2";
    //const char * path = "gds/sky-25b/tt_um_pongsagon_tinygpu_v2.oas", *pdk = "sky130A";
    //const char * path = "gds/09/tt_um_rejunity_atari2600.gds", *pdk = "sky130A";
    const char * path = "gds/09/tt_um_znah_vga_ca.gds", *pdk = "sky130A";
    //const char * path = "gds/gf-0p2/tt_um_2048_vga_game.oas", *pdk = "gf180mcuD";
    //const char * path = "gds/09/tt_um_a1k0n_nyancat.gds", *pdk = "sky130A";
    //const char * path = "gds/08/tt_um_a1k0n_vgadonut.gds", *pdk = "sky130A";
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


#endif

