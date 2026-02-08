#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>
#include <tuple>

struct FET {
    uint32_t gate, term[2];
    enum {P=0, N=1};
    uint8_t type; 
    auto tie() const { return std::tie(type, gate, term[0], term[1]); }
    bool operator<(const FET& o) const { return tie() < o.tie(); }  
};

struct Queue {
    std::vector<uint32_t> data;
    size_t head = 0, tail = 0;
    void resize(size_t n) {
        size_t s = 1; while (s <= n) s <<= 1;
        data.assign(s, 0); head = tail = 0;
    }
    void push_back(uint32_t w) {
        data[tail] = w;
        size_t m = data.size() - 1;
        tail = (tail + 1) & m;
        if (tail == head) {
            std::vector<uint32_t> next(data.size() * 2);
            for (size_t i = 0; i < data.size(); i++) next[i] = data[(head + i) & m];
            head = 0; tail = data.size();
            data = std::move(next);
        }
    }
    uint32_t pop_front() {
        uint32_t r = data[head];
        head = (head + 1) & (data.size() - 1);
        return r;
    }
    bool empty() const { return head == tail; }
    uint32_t front() const { return data[head]; }
};

template<typename T>
struct CSRMap {
    std::vector<T> data;
    std::vector<uint32_t> head;
    void init(size_t rows) { head.assign(rows + 1, 0); data.clear(); }
    void tally(uint32_t row) { head[row]++; }
    void compile() {
        for (size_t i = 0; i < head.size() - 1; ++i) head[i+1] += head[i];
        data.resize(head.back());
    }
    void push(uint32_t row, const T& item) { data[--head[row]] = item; }
    struct Range {
        const T *b, *e;
        const T* begin() const { return b; }
        const T* end() const { return e; }
        size_t size() const { return e - b; }
        bool empty() const { return b == e; }
    };
    Range operator[](uint32_t row) const { return { data.data() + head[row], data.data() + head[row+1] }; }
};

struct PackedPeer { uint32_t other, gate_type; };

struct Circuit {
    CSRMap<uint32_t> gate_to_nets; // Nets (terminals) affected by a change in this gate wire
    CSRMap<PackedPeer> net_connectivity; // Switched adjacencies between nets
    std::vector<FET> fets;
    std::vector<uint8_t> wire_data;
    
    enum State : uint8_t { V_MASK = 1, S_DIRTY = 2, S_VISITED = 4 };
    static constexpr uint32_t STOP_MARKER = 0xFFFFFFFF;
    
    std::vector<uint32_t> visited_buf, stack_buf;
    Queue dirty_wires;
    int short_count = 0;

    size_t fet_n() const { return fets.size(); }
    size_t wire_n() const { return wire_data.size(); }
    bool is_settled() const { return dirty_wires.empty(); }

    std::vector<uint8_t> get_fet_states() const {
        std::vector<uint8_t> states(fets.size());
        for (size_t i = 0; i < fets.size(); ++i) 
            states[i] = (wire_data[fets[i].gate] & V_MASK) == fets[i].type;
        return states;
    }

    void set_input(uint32_t wire, uint8_t val) {
        if ((wire_data[wire] & V_MASK) == val) return;
        wire_data[wire] = (wire_data[wire] & ~V_MASK) | val;
        trigger_gate(wire);
    }

    void trigger_gate(uint32_t gate_wire) {
        uint8_t* s = wire_data.data();
        for (uint32_t term : gate_to_nets[gate_wire]) {
            if (term >= 2 && !(s[term] & S_DIRTY)) { 
                s[term] |= S_DIRTY; 
                dirty_wires.push_back(term); 
            }
        }
    }

    int run_wave() {
        if (dirty_wires.empty()) return 0;
        dirty_wires.push_back(STOP_MARKER);
        int steps = 0;
        while (dirty_wires.front() != STOP_MARKER) { step(); steps++; }
        dirty_wires.pop_front();
        return steps;
    }

    void step() {
        uint8_t* s = wire_data.data();
        uint32_t seed = STOP_MARKER;
        while (!dirty_wires.empty()) {
            uint32_t w = dirty_wires.pop_front();
            if (w == STOP_MARKER) { dirty_wires.push_back(STOP_MARKER); return; }
            if (s[w] & S_DIRTY) { seed = w; break; }
        }
        if (seed == STOP_MARKER) return;

        uint32_t* visited_ptr = visited_buf.data();
        uint32_t* stack_ptr = stack_buf.data();
        int vn = 0, sn = 1;
        stack_ptr[0] = seed;
        s[seed] |= S_VISITED;
        uint8_t driven = 0;

        while (sn > 0) {
            uint32_t w = stack_ptr[--sn];
            visited_ptr[vn++] = w;
            for (auto const& p : net_connectivity[w]) {
                if ((s[p.gate_type & 0x7FFFFFFF] & V_MASK) == (p.gate_type >> 31)) {
                    if (p.other < 2) driven |= (1 << (s[p.other] & V_MASK));
                    else if (!(s[p.other] & S_VISITED)) {
                        s[p.other] |= S_VISITED;
                        stack_ptr[sn++] = p.other;
                    }
                }
            }
        }

        if (driven == 3) short_count++;
        bool has_driven = (driven != 0);
        uint8_t val = (driven == 2) ? 1 : 0; 

        for (int i = 0; i < vn; i++) {
            uint32_t w = visited_ptr[i];
            uint8_t old = s[w];
            s[w] = old & V_MASK; // Clear Flags
            if (has_driven && (old & V_MASK) != val) {
                s[w] = val;
                trigger_gate(w);
            }
        }
    }
};

struct CircuitBuilder {
    std::vector<FET> fets;
    int wire_n = 2;
    void add_fet(int g, int t0, int t1, uint8_t type) {
        fets.push_back({(uint32_t)g, {(uint32_t)t0, (uint32_t)t1}, type});
        wire_n = std::max({wire_n - 1, g, t0, t1}) + 1;
    }
    Circuit build() {
        Circuit c;
        c.wire_data.assign(wire_n, 0);
        c.wire_data[1] = 1; // VPWR
        c.dirty_wires.resize(wire_n + 1);
        c.fets = fets;
        c.visited_buf.resize(wire_n);
        c.stack_buf.resize(wire_n);
        for (int i = 0; i < 2; ++i) c.wire_data[i] |= Circuit::S_DIRTY;

        c.gate_to_nets.init(wire_n); c.net_connectivity.init(wire_n);
        for (auto const& f : fets) {
            if (f.term[0] >= 2) {
                c.gate_to_nets.tally(f.gate);
                c.net_connectivity.tally(f.term[0]);
            }
            if (f.term[1] >= 2) {
                c.gate_to_nets.tally(f.gate);
                c.net_connectivity.tally(f.term[1]);
            }
        }
        c.gate_to_nets.compile(); c.net_connectivity.compile();
        for (int i = (int)fets.size() - 1; i >= 0; i--) {
            auto const& f = fets[i];
            uint32_t gt = f.gate | ((uint32_t)f.type << 31);
            if (f.term[0] >= 2) {
                c.gate_to_nets.push(f.gate, f.term[0]);
                c.net_connectivity.push(f.term[0], {f.term[1], gt});
            }
            if (f.term[1] >= 2) {
                c.gate_to_nets.push(f.gate, f.term[1]);
                c.net_connectivity.push(f.term[1], {f.term[0], gt});
            }
        }
        for (int i = 2; i < wire_n; ++i) c.trigger_gate(i);
        return c;
    }
};
