#pragma once

#include <vector>
#include <set>
#include <map>
#include "geom.h"
#include "fetsim.h"

struct RectWire : Rect { int wire; };
constexpr int RECT_WIRE_FIELDS = 5; // x1, y1, x2, y2, wire

struct Reference {
    enum Type { SREF, AREF } type;
    std::string cellName;
    std::vector<Point> points; // insertion points
    int16_t cols = 1, rows = 1;
    bool reflection = false;
    double mag = 1.0;
    double angle = 0.0;
};

struct Text {
    int16_t layer = 0;
    int16_t texttype = 0;
    Point point;
    std::string content;
    bool reflection = false;
    double mag = 1.0;
    double angle = 0.0;
};

struct QuadLayer {
    uint32_t rectStart = 0;
    uint32_t rectCount = 0;
    std::vector<BVHNode> bvh;
};

enum LayerID {
    L_ERROR = 0,
    L_NWELL,
    L_DIFF,
    L_CHANNEL,  // created from L_DIFF & L_POLY
    L_TERMINAL, // created from L_DIFF - L_POLY
    L_N_TERM, L_P_TERM, // for flattened geom only
    L_POLY,     // FET Gates
    L_LICON,    // connects L_LI1 to both L_POLY and L_TERMINAL
    L_LI1,  L_MCON,
    L_MET1, L_VIA1,
    L_MET2, L_VIA2,
    L_MET3, L_VIA3,
    L_MET4, L_VIA4,
    L_MET5,
    L_COUNT
};

struct ChildConnection {
    int32_t parentIdx;
    int32_t childRefIdx;
    int32_t childIdx;
    auto tie() const { return std::tie(parentIdx, childRefIdx, childIdx); }
    bool operator<(const ChildConnection& o) const { return tie() < o.tie(); }
    bool operator==(const ChildConnection& o) const { return tie() == o.tie(); }
};

struct Cell {
    std::string name;
    std::vector<Rect> rects;
    std::array<QuadLayer, L_COUNT> layers; // Indexed by LayerID
    std::vector<Reference> references;
    std::vector<Text> texts;
    DSU wireDSU;
    std::map<std::string, int32_t> label2rect;
    std::vector<ChildConnection> connectionsR2R; // parent rect to child rect
    std::vector<ChildConnection> connectionsR2W; // parent rect to child wire
    std::vector<FET> fets;

    std::set<int32_t> bridgingRefs;
    std::vector<std::vector<Rect>> tempRects; // Temporary storage during parsing

    bool isProcessed = false;
    bool isTop = true;

    // Stats
    size_t flatRects = 0;
    size_t flatFETs = 0;
    size_t totalInstances = 0;

    // Special Nets
    int32_t groundRect = -1;
    int32_t powerRect = -1;
};

struct CellLibrary {
    std::string name;
    double userUnit;
    double dbUnit;
    std::vector<Cell> cells;
    std::map<std::string, size_t> cellMap;
    std::string topCellName;
};

using GDSLayerID = std::pair<int16_t, int16_t>;

inline LayerID getLayerID(int16_t layer, int16_t datatype) {
    static const std::map<std::pair<int16_t, int16_t>, LayerID> Map = {
        // Sky130
        {{64,20}, L_NWELL},
        {{65,20}, L_DIFF},
        {{66,20}, L_POLY}, {{66,44}, L_LICON},
        {{67,20}, L_LI1 }, {{67,44}, L_MCON},
        {{67, 5}, L_LI1},   // labels
        {{68,20}, L_MET1}, {{68,44}, L_VIA1},
        {{68, 5}, L_MET1},  // labels
        {{69,20}, L_MET2}, {{69,44}, L_VIA2},
        {{69, 5}, L_MET2},  // labels
        {{70,20}, L_MET3}, {{70,44}, L_VIA3},
        {{70, 5}, L_MET3},  // labels
        {{71,20}, L_MET4}, {{71,44}, L_VIA4},
        {{71, 5}, L_MET4},  // labels
        {{72,20}, L_MET5},

        // IHP130
        {{31, 0}, L_NWELL},
        {{ 1, 0}, L_DIFF},
        {{ 5, 0}, L_POLY}, {{ 6, 0}, L_LICON},
        {{ 8, 0}, L_LI1},  {{19, 0}, L_MCON},
        {{ 8, 1}, L_LI1},  // labels
        {{ 8, 2}, L_LI1},  // labels
        {{ 8,25}, L_LI1},  // labels
        {{10, 0}, L_MET1}, {{29, 0}, L_VIA1},
        {{10,25}, L_MET1}, // labels
        {{30, 0}, L_MET2}, {{49, 0}, L_VIA2},
        {{50, 0}, L_MET3}, {{66, 0}, L_VIA3},
        {{67, 0}, L_MET4}, 
        {{67,25}, L_MET4}  // labels
    };
    auto it = Map.find({layer, datatype});
    return (it != Map.end()) ? it->second : L_COUNT;
}
