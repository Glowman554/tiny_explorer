#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <cctype>
#include <chrono>

#include "fetsim.h"

// Utilities
inline void log_wires(const Circuit& c, FILE* f) {
    if (!f) return;
    for (uint8_t d : c.wire_data) fputc((d & Circuit::V_MASK) ? '1' : '0', f);
    fputc('\n', f);
}


Circuit load_circuit(const char* filename, CircuitMetadata& meta, std::map<std::string, int>& name_to_id) {
    FILE* f = fopen(filename, "r");
    if (!f) { perror("fopen"); exit(1); }
    char line[4096];
    if (!fgets(line, sizeof(line), f)) { fclose(f); exit(1); }
    std::vector<std::string> wire_names;
    char* p = line;
    while (*p) {
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        char* start = p;
        while (*p && !isspace(*p)) p++;
        wire_names.push_back(std::string(start, p - start));
    }
    for (int i = 0; i < (int)wire_names.size(); ++i) name_to_id[wire_names[i]] = i;

    CircuitBuilder b;
    int max_wire = (int)wire_names.size() - 1;
    char type_str[16]; int g, t0, t1;
    struct RawFET { char type; int g, t0, t1; };
    std::vector<RawFET> raw_fets;
    while (fscanf(f, "%s %d %d %d", type_str, &g, &t0, &t1) == 4) {
        raw_fets.push_back({type_str[0], g, t0, t1});
        if (g > max_wire) max_wire = g;
        if (t0 > max_wire) max_wire = t0;
        if (t1 > max_wire) max_wire = t1;
    }
    fclose(f);
    while (b.wire_n <= max_wire) b.add_wire();
    for (int i = 0; i < (int)wire_names.size(); ++i) b.wire_names[i] = wire_names[i];
    for (auto const& rf : raw_fets) b.add_fet(rf.g, rf.t0, rf.t1, (rf.type == 'N')?FET::N:FET::P);
    return b.build(&meta);
}

int main_dff(const char* filename) {
    CircuitMetadata meta;
    std::map<std::string, int> name_to_id;
    Circuit c = load_circuit(filename, meta, name_to_id);

    FILE* log_f = fopen("fetsim.log", "w");
    if (log_f) {
        printf("Capturing 20 power-on steps to fetsim.log...\n");
        for (int i = 0; i < 40 && c.dirty_wires.size(); ++i) {
            c.step();
            log_wires(c, log_f);
        }
    }

    int clk = name_to_id["CLK"];
    int d = name_to_id["D"];
    int q = name_to_id["Q"];

    auto step_all = [&](const char* phase) { 
        int steps = 0; 
        log_wires(c, log_f);
        while(!c.dirty_wires.empty()) {
            c.step();
            steps++;
            if (steps >= 1000) {
                printf("  [Oscillation in %s]\n", phase);
                break;
            }
            log_wires(c, log_f);
        }
        return steps;
    };

    printf("Testing DFF (dfxtp_2):\n");
    printf("CLK D | Q\n");
    printf("------+--\n");

    auto test = [&](int clk_v, int d_v, const char* label) {
        c.set_input(clk, clk_v);
        c.set_input(d, d_v);
        int s = step_all(label);
        printf(" %d  %d | %d   (%d steps, %d shorts)\n", clk_v, d_v, (c.wire_data[q] & Circuit::V_MASK), s, c.short_count);
    };

    // Driving sequence
    test(0, 0, "Init");
    test(0, 1, "D=1");
    test(1, 1, "CLK=1");
    test(0, 1, "CLK=0");
    test(0, 0, "D=0");
    test(1, 0, "CLK=1");
    test(1, 1, "D=1_CLK=1");
    test(0, 1, "CLK=0");
    test(1, 1, "CLK=1");

    fclose(log_f);

    return 0;
}

