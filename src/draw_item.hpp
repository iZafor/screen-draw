#pragma once

#include "geometry.hpp"

// ─── Drawable Item Representation ────────────────────────────────────────────

enum class StrokeKind {
    Freehand,
    Shape,
    Text
};

struct DrawItem {
    StrokeKind kind = StrokeKind::Shape;

    // Common style
    std::string color = "#FF3B30";
    double width = 5.0;
    bool is_eraser = false;

    // Freehand points
    std::vector<Point> points;

    // Shape data (Line, Rect, Circle, Arrow)
    ToolType shape_type = ToolType::Rect;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    // Text data
    std::string text;
    double text_x = 0, text_y = 0;
    double font_size = 24.0;
    std::string bg_color = "transparent";

    // Cached text metrics (updated on draw / creation)
    mutable double cached_text_w = 0;
    mutable double cached_text_h = 0;
    mutable double cached_x_bearing = 0;
    mutable double cached_y_bearing = 0;
    mutable bool text_metrics_valid = false;

    bool is_selectable() const {
        return !is_eraser;
    }

    void update_text_metrics() const;
    BBox raw_bounds() const;
    BBox visual_bounds(bool include_handles = true) const;
    void draw(cairo_t* cr) const;
    bool hit_test(double px, double py, bool allow_interior_bbox = false) const;
    bool intersects_box(const BBox& box) const;
    std::vector<HandlePoint> get_handles() const;
    HandleId hit_handle(double px, double py) const;
    void apply_move_from(const DrawItem& orig, double dx, double dy);
    void apply_resize_from(const DrawItem& orig, HandleId hid, double dx, double dy);
    void draw_selection_overlay(cairo_t* cr, bool show_handles = true) const;
};
