#pragma once

#include <cstddef>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <climits>



struct Rect { 
    int32_t x1, y1, x2, y2;
    static constexpr Rect empty() {
        return {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    }
};

struct BVHNode {
    Rect bbox;
    int32_t first, count, left; // left is index of left child, -1 if leaf
};

template<typename NodesT, typename RectsT>
struct BVHView {
    const NodesT& nodes;
    const RectsT& rects;
    BVHView(const NodesT& n, const RectsT& r) : nodes(n), rects(r) {}
};

struct DSU {
    std::vector<int> p;
    DSU(size_t n = 0) : p(n, -1) {}
    bool is_root(int i) const { return p[i] < 0; }
    int find(int i) {
        if (i >= p.size()) {
            int old_n = p.size();
            p.resize(i+1, -1);
            return i;
        }
        return _find(i);
    }
    int _find(int i) {
        int root = i;
        while (!is_root(root)) root = p[root];
        while (i != root) { int next = p[i]; p[i] = root; i = next; }
        return root;
    }
    void unite(int i, int j) {
        int ri = find(i), rj = find(j);
        if (ri != rj) {
            if (p[ri] > p[rj]) std::swap(ri, rj);
            p[ri] += p[rj];
            p[rj] = ri;
        }
    }
    uint32_t count() const {
        uint32_t c = 0;
        for (int i = 0; i < p.size(); ++i) {
            c += is_root(i);
        }
        return c;
    }
    enum {NeedsID=-1, Skip=-2};
    template<typename ID>
    int assign_ids(std::vector<ID> & ids, ID next_id) {
        ids.resize(p.size(), NeedsID);
        for (int i = 0; i < (int)p.size(); ++i) {
            if (ids[i] != NeedsID) continue;
            int root = _find(i);
            if (ids[root] == NeedsID) {
                ids[root] = next_id++;
            }
            ids[i] = ids[root];
        }
        return next_id;
    }
};

inline bool overlaps(const Rect& a, const Rect& b) {
    return a.x1 < b.x2 && a.x2 > b.x1 && a.y1 < b.y2 && a.y2 > b.y1;
}

inline bool contains(const Rect& a, const Rect& b) {
    return a.x1 <= b.x1 && a.y1 <= b.y1 && a.x2 >= b.x2 && a.y2 >= b.y2;
}

inline bool touches(const Rect& a, const Rect& b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

inline int64_t area(const Rect & r) {
    return (int64_t)(r.x2 - r.x1) * (r.y2 - r.y1);
}

inline Rect getUnion(const Rect& a, const Rect& b) {
    return {
        std::min(a.x1, b.x1),
        std::min(a.y1, b.y1),
        std::max(a.x2, b.x2),
        std::max(a.y2, b.y2)
    };
}

inline bool getIntersection(const Rect& a, const Rect& b, Rect& out) {
    out.x1 = std::max(a.x1, b.x1);
    out.y1 = std::max(a.y1, b.y1);
    out.x2 = std::min(a.x2, b.x2);
    out.y2 = std::min(a.y2, b.y2);
    return overlaps(a, b);
}

inline void subtractRect(const Rect& a, const Rect& b, std::vector<Rect>& results) {
    Rect inter;
    if (!getIntersection(a, b, inter)) {
        results.push_back(a);
        return;
    }
    if (a.y1 < inter.y1) results.push_back({a.x1, a.y1, a.x2, inter.y1});
    if (a.y2 > inter.y2) results.push_back({a.x1, inter.y2, a.x2, a.y2});
    if (a.x1 < inter.x1) results.push_back({a.x1, inter.y1, inter.x1, inter.y2});
    if (a.x2 > inter.x2) results.push_back({inter.x2, inter.y1, a.x2, inter.y2});
}

template<class TRect>
void buildBVHRecursive(std::vector<TRect>& rects, int first, int count, std::vector<BVHNode>& nodes, int nodeIdx) {
    Rect bbox = Rect::empty();
    for (int i = 0; i < count; ++i) {
        bbox = getUnion(bbox, rects[first + i]);
    }
    nodes[nodeIdx].bbox = bbox;
    nodes[nodeIdx].first = first;
    nodes[nodeIdx].count = count;

    if (count <= 4) {
        nodes[nodeIdx].left = -1;
    } else {
        int32_t dx = bbox.x2 - bbox.x1;
        int32_t dy = bbox.y2 - bbox.y1;
        int axis = (dx > dy) ? 0 : 1;
        int32_t mid = (axis == 0) ? (bbox.x1 + bbox.x2) / 2 : (bbox.y1 + bbox.y2) / 2;

        auto it = std::partition(rects.begin() + first, rects.begin() + first + count, [&](const Rect& r) {
            int32_t center = (axis == 0) ? (r.x1 + r.x2) / 2 : (r.y1 + r.y2) / 2;
            return center < mid;
        });
        
        int leftCount = std::distance(rects.begin() + first, it);
        if (leftCount == 0 || leftCount == count) leftCount = count / 2;

        int leftIdx = (int)nodes.size();
        nodes.emplace_back(); // left child
        nodes.emplace_back(); // right child
        nodes[nodeIdx].left = leftIdx;

        buildBVHRecursive(rects, first, leftCount, nodes, leftIdx);
        buildBVHRecursive(rects, first + leftCount, count - leftCount, nodes, leftIdx + 1);
    }
}

template<class Rect>
void buildLayerBVH(std::vector<Rect>& rects, uint32_t start, uint32_t count, std::vector<BVHNode>& nodes) {
    if (count == 0) {
        nodes.clear();
        return;
    }
    nodes.clear();
    nodes.emplace_back(); // root
    buildBVHRecursive(rects, (int)start, (int)count, nodes, 0);
}

template<typename Visitor, typename Rect, typename Pred>
bool queryBVHRecursive(const std::vector<BVHNode>& nodes, int nodeIdx,
        const std::vector<Rect>& rects, 
        const Rect& q, Visitor& visitor, Pred pred) {
    const auto& node = nodes[nodeIdx];
    if (!pred(node.bbox, q)) return true;

    if (node.left < 0) { // Leaf
        for (int i = 0; i < node.count; ++i) {
            const auto& r = rects[node.first + i];
            if (pred(r, q)) {
                if (!visitor(node.first + i)) return false;
            }
        }
    } else { // Internal
        if (!queryBVHRecursive(nodes, node.left, rects, q, visitor, pred)) return false;
        if (!queryBVHRecursive(nodes, node.left + 1, rects, q, visitor, pred)) return false;
    }
    return true;
}

template<typename Visitor, typename Rect, typename Pred>
bool queryBVH(const std::vector<BVHNode>& nodes, 
              const std::vector<Rect>& rects,
              const Rect& q, Visitor visitor, Pred pred) {
    if (nodes.empty()) return true;
    return queryBVHRecursive(nodes, 0, rects, q, visitor, pred);
}

template<typename F, typename ViewA, typename ViewB, typename Pred>
void collideTreesRecursive(ViewA a, int idA, ViewB b, int idB, 
                           F visitor, Pred pred) {
    const auto& nodeA = a.nodes[idA];
    const auto& nodeB = b.nodes[idB];
    if (!pred(nodeA.bbox, nodeB.bbox)) return;

    if (nodeA.left < 0 && nodeB.left < 0) {
        for (int i = 0; i < nodeA.count; ++i) {
            for (int k = 0; k < nodeB.count; ++k) {
                int idxA = nodeA.first + i;
                int idxB = nodeB.first + k;
                if (pred(a.rects[idxA], b.rects[idxB])) visitor(idxA, idxB);
            }
        }
    } else if (nodeA.left < 0) {
        collideTreesRecursive(a, idA, b, nodeB.left, visitor, pred);
        collideTreesRecursive(a, idA, b, nodeB.left + 1, visitor, pred);
    } else if (nodeB.left < 0) {
        collideTreesRecursive(a, nodeA.left, b, idB, visitor, pred);
        collideTreesRecursive(a, nodeA.left + 1, b, idB, visitor, pred);
    } else {
        if (area(nodeA.bbox) > area(nodeB.bbox)) {
            collideTreesRecursive(a, nodeA.left, b, idB, visitor, pred);
            collideTreesRecursive(a, nodeA.left + 1, b, idB, visitor, pred);
        } else {
            collideTreesRecursive(a, idA, b, nodeB.left, visitor, pred);
            collideTreesRecursive(a, idA, b, nodeB.left + 1, visitor, pred);
        }
    }
}

template<typename F, typename NodesA, typename RectsA, typename NodesB, typename RectsB, typename Pred = decltype(touches)>
void collideTrees(const NodesA& nodesA, const RectsA& rectsA,
                  const NodesB& nodesB, const RectsB& rectsB,
                  F visitor, Pred pred = touches) {
    if (nodesA.empty() || nodesB.empty()) return;
    collideTreesRecursive(BVHView{nodesA, rectsA}, 0, BVHView{nodesB, rectsB}, 0, visitor, pred);
}

template<typename F, typename Nodes, typename Rects, typename Pred>
void collideSelfRecursive(const Nodes& nodes, int id,
                          const Rects& rects, F visitor, Pred pred) {
    const auto& n = nodes[id];
    if (n.left < 0) {
        for (int i = 0; i < n.count; ++i) {
            for (int k = i + 1; k < n.count; ++k) {
                int idx1 = n.first + i;
                int idx2 = n.first + k;
                if (pred(rects[idx1], rects[idx2])) visitor(idx1, idx2);
            }
        }
    } else {
        collideSelfRecursive(nodes, n.left, rects, visitor, pred);
        collideSelfRecursive(nodes, n.left + 1, rects, visitor, pred);
        BVHView view{nodes, rects};
        collideTreesRecursive(view, n.left, view, n.left + 1, visitor, pred);
    }
}

template<typename F, typename Nodes, typename Rects, typename Pred = decltype(touches)>
void collideSelf(const Nodes& nodes,
                 const Rects& rects, F visitor, Pred pred = touches) {
    if (nodes.empty()) return;
    collideSelfRecursive(nodes, 0, rects, visitor, pred);
}



/*
// Version 1: Simple containment-only optimization
template<typename Rect>
int optimizeRects_Simple(std::vector<Rect> & rects) {
    if (rects.empty()) return 0;
    std::vector<BVHNode> nodes;
    std::vector<uint8_t> discarded(rects.size(), 0);
    buildLayerBVH(rects, 0, (uint32_t)rects.size(), nodes);
    collideSelf(nodes, rects, [&](int i, int j) {
        if (discarded[i] || discarded[j]) return;
        if (contains(rects[j], rects[i])) discarded[i] = true;
        else if (contains(rects[i], rects[j])) discarded[j] = true;
    }, overlaps);
    int totalDiscarded = 0;
    size_t writeIdx = 0;
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!discarded[i]) {
            if (writeIdx != i) rects[writeIdx] = std::move(rects[i]);
            writeIdx++;
        } else {
            totalDiscarded++;
        }
    }
    rects.resize(writeIdx);
    return totalDiscarded;
}
*/

template<typename RectT>
int optimizeRects(std::vector<RectT> & rects) {
    if (rects.size() < 2) return 0;
    size_t initialSize = rects.size();

    // The Pool: static original rectangles
    std::vector<RectT> pool = std::move(rects);
    rects.clear();
    
    std::vector<BVHNode> poolNodes;
    buildLayerBVH(pool, 0, (uint32_t)pool.size(), poolNodes);
    std::vector<uint8_t> poolDiscarded(pool.size(), 0);
    
    // Results from Passes
    std::vector<RectT> front; // newly grown rects
    std::vector<RectT> finalRects;

    auto mergeRects = [&](const RectT& a, const RectT& b) {
        RectT m = a;
        m.x1 = std::min(a.x1, b.x1);
        m.y1 = std::min(a.y1, b.y1);
        m.x2 = std::max(a.x2, b.x2);
        m.y2 = std::max(a.y2, b.y2);
        return m;
    };

    auto canMerge = [&](const RectT& a, const RectT& b) {
        bool sameX = a.x1 == b.x1 && a.x2 == b.x2;
        bool sameY = a.y1 == b.y1 && a.y2 == b.y2;
        return (sameX || sameY) && touches(a, b);
    };

    // Pass 1: Initial pool internal merges
    collideSelf(poolNodes, pool, [&](int i, int j) {
        if (poolDiscarded[i] || poolDiscarded[j]) return;
        
        if (contains(pool[j], pool[i])) { poolDiscarded[i] = 1; return; }
        if (contains(pool[i], pool[j])) { poolDiscarded[j] = 1; return; }
        
        if (canMerge(pool[i], pool[j])) {
            front.push_back(mergeRects(pool[i], pool[j]));
            poolDiscarded[i] = poolDiscarded[j] = 1;
        }
    }, touches);

    // Pass 2+: Iteratively collide "front" with "poolNodes" and "front" with "front"
    while (!front.empty()) {
        std::vector<RectT> nextFront;
        std::vector<uint8_t> frontDiscarded(front.size(), 0);
        std::vector<BVHNode> frontNodes;
        buildLayerBVH(front, 0, (uint32_t)front.size(), frontNodes);

        // A. Collide front with static pool
        collideTrees(frontNodes, front, poolNodes, pool, [&](int fi, int pi) {
            if (frontDiscarded[fi] || poolDiscarded[pi]) return;
            
            if (contains(pool[pi], front[fi])) { frontDiscarded[fi] = 1; return; }
            if (contains(front[fi], pool[pi])) { poolDiscarded[pi] = 1; return; }
            
            if (canMerge(front[fi], pool[pi])) {
                nextFront.push_back(mergeRects(front[fi], pool[pi]));
                frontDiscarded[fi] = 1; poolDiscarded[pi] = 1;
            }
        }, touches);

        // B. Collide front with front
        collideSelf(frontNodes, front, [&](int i, int j) {
            if (frontDiscarded[i] || frontDiscarded[j]) return;
            
            if (contains(front[j], front[i])) { frontDiscarded[i] = 1; return; }
            if (contains(front[i], front[j])) { frontDiscarded[j] = 1; return; }
            
            if (canMerge(front[i], front[j])) {
                nextFront.push_back(mergeRects(front[i], front[j]));
                frontDiscarded[i] = frontDiscarded[j] = 1;
            }
        }, touches);

        // Collect front rects that didn't merge in this pass
        for (size_t i = 0; i < front.size(); ++i) {
            if (!frontDiscarded[i]) finalRects.push_back(std::move(front[i]));
        }
        front = std::move(nextFront);
    }

    // Collect remaining non-discarded pool rects
    for (size_t i = 0; i < pool.size(); ++i) {
        if (!poolDiscarded[i]) finalRects.push_back(std::move(pool[i]));
    }
    
    rects = std::move(finalRects);
    return (int)(initialSize - rects.size());
}
