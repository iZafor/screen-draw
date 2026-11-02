#pragma once

#include <cairo.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ─── Tool & Preset Constants ─────────────────────────────────────────────────

enum class ToolType {
    Select,
    Pen,
    Eraser,
    Text,
    Line,
    Rect,
    Circle,
    Arrow
};

struct ColorPreset {
    const char* hex;
    const char* name;
};

static const ColorPreset COLOR_PRESETS[] = {
    {"#FF3B30", "Red"},
    {"#FF9500", "Orange"},
    {"#FFCC00", "Yellow"},
    {"#34C759", "Green"},
    {"#007AFF", "Blue"},
    {"#5856D6", "Indigo"},
    {"#AF52DE", "Purple"},
    {"#FF2D55", "Pink"},
    {"#FFFFFF", "White"},
    {"#8E8E93", "Gray"},
    {"#000000", "Black"},
};
static constexpr int NUM_COLORS = sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]);

static const int STROKE_PRESETS[] = {2, 3, 5, 8, 12, 18, 26};
static constexpr int NUM_STROKES = sizeof(STROKE_PRESETS) / sizeof(STROKE_PRESETS[0]);

static const int ERASER_RADIUS_PRESETS[] = {6, 10, 16, 24, 36, 50};
static constexpr int NUM_ERASER_RADII = sizeof(ERASER_RADIUS_PRESETS) / sizeof(ERASER_RADIUS_PRESETS[0]);

static const int TEXT_SIZE_PRESETS[] = {16, 24, 32, 48, 64, 96};
static constexpr int NUM_TEXT_SIZES = sizeof(TEXT_SIZE_PRESETS) / sizeof(TEXT_SIZE_PRESETS[0]);

static constexpr double TOOLBAR_HEIGHT = 50.0;
static constexpr double TOOLBAR_PADDING = 5.0;
static constexpr double TOOLBAR_RADIUS = 16.0;
static constexpr double TOOLBAR_BTN_SIZE = 40.0;
static constexpr double TOOLBAR_GAP = 4.0;

static constexpr double SM_COLOR_SWATCH = 26.0;
static constexpr double SM_COLOR_GAP = 8.0;
static constexpr double SM_STROKE_PILL_W = 34.0;
static constexpr double SM_STROKE_PILL_H = 26.0;

static constexpr double HANDLE_RADIUS = 5.5;
static constexpr double HANDLE_HIT_RADIUS = 10.0;

// ─── Resize Handle Identifiers ───────────────────────────────────────────────

enum class HandleId {
    None = 0,
    // For Line / Arrow direct endpoints:
    LineStart,
    LineEnd,
    // Standard 8-way bounding box handles:
    NW,
    N,
    NE,
    E,
    SE,
    S,
    SW,
    W,
    // Body drag (move):
    Body
};

struct HandlePoint {
    HandleId id;
    double x;
    double y;
};

// ─── Toolbar & Submenu Structs ───────────────────────────────────────────────

struct ToolbarButton {
    std::string name;
    std::string tip;
    double x = 0, y = 0, w = 0, h = 0;
};

struct SubmenuItem {
    std::string type;
    std::string str_val;
    int int_val = 0;
    double x = 0, y = 0, w = 0, h = 0;
};

// ─── Geometry & Color Helpers ────────────────────────────────────────────────

struct Point {
    double x, y;
};

struct RGBA {
    double r, g, b, a;
};

struct BBox {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    double width() const { return x2 - x1; }
    double height() const { return y2 - y1; }
    bool valid() const { return x2 >= x1 && y2 >= y1; }

    BBox padded(double p) const {
        return {x1 - p, y1 - p, x2 + p, y2 + p};
    }

    BBox united(const BBox& o) const {
        if (!valid()) return o;
        if (!o.valid()) return *this;
        return {std::min(x1, o.x1), std::min(y1, o.y1),
                std::max(x2, o.x2), std::max(y2, o.y2)};
    }

    bool intersects(double rx, double ry, double rw, double rh) const {
        return !(x2 < rx || x1 > rx + rw || y2 < ry || y1 > ry + rh);
    }

    bool contains(double px, double py) const {
        return px >= x1 && px <= x2 && py >= y1 && py <= y2;
    }
};

int64_t monotonic_us();
RGBA hex_to_rgba(const std::string& hex_str, double alpha = 1.0);
void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);
double point_to_segment_dist(double px, double py, double x1, double y1, double x2, double y2);
bool segment_intersects_box(double x1, double y1, double x2, double y2, const BBox& box);
void append_utf8(std::string& out, uint32_t cp);
size_t cursor_index_from_x(const std::string& str, double font_sz, double start_x, double target_x);