int main_sim(int argc, char** argv) {
    CircuitMetadata meta;
    std::map<std::string, int> name_to_id;
    Circuit c = load_circuit(argv[1], meta, name_to_id);
    printf("Circuit loaded: %zu wires\n", meta.wire_names.size());

    int clk = -1, rst_n = -1, ena = -1;
    if (name_to_id.count("clk")) clk = name_to_id["clk"];
    if (name_to_id.count("rst_n")) rst_n = name_to_id["rst_n"];
    if (name_to_id.count("ena")) ena = name_to_id["ena"];

    if (clk == -1 || rst_n == -1) {
        fprintf(stderr, "Error: 'clk' or 'rst_n' not found in top-level wires.\n");
        return 1;
    }

    if (ena != -1) c.set_input(ena, 1);

    std::vector<int> out_wires;
    for (int i = 0; i < 8; ++i) {
        std::string name = "uo_out[" + std::to_string(i) + "]";
        if (name_to_id.count(name)) out_wires.push_back(name_to_id[name]);
    }

    const int max_settle_steps = 400000;
    auto run_settle = [&]() {
        int steps = 0;
        while (!c.dirty_wires.empty() && steps < max_settle_steps) {
            c.step();
            steps++;
        }
        if (!c.dirty_wires.empty()) {
            printf("Warning: didn't settle in %d steps\n", max_settle_steps);
        }
        return steps;
    };

    const int vga_w = 2000, vga_h = 540;
    std::vector<uint8_t> vga_buffer(vga_w * vga_h * 3, 0);
    int ray_x = 0, ray_y = 0, max_ray_x = 0;
    bool last_hsync = false, last_vsync = false;
    bool vsync_triggered = false;

    auto save_vga = [&]() {
        int out_w = std::min(vga_w, std::max(1, max_ray_x));
        FILE* f = fopen("vga.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", out_w, vga_h);
            for (int y = 0; y < vga_h; ++y) {
                fwrite(&vga_buffer[y * vga_w * 3], 1, out_w * 3, f);
            }
            fclose(f);
        }
    };

    for (int i = 0; i < 8; ++i) {
        std::string name = "ui_in[" + std::to_string(i) + "]";
        if (name_to_id.count(name)) c.set_input(name_to_id[name], i==7);
    }
    printf("Inputs initialized (ui_in=0, ena=1).\n");
    printf("Detected %zu output wires.\n", out_wires.size());

    FILE* log_f = nullptr; //fopen("uo_out.log", "w");
    auto tick = [&](int tick_idx) {
        c.set_input(clk, 0); run_settle();
        c.set_input(clk, 1); run_settle();

        bool r1    = (c.wire_data[out_wires[0]] & Circuit::V_MASK);
        bool g1    = (c.wire_data[out_wires[1]] & Circuit::V_MASK);
        bool b1    = (c.wire_data[out_wires[2]] & Circuit::V_MASK);
        bool vsync = (c.wire_data[out_wires[3]] & Circuit::V_MASK);
        bool r0    = (c.wire_data[out_wires[4]] & Circuit::V_MASK);
        bool g0    = (c.wire_data[out_wires[5]] & Circuit::V_MASK);
        bool b0    = (c.wire_data[out_wires[6]] & Circuit::V_MASK);
        bool hsync = (c.wire_data[out_wires[7]] & Circuit::V_MASK);

        // Sync detection: Reset ray on FALLING edge (end of pulse)
        if (!hsync && last_hsync) {
            if (ray_x > max_ray_x) max_ray_x = ray_x;
            ray_x = 0;
            ray_y++;
        }
        if (!vsync && last_vsync) {
            if (ray_x > max_ray_x) max_ray_x = ray_x;
            ray_x = 0;
            ray_y = 0;
            printf("VSync end detected at tick %d! (max_ray_x=%d)\n", tick_idx, max_ray_x);
            if (tick_idx > 10000) {
                //vsync_triggered = true;
            }

        }

        static long long colored_pixels = 0;
        if (ray_x < vga_w && ray_y < vga_h) {
            uint8_t r = (r1 * 2 + r0) * 85;
            uint8_t g = (g1 * 2 + g0) * 85;
            uint8_t b = (b1 * 2 + b0) * 85;
            if (r || g || b) colored_pixels++;
            int idx = (ray_y * vga_w + ray_x) * 3;
            vga_buffer[idx + 0] = r;
            vga_buffer[idx + 1] = g;
            vga_buffer[idx + 2] = b;
        }
        ray_x++;

        if ((tick_idx+1) % 100000 == 0) {
            printf("  Ray at %d,%d, non-black pixels drawn: %lld\n", ray_x, ray_y, colored_pixels);
            save_vga();
        }

        last_hsync = hsync;
        last_vsync = vsync;

        if ((tick_idx+1) % 20000 == 0) save_vga();
        if (log_f) {
            for (int w : out_wires) fputc((c.wire_data[w] & Circuit::V_MASK) ? '1' : '0', log_f);
            fputc('\n', log_f);
        }
    };

    printf("Circuit power-on settle: %d steps\n", run_settle());

    printf("Resetting (rst_n=0) for 100 ticks...\n");
    c.set_input(rst_n, 0);
    c.set_input(name_to_id["ui_in[4]"], 1);
    c.set_input(name_to_id["ui_in[1]"], 1);
    for (int i = 0; i < 100; ++i) tick(i);
    c.set_input(name_to_id["ui_in[4]"], 0);
    c.set_input(name_to_id["ui_in[1]"], 0);

    printf("Running simulation (rst_n=1) until VSync...\n");
    c.set_input(rst_n, 1);
    auto t_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; !vsync_triggered; ++i) {
        tick(i);
        if ((i+1) % 10000 == 0) {
            auto t_now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(t_now - t_start).count();
            printf("Tick %d... (%.2f ticks/s) ray: %d,%d\n", i + 1, (i + 1) / elapsed, ray_x, ray_y);
        }
        if (i > 2000000) { printf("Timeout reached.\n"); break; }
    }
    save_vga();
    auto t_end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(t_end - t_start).count();
    printf("Simulation completed in %.2f seconds\n", duration);

    if (log_f) fclose(log_f);
    printf("Simulation finished. VGA frame saved to vga.ppm\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <circuit.txt>\n", argv[0]);
        return 1;
    }
    std::string fname = argv[1];
    if (fname.find("dfxtp_2") != std::string::npos) {
        return main_dff(argv[1]);
    }
    return main_sim(argc, argv);
}