#pragma once
#include <vector>
#include <numeric>
#include <cstdint>
#include <cstddef>

struct DSU {
    std::vector<int> p;
    DSU() = default;
    void reset(size_t n) { p.resize(n); std::iota(p.begin(), p.end(), 0); }
    int find(int i) {
        int root = i;
        while (p[root] != root) root = p[root];
        while (p[i] != root) { int next = p[i]; p[i] = root; i = next; }
        return root;
    }
    void unite(int i, int j) {
        int ri = find(i), rj = find(j);
        if (ri != rj) p[ri] = rj;
    }
    uint32_t count() const {
        uint32_t c = 0;
        for (int i = 0; i < (int)p.size(); ++i) if (p[i] == i) c++;
        return c;
    }
};
