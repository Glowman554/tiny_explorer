#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <map>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <sys/stat.h>

#include <gdstk/gdstk.hpp>
#include <fstream>
#include <set>

#include "extractor.h"
#include "fetsim.h"

#ifdef WASM
#define WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define WASM_EXPORT(name)
#endif

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
}

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

    void reset() {
        if (!isValid()) return;
        if (ena != -1) circuit.set_signal(ena, 1);
        circuit.set_signal(rst_n, 0);
        // TT-specific reset behavior
        if (in_pins[1] != -1) circuit.set_signal(in_pins[1], 1);
        if (in_pins[4] != -1) circuit.set_signal(in_pins[4], 1);
        for (int i = 0; i < 10; ++i) tick();
        if (in_pins[1] != -1) circuit.set_signal(in_pins[1], 0);
        if (in_pins[4] != -1) circuit.set_signal(in_pins[4], 0);
        circuit.set_signal(rst_n, 1);
    }

    bool isValid() const { return clk != -1 && rst_n != -1; }

    void run(int max_ticks = 1000000) {
        if (!isValid()) return;
        
        printf("VGA Sim: clk=%d, rst_n=%d, outputs=[", clk, rst_n);
        for(int i=0; i<8; ++i) printf("%d%s", out_pins[i], i==7 ? "]\n" : ",");

        printf("Resetting...\n");
        reset();
        
        printf("Running simulation...\n");
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

    void settle(int max_waves=200) {
        circuit.settle(max_waves);
    }

    void tick() {
        circuit.set_signal(clk, 0); settle();
        circuit.set_signal(clk, 1); settle();

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

struct Module {
    CircuitExtractor extractor;
    Circuit & circuit;
    VGASimulator* vga = nullptr;

    Module() : circuit(extractor.circuit) {}

};

Module* _g_mod = nullptr;
Module* g_mod() {
    if (!_g_mod) {
        _g_mod = new Module();
    }
    return _g_mod;
}

#define WASM_ARRAY_(NAME, ATTR) \
    WASM_EXPORT("wasm_" #NAME "_ptr")     \
    extern "C" void* wasm_##NAME##_ptr() {         \
        return g_mod()->ATTR.data();  \
    }                                       \
    WASM_EXPORT("wasm_" #NAME "_size")    \
    extern "C" uint32_t wasm_##NAME##_size() {     \
        auto & arr = g_mod()->ATTR;   \
        return arr.size() * sizeof(arr[0]); \
    }

#define WASM_ARRAY(NAME) WASM_ARRAY_(NAME, NAME)

#define WASM_INT_(NAME, ATTR) \
    WASM_EXPORT("wasm_" #NAME) \
    extern "C" int wasm_##NAME() { \
        return g_mod()->ATTR; \
    }

extern "C" {
    WASM_EXPORT("wasm_load_file")
    bool wasm_load_file(const char* path, const char* pdk) {
        g_mod()->extractor.pdk = pdk;
        return g_mod()->extractor.load(path);
    }

    WASM_EXPORT("wasm_process")
    bool wasm_process() {
        return g_mod()->extractor.process();
    }

    WASM_ARRAY_(flatRects, extractor.flatRects);
    WASM_ARRAY_(flatLayerOffsets, extractor.flatLayerOffsets);
    WASM_ARRAY_(wireData, circuit.wire_data);
    WASM_INT_(labeledCount, extractor.labeledWires.size());
    WASM_INT_(shortCount, circuit.short_count);

    WASM_EXPORT("wasm_circuit_get_labeled_id")
    int wasm_circuit_get_labeled_id(uint32_t idx) {
        const auto & lw = g_mod()->extractor.labeledWires;
        if (idx < lw.size()) {
            return lw[idx].first;
        }
        return -1;
    }

    WASM_EXPORT("wasm_circuit_get_labeled_name")
    const char* wasm_circuit_get_labeled_name(uint32_t idx) {
        const auto & lw = g_mod()->extractor.labeledWires;
        if (idx < lw.size()) {
            return lw[idx].second.c_str();
        }
        return nullptr;
    }

    WASM_EXPORT("wasm_circuit_set_input")
    void wasm_circuit_set_input(uint32_t wire, uint32_t val) {
        if (wire < g_mod()->circuit.wire_n()) {
            g_mod()->circuit.set_signal(wire, (uint8_t)val);
        }
    }

    WASM_EXPORT("wasm_circuit_run_wave")
    int wasm_circuit_run_wave() {
        return g_mod()->circuit.run_wave();
    }

    WASM_EXPORT("wasm_vga_init")
    void wasm_vga_init() {
        if (g_mod()->vga) delete g_mod()->vga;
        g_mod()->vga = new VGASimulator(g_mod()->circuit, g_mod()->extractor.labeledWires);
        g_mod()->vga->reset();
    }

    WASM_EXPORT("wasm_vga_tick")
    void wasm_vga_tick(int n) {
        if (g_mod()->vga) {
            for (int i=0; i<n; ++i) g_mod()->vga->tick();
        }
    }

    WASM_ARRAY_(vga_buffer, vga->vga_buffer);
    WASM_INT_(vga_width, vga->width);
    WASM_INT_(vga_height, vga->height);
}

#ifdef WASM
int main() { return 0; }
#endif


#ifndef WASM

int main() {

    wasm_arena_init(1);
    //const char * path = "gds/ihp-25a/tt_um_znah_vga_ca.gds", *pdk = "ihp-sg13g2";
    //const char * path = "gds/sky-25b/tt_um_pongsagon_tinygpu_v2.oas", *pdk = "sky130A";
    const char * path = "gds/09/tt_um_rejunity_atari2600.gds", *pdk = "sky130A";
    //const char * path = "gds/09/tt_um_znah_vga_ca.gds", *pdk = "sky130A";
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
        //sim.run(380000);
        //sim.run(100);
    } else {
        printf("No VGA pins detected, skipping simulation.\n");
    }
    return 0;

    // const InstID inst_id = 4426;
    // printf("\n--- All Wires for Inst %d ---\n", inst_id);
    // std::vector<int> wires;
    // if (4426 < (int)proc.instances.size()) {
    //     const auto& inst = proc.instances[inst_id];
    //     const auto& cell = proc.cells[inst.cell_id];
    //     int offset = proc.instOffsets[inst_id];
    //     std::set<int> printed;
    //     for (int local = 0; local < (int)cell.wireCount; ++local) {
    //         int globalWire = proc.segment2flat[offset + local];
    //         wires.push_back(globalWire);
    //         if (globalWire >= 2 && printed.insert(globalWire).second) {
    //             proc.printWire(globalWire);
    //         }
    //     }
    // }
    // printf("\n--------------------------------\n");
    // // print ws as comma separated list
    // std::sort(wires.begin(), wires.end());
    // wires.erase(std::unique(wires.begin(), wires.end()), wires.end());
    // for (size_t i = 0; i < wires.size(); ++i) {
    //     printf("%d%s", wires[i], (i == wires.size() - 1) ? "" : ", ");
    // }
    // printf("\n");

    // generate graphviz dot file with component, conneted to wire
    {
        int target = 42256;
        std::set<int> cluster_wires;
        std::vector<int> q = {target};
        cluster_wires.insert(target);
        
        // Find full channel neighborhood (S/D connected)
        for (size_t head = 0; head < q.size(); ++head) {
            int w = q[head];
            for (const auto& fet : proc.builder.fets) {
                int peer = -1;
                if (fet.term[0] == (uint32_t)w) peer = fet.term[1];
                else if (fet.term[1] == (uint32_t)w) peer = fet.term[0];
                
                if (peer != -1 && peer >= 2 && cluster_wires.find(peer) == cluster_wires.end()) {
                    cluster_wires.insert(peer);
                    q.push_back(peer);
                }
            }
        }

        std::set<int> viz_wires = cluster_wires;
        std::set<size_t> viz_fets;

        // Include any FET that has a terminal or gate in the cluster
        for (size_t i = 0; i < proc.builder.fets.size(); ++i) {
            const auto& fet = proc.builder.fets[i];
            bool t0_in = cluster_wires.count(fet.term[0]);
            bool t1_in = cluster_wires.count(fet.term[1]);
            bool g_in = cluster_wires.count(fet.gate);

            if (t0_in || t1_in || g_in) {
                viz_fets.insert(i);
                viz_wires.insert(fet.gate);
                viz_wires.insert(fet.term[0]);
                viz_wires.insert(fet.term[1]);
                printf("FET: inst %zu, g %d t %d %d\n", i, 
                    fet.gate, fet.term[0], fet.term[1]);
            }
        }

        std::ofstream f("dump.dot");
        f << "digraph G {\n  rankdir=LR;\n  node [fontname=\"sans-serif\", fontsize=10];\n";
        
        std::map<int, std::string> net2name;
        for (auto const& lw : proc.labeledWires) net2name[lw.first] = lw.second;

        for (int w : viz_wires) {
            std::string label;
            if (w == 0) label = "VSS";
            else if (w == 1) label = "VDD";
            else label = net2name.count(w) ? net2name[w] : "w" + std::to_string(w);
            
            std::string color = "black";
            int penwidth = 1;
            if (w == target) {
                color = "blue";
                penwidth = 3;
            } else if (cluster_wires.count(w)) {
                color = "darkgreen";
                penwidth = 2;
            }

            f << "  w" << w << " [label=\"" << label << "\", color=\"" << color << "\", penwidth=" << penwidth << (w < 2 ? ", shape=plaintext" : "") << "];\n";
        }

        for (size_t i : viz_fets) {
            const auto& fet = proc.builder.fets[i];
            bool is_n = (fet.type == FET::N);
            uint8_t g_val = (fet.gate < proc.circuit.wire_data.size()) ? (proc.circuit.wire_data[fet.gate] & 1) : 0;
            bool is_open = is_n ? (g_val == 1) : (g_val == 0);

            std::string type_str = is_n ? "N" : "P";
            std::string color = is_n ? "green" : "red";
            
            f << "  f" << i << " [shape=box, label=\"" << type_str << "(" << fet.instance << ")" << "\", color=\"" << color 
              << "\", width=0.2, height=0.2" << (is_open ? ", penwidth=3" : "") << "];\n";
            
            std::string chan_style = is_open ? " [arrowhead=none, penwidth=3]" : " [arrowhead=none]";
            f << "  f" << i << " -> w" << fet.term[0] << chan_style << ";\n";
            f << "  f" << i << " -> w" << fet.term[1] << chan_style << ";\n";
            if (fet.gate >= 0) f << "  w" << fet.gate << " -> f" << i << " [style=dashed];\n";
        }
        f << "}\n";
        printf("Expanded neighborhood of wire %d dumped to dump.dot (%zu wires, %zu FETs)\n", 
               target, viz_wires.size(), viz_fets.size());
    }
    return 0;
}


#endif

