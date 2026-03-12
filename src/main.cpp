#include <array>
#include <cstdio>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <sys/stat.h>

#include <gdstk/gdstk.hpp>

#include "cells.h"
#include "extractor.h"
#include "fetsim.h"
#include "vga.h"

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

struct Module {
    CircuitExtractor extractor;
    Circuit & circuit;
    VGASimulator vga;

    Module() : circuit(extractor.circuit), vga(circuit) {}

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

#define WASM_INDEXED_ARRAY_(NAME, COLLECTION) \
    WASM_EXPORT("wasm_" #NAME "_ptr") \
    extern "C" void* wasm_##NAME##_ptr(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return nullptr; \
        return (void*)g_mod()->COLLECTION[idx].data(); \
    } \
    WASM_EXPORT("wasm_" #NAME "_size") \
    extern "C" uint32_t wasm_##NAME##_size(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return 0; \
        auto & arr = g_mod()->COLLECTION[idx]; \
        return (uint32_t)(arr.size() * sizeof(arr[0])); \
    }

#define WASM_INDEXED_ARRAY_ATTR_(NAME, COLLECTION, ATTR) \
    WASM_EXPORT("wasm_" #NAME "_ptr") \
    extern "C" void* wasm_##NAME##_ptr(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return nullptr; \
        return (void*)g_mod()->COLLECTION[idx].ATTR.data(); \
    } \
    WASM_EXPORT("wasm_" #NAME "_size") \
    extern "C" uint32_t wasm_##NAME##_size(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return 0; \
        auto & arr = g_mod()->COLLECTION[idx].ATTR; \
        return (uint32_t)(arr.size() * sizeof(arr[0])); \
    }

#define WASM_INDEXED_STR_(NAME, COLLECTION, ATTR) \
    WASM_EXPORT("wasm_" #NAME) \
    extern "C" const char* wasm_##NAME(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return nullptr; \
        return g_mod()->COLLECTION[idx].ATTR.c_str(); \
    }

#define WASM_INDEXED_INT_(NAME, COLLECTION, ATTR) \
    WASM_EXPORT("wasm_" #NAME) \
    extern "C" const int wasm_##NAME(int idx) { \
        if (idx < 0 || idx >= (int)g_mod()->COLLECTION.size()) return 0; \
        return g_mod()->COLLECTION[idx].ATTR; \
    }

#define WASM_INT_(NAME, ATTR) \
    WASM_EXPORT("wasm_" #NAME) \
    extern "C" int wasm_##NAME() { \
        return g_mod()->ATTR; \
    }

extern "C" {
    WASM_EXPORT("wasm_init")
    void wasm_init() {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    WASM_EXPORT("wasm_load_file")
    bool wasm_load_file(const char* path, const char* pdk) {
        wasm_init();
        g_mod()->extractor.pdk = pdk;
        bool res = g_mod()->extractor.load(path);
        fflush(stdout);
        return res;
    }

    WASM_EXPORT("wasm_process")
    bool wasm_process() {
        bool res = g_mod()->extractor.process();
        g_mod()->vga.init(g_mod()->extractor.labeledWires);
        g_mod()->vga.reset();
        fflush(stdout);
        return res;
    }

    WASM_ARRAY_(flatRects, extractor.flatRects);
    WASM_ARRAY_(flatLayerOffsets, extractor.flatLayerOffsets);
    WASM_INDEXED_ARRAY_(flatBVHs, extractor.flatBVHs);
    WASM_ARRAY_(wireData, circuit.wire_data);
    WASM_ARRAY_(fets, extractor.builder.fets);
    WASM_INT_(labeledCount, extractor.labeledWires.size());
    WASM_INT_(shortCount, circuit.short_count);

    WASM_EXPORT("wasm_circuit_get_labeled_id")
    int wasm_circuit_get_labeled_id(uint32_t idx) {
        const auto & lw = g_mod()->extractor.labeledWires;
        if (idx < lw.size()) {
            return lw[idx].id;
        }
        return -1;
    }

    WASM_EXPORT("wasm_circuit_get_labeled_name")
    const char* wasm_circuit_get_labeled_name(uint32_t idx) {
        const auto & lw = g_mod()->extractor.labeledWires;
        if (idx < lw.size()) {
            return lw[idx].name.c_str();
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

    WASM_INT_(circuit_is_settled, circuit.dirty_wires.empty());

    WASM_EXPORT("wasm_circuit_get_layer_name")
    const char* wasm_circuit_get_layer_name(uint32_t idx) {
        return getLayerName((LayerID)idx);
    }

    WASM_EXPORT("wasm_vga_tick")
    void wasm_vga_tick(int n) {
        for (int i=0; i<n; ++i) g_mod()->vga.vga_tick();
    }

    WASM_ARRAY_(vga_buffer, vga.vga_buffer);
    WASM_INT_(vga_stride, vga.max_width);
    WASM_INT_(vga_width, vga.width);
    WASM_INT_(vga_height, vga.height);
    WASM_INT_(vga_ray_x, vga.ray_x);
    WASM_INT_(vga_ray_y, vga.ray_y);

    WASM_INT_(cell_count, extractor.cells.size());
    WASM_INDEXED_STR_(cell_name, extractor.cells, name);
    WASM_INDEXED_ARRAY_ATTR_(cell_rects, extractor.cells, rects);
    WASM_INDEXED_ARRAY_ATTR_(cell_rect2wire, extractor.cells, rect2wire);

    WASM_EXPORT("wasm_cell_bvh_ptr")
    void* wasm_cell_bvh_ptr(int cell_idx, int layer_idx) {
        if (cell_idx < 0 || cell_idx >= (int)g_mod()->extractor.cells.size()) return nullptr;
        if (layer_idx < 0 || layer_idx >= L_COUNT) return nullptr;
        return g_mod()->extractor.cells[cell_idx].layers[layer_idx].bvh.data();
    }

    WASM_EXPORT("wasm_cell_bvh_size")
    uint32_t wasm_cell_bvh_size(int cell_idx, int layer_idx) {
        if (cell_idx < 0 || cell_idx >= (int)g_mod()->extractor.cells.size()) return 0;
        if (layer_idx < 0 || layer_idx >= L_COUNT) return 0;
        return (uint32_t)g_mod()->extractor.cells[cell_idx].layers[layer_idx].bvh.size() * sizeof(BVHNode);
    }

    WASM_INDEXED_ARRAY_ATTR_(layer_bvh, extractor.instLayers, bvh);
    WASM_INDEXED_ARRAY_ATTR_(layer_instances, extractor.instLayers, instances);

    WASM_EXPORT("wasm_strlen")
    uint32_t wasm_strlen(const char* s) {
        return s ? (uint32_t)strlen(s) : 0;
    }

    // WASM_EXPORT("wasm_query_layer")
    // int wasm_query_wire(LayerID layer, int x0, int y0, int x1, int y1) {


    // }

}

#ifdef WASM
int main() { return 0; }
#endif


#ifndef WASM

int main() {

    wasm_arena_init(1);
    //const char * path = "gds/ihp-25a/tt_um_znah_vga_ca.gds";
    //const char * path = "gds/sky-25b/tt_um_pongsagon_tinygpu_v2.oas";
    //const char * path = "gds/09/tt_um_rejunity_atari2600.gds";
    const char * path = "gds/09/tt_um_znah_vga_ca.gds";
    //const char * path = "gds/gf-0p2/tt_um_2048_vga_game.oas";
    //const char * path = "gds/09/tt_um_a1k0n_nyancat.gds";
    //const char * path = "gds/08/tt_um_a1k0n_vgadonut.gds";
    //const char * path = "gds/09/tt_um_oscillating_bones.gds";
    //const char * path = "gds/multiplier8.oas";
    //const char * path = "gds/ihp_ca.gds";
    printf("Loading: %s\n", path);

    Module* mod = g_mod();
    CircuitExtractor& proc = mod->extractor;

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

    g_mod()->vga.init(proc.labeledWires);
    if (g_mod()->vga.isValid()) {
        g_mod()->vga.run(380000);
        //g_mod()->vga.run(100);
    } else {
        printf("No VGA pins detected, skipping simulation.\n");
    }
    return 0;
}


#endif

