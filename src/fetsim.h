#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include <tuple>
#include <algorithm>

template <typename T>
void unique_sort(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

struct FET {
    uint32_t gate, term[2];
    enum {P=0, N=1};
    uint8_t type; 
    auto tie() const { return std::tie(gate, term[0], term[1], type); }
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
    size_t size() const {
        return (tail - head) & (data.size() - 1);
    }

    bool empty() const { return head == tail; }
    uint32_t front() const { return data[head]; }
};

template<typename T>
struct CSRMap {
    std::vector<T> data;
    std::vector<uint32_t> head;
    void build(std::vector<std::pair<uint32_t, T>> & pairs, uint32_t row_n) {
        unique_sort(pairs);
        head.assign(row_n + 1, 0);
        data.clear();
        data.reserve(pairs.size());
        for (auto const& p : pairs) {
            data.push_back(p.second);
            if (p.first < row_n) head[p.first + 1]++;
        }
        for (size_t i = 0; i < row_n; ++i) head[i+1] += head[i];
    }
    struct Range {
        const T *b, *e;
        const T* begin() const { return b; }
        const T* end() const { return e; }
        size_t size() const { return e - b; }
        bool empty() const { return b == e; }
    };
    Range operator[](uint32_t row) const { return { data.data() + head[row], data.data() + head[row+1] }; }
};

struct PackedPeer {
    uint32_t other, gate_type;
    uint32_t gate() const {return gate_type & 0x7FFFFFFF;}
    uint32_t fet_type() const {return gate_type >> 31;}
    auto tie() const { return std::tie(other, gate_type); }
    bool operator<(const PackedPeer& o) const { return tie() < o.tie(); }
    bool operator==(const PackedPeer& o) const { return tie() == o.tie(); }
};

struct Circuit {
    CSRMap<uint32_t> gate_to_nets; // Nets (terminals) affected by a change in this gate wire
    CSRMap<PackedPeer> net_connectivity; // Switched adjacencies between nets
    std::vector<FET> fets;
    std::vector<uint8_t> wire_data;
    
    enum State : uint8_t { V_MASK = 1, S_DIRTY = 2, S_VISITED = 4 };
    
    std::vector<uint32_t> visited_buf, stack_buf;
    Queue dirty_wires;
    int short_count = 0;

    size_t fet_n() const { return fets.size(); }
    size_t wire_n() const { return wire_data.size(); }
    bool is_settled() const { return dirty_wires.empty(); }

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
        int steps = dirty_wires.size();
        for (int i=0; i<steps; ++i) step();
        return steps;
    }

    void step() {
        uint8_t* s = wire_data.data();
        uint32_t seed = -1;
        while (!dirty_wires.empty()) {
            uint32_t w = dirty_wires.pop_front();
            if (s[w] & S_DIRTY) { seed = w; break; }
        }
        if (seed == -1) return;

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
                if ((s[p.gate()] & V_MASK) == p.fet_type()) {
                    if (p.other < 2) driven |= (1 << p.other); // power rail
                    else if (!(s[p.other] & S_VISITED)) {
                        s[p.other] |= S_VISITED;
                        stack_ptr[sn++] = p.other;
                    }
                }
            }
        }

        if (driven == 3) short_count++;
        bool has_driven = (driven != 0); // && (driven != 3);
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
        if (t0>t1) {
            std::swap(t0, t1);
        }
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

        std::sort(fets.begin(), fets.end());
        std::vector<std::pair<uint32_t, uint32_t>> gate_to_nets;
        std::vector<std::pair<uint32_t, PackedPeer>> net_connectivity;
        int inverter_count = 0;
        for (int i = 0; i < fets.size(); ++i) {
            auto const& a = fets[i];
            uint32_t gt = a.gate | ((uint32_t)a.type << 31);
            if (a.term[0] >= 2) {
                gate_to_nets.push_back({a.gate, a.term[0]});
                net_connectivity.push_back({a.term[0], {a.term[1], gt}});
            }
            if (a.term[1] >= 2) {
                gate_to_nets.push_back({a.gate, a.term[1]});
                net_connectivity.push_back({a.term[1], {a.term[0], gt}});
            }
        }
        printf("Detected %d inverters\n", inverter_count);
        c.gate_to_nets.build(gate_to_nets, wire_n);
        c.net_connectivity.build(net_connectivity, wire_n);

        for (int i = 2; i < wire_n; ++i) c.trigger_gate(i);
        return c;
    }
};
