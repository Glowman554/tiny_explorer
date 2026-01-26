#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "cells.h"

class GdsReader {
    const uint8_t* buffer = nullptr;
    const uint8_t* end = nullptr;
    const uint8_t* pos = nullptr;
    size_t totalRead = 0;

public:
    GdsReader(const uint8_t* buf, size_t len) 
        : buffer(buf), end(buf + len), pos(buf) {}

    uint8_t readU8() {
        totalRead++;
        if (pos >= end) return 0;
        return *pos++;
    }

    size_t tell() const { return totalRead; }

    uint16_t readU16() {
        uint16_t val = (uint16_t)readU8() << 8;
        val |= readU8();
        return val;
    }

    int16_t readI16() { return (int16_t)readU16(); }

    int32_t readI32() {
        uint32_t val = (uint32_t)readU8() << 24;
        val |= (uint32_t)readU8() << 16;
        val |= (uint32_t)readU8() << 8;
        val |= (uint32_t)readU8();
        return (int32_t)val;
    }

    double readReal8() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | readU8();
        int sign = (v >> 63) ? -1 : 1;
        int exponent = (int)((v >> 56) & 0x7f) - 64;
        uint64_t mantissa = v & 0x00ffffffffffffffULL;
        return sign * (double)mantissa * pow(16.0, exponent - 14);
    }

    std::string readString(int len) {
        std::string s;
        bool done = false;
        for (int i = 0; i < len; ++i) {
            uint8_t c = readU8();
            if (c == 0) done = true;
            if (!done) s += (char)c;
        }
        return s;
    }

    void skip(int len) {
        if (len <= 0) return;
        totalRead += len;
        size_t avail = (pos < end) ? (size_t)(end - pos) : 0;
        pos += std::min((size_t)len, avail);
    }
};

namespace Record {
    const uint8_t HEADER = 0x00, BGNLIB = 0x01, LIBNAME = 0x02, UNITS = 0x03,
                  ENDLIB = 0x04, BGNSTR = 0x05, STRNAME = 0x06, ENDSTR = 0x07,
                  BOUNDARY = 0x08, PATH = 0x09, SREF = 0x0A, AREF = 0x0B,
                  TEXT = 0x0C, LAYER = 0x0D, DATATYPE = 0x0E, WIDTH = 0x0F,
                  XY = 0x10, ENDEL = 0x11, SNAME = 0x12, COLROW = 0x13,
                  TEXTNODE = 0x14, NODE = 0x15, TEXTTYPE = 0x16, PRESENTATION = 0x17,
                  STRING = 0x19, STRANS = 0x1A, MAG = 0x1B, ANGLE = 0x1C,
                  PATHTYPE = 0x21;
}

struct GDSParser {
    CellLibrary & lib;
    using CellCallback = std::function<void(Cell&, CellLibrary& lib)>;
    CellCallback onCell;
    bool isFinished = false;

    Cell* currentCell = nullptr;
    int16_t currentLayer = 0;
    int16_t currentDatatype = 0;
    std::vector<Point> currentPoints;
    bool isBoundary = false;
    bool isPath = false;
    int32_t currentWidth = 0;
    int16_t currentPathType = 0;
    Reference* currentRef = nullptr;
    Text* currentText = nullptr;
    std::vector<uint8_t> backlog;

    GDSParser(CellLibrary & lib, CellCallback onCell) : lib(lib), onCell(onCell) {}

