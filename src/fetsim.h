#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <tuple>

struct FET {
    int32_t gate;
    int32_t term[2];
    enum {P=0, N=1};
    uint8_t type; // 1 for N-type (on high), 0 for P-type (on low)

    auto tie() const { return std::tie(type, gate, term[0], term[1]); }
    bool operator<(const FET& other) const { return tie() < other.tie(); }  
};

struct Queue {
    std::vector<uint32_t> data;
    size_t head = 0, tail = 0, mask = 0;
    void push_back(uint32_t w) { data[tail] = w; tail = (tail + 1) & mask; }
    uint32_t front() { return data[head]; }
    uint32_t pop_front() { 
        uint32_t r = data[head];
        head = (head + 1) & mask; 
        return r;
    }
    bool empty() const { return head == tail; }
    size_t size() const { return (tail - head) & mask; }
    void clear() { head = tail = 0; }
    void resize(size_t n) {
        size_t s = 1;
        while (s <= n) s <<= 1;
        data.resize(s);
        mask = s - 1;
        clear();
    }
};

// Compressed Sparse Row
template<typename T>
struct CSRMap {
    std::vector<T> data;
    std::vector<uint32_t> head;

    void init(size_t n_rows) {
        head.assign(n_rows + 1, 0);
        data.clear();
    }

    void tally(uint32_t row) {
        head[row]++;
    }

    void compile() {
        for (size_t i = 0; i < head.size() - 1; ++i) head[i+1] += head[i];
        data.resize(head.back());
    }

    void push(uint32_t row, const T& item) {
        data[--head[row]] = item;
    }

    struct Range {
        const T *b, *e;
        const T* begin() const { return b; }
        const T* end() const { return e; }
        size_t size() const { return e - b; }
    };

    Range operator[](uint32_t row) const { 
        return { data.data() + head[row], data.data() + head[row+1] }; 
    }
};

struct Circuit {
    uint32_t fixed_wire_n;
    
    struct PackedGate { 
        uint32_t t0, t1;   // The two source/drain terminals connected by this FET
        uint32_t fi_type;  // Combined FET index (lower 31 bits) and Type (top bit: 1=N, 0=P)
    };
    struct PackedPeer { 
        uint32_t other;    // The wire index of the "other" terminal (destination of traversal)
        uint32_t fi;       // FET index (to check fet_on state)
    };
    
    // wire to FET gates
    CSRMap<PackedGate> wire_gates;

    // wire to peer wires (connected by FET channels)
    CSRMap<PackedPeer> wire_terms;
    
    // State of each FET (true if conducting). Indexed by fet_index.
    std::vector<uint8_t> fet_on;
    
    // Wire data bits: 0=value, 1=S_DIRTY, 2=S_VISITED
    enum { V_MASK = 1, S_DIRTY = 2, S_VISITED = 4 };
    // State of each wire (0/1 value + flags). Indexed by wire_index.
    std::vector<uint8_t> wire_data;
    
    // Simulation stats
    int short_count = 0;

    // Temporary buffers for BFS/traversal to avoid reallocations.
    std::vector<uint32_t> visited_buf, stack_buf;
    uint32_t *visited_ptr = nullptr, *stack_ptr = nullptr;
    int visited_n = 0, stack_n = 0;

    // Queue of wires that changed state and need processing.
    Queue dirty_wires;


    void set_input(uint32_t wire, uint8_t val) {
        if ((wire_data[wire] & V_MASK) == val) return;
        wire_data[wire] = (wire_data[wire] & ~V_MASK) | val;
        update_fets(wire, val);
    }

    void update_fets(uint32_t gate_wire, uint8_t gate_val) {
        uint8_t* wire_states = wire_data.data();
        uint8_t* fet_states = fet_on.data();

        for (const auto& gate_entry : wire_gates[gate_wire]) {
            uint32_t fet_idx = gate_entry.fi_type & 0x7FFFFFFF;
            uint32_t type = gate_entry.fi_type >> 31;
            bool on = (type == gate_val);
            if (on == fet_states[fet_idx]) continue;
            fet_states[fet_idx] = on;
            
            if (!(wire_states[gate_entry.t0] & S_DIRTY)) {
                dirty_wires.push_back(gate_entry.t0);
                wire_states[gate_entry.t0] |= S_DIRTY;
            }
            if (!(wire_states[gate_entry.t1] & S_DIRTY)) {
                dirty_wires.push_back(gate_entry.t1);
                wire_states[gate_entry.t1] |= S_DIRTY;
            }
        }
    }

    int run_wave() {
        if (dirty_wires.empty()) return 0;
        const uint32_t stop = -1;
        dirty_wires.push_back(stop);
        int step_count = 0;
        while (dirty_wires.front() != stop) {
            step();
            ++step_count;
        }
        dirty_wires.pop_front(); // remove stop
        return step_count;
    }

    bool step() {
        uint32_t start_wire = -1;
        uint8_t* wire_states = wire_data.data();
        while (!dirty_wires.empty()) {
            uint32_t wire = dirty_wires.front();
            if (wire == (uint32_t)-1) break;
            dirty_wires.pop_front();
            if ((wire_states[wire] & S_DIRTY)) {
                start_wire = wire;
                break;
            }
        }
        if (start_wire == (uint32_t)-1) return false;

        visited_n = 0;
        wire_states[start_wire] |= S_VISITED;
        uint8_t driven_mask = 0; 
        stack_ptr[0] = start_wire;
        stack_n = 1;

        uint8_t* fet_states = fet_on.data();


        while (stack_n > 0) {
            uint32_t wire = stack_ptr[--stack_n];
            visited_ptr[visited_n++] = wire;
            
            for (const auto& peer : wire_terms[wire]) {
                if (!fet_states[peer.fi]) continue;
                uint32_t other = peer.other;
                uint8_t other_state = wire_states[other];
                if (other < 2) {
                    driven_mask |= (1 << (other_state & V_MASK));
                } else if (!(other_state & S_VISITED)) {
                    stack_ptr[stack_n++] = other;
                    wire_states[other] |= S_VISITED;
                }
            }
        }
        
        if (driven_mask == 3) short_count++;
        bool is_driven = driven_mask != 0;
        uint8_t new_val = (driven_mask == 2) ? 1 : 0; 

        for (int i = 0; i < visited_n; ++i) {
            uint32_t wire = visited_ptr[i];
            uint8_t state = wire_states[wire];
            wire_states[wire] = state & V_MASK; 
            if (is_driven && (state & V_MASK) != new_val) {
                wire_states[wire] = new_val;
                update_fets(wire, new_val);
            }
        }
        return !dirty_wires.empty();
    }
};

struct CircuitMetadata {
    std::vector<std::string> wire_names;
    std::vector<std::string> fet_scopes;
};

struct CircuitBuilder {
    std::vector<FET> fets;
    std::vector<std::string> fet_scopes;
    int wire_n = 2; // VGND, VPWR
    std::vector<std::string> wire_names = {"GND", "PWR"};
    std::string current_prefix;

    struct Scope {
        CircuitBuilder& b;
        std::string old;
        Scope(CircuitBuilder& b, const char* name) : b(b), old(b.current_prefix) {
            if (name) b.current_prefix += name + std::string("/");
        }
        ~Scope() { b.current_prefix = old; }
    };

    int add_wire(const char* name = nullptr) {
        int id = wire_n++;
        if (name) wire_names.push_back(current_prefix + name);
        else wire_names.push_back(current_prefix + "w" + std::to_string(id));
        return id;
    }

    void add_fet(int g, int t0, int t1, uint8_t type) {
        fets.push_back({g, {t0, t1}, type});
        fet_scopes.push_back(current_prefix);
    }

    Circuit build(CircuitMetadata* meta = nullptr) {
        if (meta) {
            meta->wire_names = wire_names;
            meta->fet_scopes = fet_scopes;
        }
        Circuit c;
        c.fixed_wire_n = 2;
        c.wire_data.assign(wire_n, 0);
        c.wire_data[1] = Circuit::V_MASK; // VPWR
        c.dirty_wires.resize(wire_n);
        c.fet_on.assign(fets.size(), 0);
        c.visited_buf.resize(wire_n);
        c.stack_buf.resize(wire_n);
        c.visited_ptr = c.visited_buf.data();
        c.stack_ptr = c.stack_buf.data();
        
        for (int i = 0; i < (int)c.fixed_wire_n; ++i) c.wire_data[i] |= Circuit::S_DIRTY;

        c.wire_gates.init(wire_n);
        c.wire_terms.init(wire_n);

        for (const auto& f : fets) {
            c.wire_gates.tally(f.gate);
            c.wire_terms.tally(f.term[0]);
            c.wire_terms.tally(f.term[1]);
        }
        
        c.wire_gates.compile();
        c.wire_terms.compile();

        for (int i = (int)fets.size() - 1; i >= 0; --i) {
            const auto& f = fets[i];
            uint32_t fet_idx = i;
            uint32_t g = f.gate;
            uint32_t t0 = f.term[0];
            uint32_t t1 = f.term[1];
            
            uint32_t fi_type = fet_idx | ((uint32_t)f.type << 31);
            c.wire_gates.push(g, {t0, t1, fi_type});
            c.wire_terms.push(t1, {t0, fet_idx});
            c.wire_terms.push(t0, {t1, fet_idx});
        }
        for (int i = 0; i < wire_n; ++i) {
            c.update_fets(i, c.wire_data[i] & Circuit::V_MASK);
        }
        return c;
    }
};