    void processRecord(uint8_t type, GdsReader& reader, int dataLen) {
        if (type == Record::ENDLIB) {
            isFinished = true;
            return;
        }
        size_t start = reader.tell();
        switch (type) {
            case Record::LIBNAME: lib.name = reader.readString(dataLen); break;
            case Record::UNITS:
                lib.userUnit = reader.readReal8();
                lib.dbUnit = reader.readReal8();
                break;
            case Record::BGNSTR:
                lib.cells.emplace_back();
                currentCell = &lib.cells.back();
                currentCell->tempRects.resize(L_COUNT);
                break;
            case Record::STRNAME:
                if (currentCell) {
                    currentCell->name = reader.readString(dataLen);
                    lib.cellMap[currentCell->name] = lib.cells.size() - 1;
                }
                break;
            case Record::ENDSTR:
                if (currentCell) { onCell(*currentCell, lib); }
                currentCell = nullptr; currentRef = nullptr; currentText = nullptr;
                break;

            case Record::BOUNDARY:
                isBoundary = true; isPath = false;
                currentPoints.clear(); currentLayer = 0; currentDatatype = 0;
                break;
            case Record::PATH:
                isPath = true; isBoundary = false;
                currentPoints.clear(); currentLayer = 0; currentDatatype = 0;
                currentWidth = 0; currentPathType = 0;
                break;
            case Record::SREF:
            case Record::AREF:
                if (currentCell) {
                    currentCell->references.emplace_back();
                    currentRef = &currentCell->references.back();
                    currentRef->type = (type == Record::SREF) ? Reference::SREF : Reference::AREF;
                }
                break;
            case Record::TEXT:
                if (currentCell) {
                    currentCell->texts.emplace_back();
                    currentText = &currentCell->texts.back();
                }
                break;
            case Record::LAYER:
                if (isBoundary || isPath) currentLayer = reader.readI16();
                else if (currentText) currentText->layer = reader.readI16();
                break;
            case Record::WIDTH: currentWidth = reader.readI32(); break;
            case Record::PATHTYPE: currentPathType = reader.readI16(); break;
            case Record::DATATYPE:
            case Record::TEXTTYPE:
                if (isBoundary || isPath) currentDatatype = reader.readI16();
                else if (currentText) currentText->texttype = reader.readI16();
                break;
            case Record::XY:
                if (isBoundary || isPath) {
                    int n = dataLen / 4;
                    for (int i = 0; i < n / 2; ++i) currentPoints.push_back({reader.readI32(), reader.readI32()});
                } else if (currentRef) {
                    int n = dataLen / 4;
                    for (int i = 0; i < n / 2; ++i) currentRef->points.push_back({reader.readI32(), reader.readI32()});
                } else if (currentText) {
                    currentText->point = {reader.readI32(), reader.readI32()};
                }
                break;
            case Record::COLROW:
                if (currentRef) { currentRef->cols = reader.readI16(); currentRef->rows = reader.readI16(); }
                break;
            case Record::SNAME: if (currentRef) currentRef->cellName = reader.readString(dataLen); break;
            case Record::STRING: if (currentText) currentText->content = reader.readString(dataLen); break;
            case Record::STRANS:
                if (currentRef) { uint16_t f = reader.readU16(); currentRef->reflection = (f & 0x8000) != 0; }
                else if (currentText) { uint16_t f = reader.readU16(); currentText->reflection = (f & 0x8000) != 0; }
                break;
            case Record::MAG:
                if (currentRef) currentRef->mag = reader.readReal8();
                else if (currentText) currentText->mag = reader.readReal8();
                break;
            case Record::ANGLE:
                if (currentRef) currentRef->angle = reader.readReal8();
                else if (currentText) currentText->angle = reader.readReal8();
                break;
            case Record::ENDEL:
                if (isBoundary && currentCell) {
                    LayerID lid = getLayerID(currentLayer, currentDatatype);
                    if (lid != L_COUNT) fracture(currentPoints, currentCell->tempRects[lid]);
                } else if (isPath && currentCell) {
                    LayerID lid = getLayerID(currentLayer, currentDatatype);
                    if (lid != L_COUNT) convertPathToRects(currentPoints, currentWidth, currentPathType, currentCell->tempRects[lid]);
                }
                isBoundary = false; isPath = false; currentRef = nullptr; currentText = nullptr;
                break;
            default: break;
        }
        size_t consumed = reader.tell() - start;
        if (consumed < (size_t)dataLen) reader.skip(dataLen - consumed);
    }

    void consumeChunk(const uint8_t* data, size_t len) {
        if (len == 0) return;
        const uint8_t* p = data;
        const uint8_t* end = data + len;

        // Combine with backlog if necessary
        if (!backlog.empty()) {
            while (p < end && backlog.size() < 2) backlog.push_back(*p++);
            if (backlog.size() < 2) return;
            uint16_t rlen = ((uint16_t)backlog[0] << 8) | backlog[1];
            while (p < end && backlog.size() < rlen) backlog.push_back(*p++);
            if (backlog.size() < rlen) return;

            GdsReader r(backlog.data(), backlog.size());
            r.readU16(); // Skip len
            uint16_t head = r.readU16();
            processRecord(head >> 8, r, rlen - 4);
            backlog.clear();
        }

        while (p + 4 <= end) {
            uint16_t rlen = ((uint16_t)p[0] << 8) | p[1];
            if (rlen < 4) { p += 2; continue; }
            if (p + rlen > end) break;

            GdsReader r(p, rlen);
            r.readU16(); // Skip len
            uint16_t head = r.readU16();
            processRecord(head >> 8, r, rlen - 4);
            p += rlen;
        }

        if (p < end) {
            backlog.assign(p, end);
        }
    }
};