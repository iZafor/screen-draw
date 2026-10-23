// Screen Draw — Native C++17 / GTK3 / Cairo Screen Annotation Overlay
// Fast, hardware-composited, dirty-region optimized UI with movable & resizable shapes.

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <memory>
#include <string>
#include <vector>

// ─── D-Bus Constants ─────────────────────────────────────────────────────────

static const char* DBUS_NAME  = "com.tools.ScreenDraw";
static const char* DBUS_PATH  = "/com/tools/ScreenDraw";
static const char* DBUS_IFACE = "com.tools.ScreenDraw";

static const char* DBUS_XML =
    "<node>"
    "  <interface name='com.tools.ScreenDraw'>"
    "    <method name='Toggle'/>"
    "    <method name='Show'/>"
    "    <method name='Hide'/>"
    "    <method name='Quit'/>"
    "  </interface>"
    "</node>";

static const char* GS_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys";
static const char* GS_KEY = "custom-keybindings";
static const char* GS_CUSTOM_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding";
static const char* KEYBINDING_SLOT = "screen-draw";
static const char* KEYBINDING_PATH_PREFIX = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/";

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

// ─── Geometry & Color Helpers ────────────────────────────────────────────────

struct RGBA {
    double r, g, b, a;
};

static RGBA hex_to_rgba(const std::string& hex_str, double alpha = 1.0) {
    if (hex_str.empty()) return {1.0, 0.0, 0.0, alpha};
    const char* s = hex_str.c_str();
    if (s[0] == '#') s++;
    if ( std::strlen(s) < 6 ) return {1.0, 0.0, 0.0, alpha};
    unsigned int rv = 0, gv = 0, bv = 0;
    std::sscanf(s, "%02x%02x%02x", &rv, &gv, &bv);
    return {rv / 255.0, gv / 255.0, bv / 255.0, alpha};
}

static void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (w <= 0 || h <= 0) return;
    r = std::min({r, w * 0.5, h * 0.5});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

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

static double point_to_segment_dist(double px, double py, double x1, double y1, double x2, double y2) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double len_sq = dx * dx + dy * dy;
    if (len_sq < 1e-9) {
        return std::hypot(px - x1, py - y1);
    }
    double t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    t = std::clamp(t, 0.0, 1.0);
    double proj_x = x1 + t * dx;
    double proj_y = y1 + t * dy;
    return std::hypot(px - proj_x, py - proj_y);
}

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

// ─── Drawable Item Representation ────────────────────────────────────────────

enum class StrokeKind {
    Freehand,
    Shape,
    Text
};

struct Point {
    double x, y;
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

    void update_text_metrics() const {
        if (kind != StrokeKind::Text) return;
        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr = cairo_create(tmp);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, text.c_str(), &ext);
        cached_text_w = std::max(ext.width, 10.0);
        cached_text_h = std::max(ext.height, font_size * 0.75);
        cached_x_bearing = ext.x_bearing;
        cached_y_bearing = (ext.height > 0) ? ext.y_bearing : -font_size * 0.75;
        text_metrics_valid = true;
        cairo_destroy(cr);
        cairo_surface_destroy(tmp);
    }

    // Tight geometric bounding box (without stroke padding)
    BBox raw_bounds() const {
        if (kind == StrokeKind::Shape) {
            return {std::min(x1, x2), std::min(y1, y2),
                    std::max(x1, x2), std::max(y1, y2)};
        } else if (kind == StrokeKind::Freehand) {
            if (points.empty()) return {0, 0, 0, 0};
            double min_x = points[0].x, max_x = points[0].x;
            double min_y = points[0].y, max_y = points[0].y;
            for (const auto& p : points) {
                min_x = std::min(min_x, p.x);
                max_x = std::max(max_x, p.x);
                min_y = std::min(min_y, p.y);
                max_y = std::max(max_y, p.y);
            }
            return {min_x, min_y, max_x, max_y};
        } else { // Text
            if (!text_metrics_valid) update_text_metrics();
            double pad = (bg_color != "transparent" && !bg_color.empty()) ? 8.0 : 2.0;
            double bx1 = text_x + cached_x_bearing - pad;
            double by1 = text_y + cached_y_bearing - pad;
            double bx2 = bx1 + cached_text_w + pad * 2.0;
            double by2 = by1 + cached_text_h + pad * 2.0;
            return {bx1, by1, bx2, by2};
        }
    }

    // Visual bounding box including stroke width, arrowhead, and selection handles
    BBox visual_bounds(bool include_handles = true) const {
        BBox b = raw_bounds();
        double pad = width * 0.5 + 4.0;
        if (kind == StrokeKind::Shape && shape_type == ToolType::Arrow) {
            pad = std::max(pad, std::max(width * 3.5, 20.0) + width + 4.0);
        }
        if (include_handles) {
            pad = std::max(pad, HANDLE_RADIUS + 8.0);
        }
        return b.padded(pad);
    }

    void draw(cairo_t* cr) const {
        if (kind == StrokeKind::Freehand) {
            if (points.size() < 2) return;
            if (is_eraser) {
                cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
            } else {
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                RGBA c = hex_to_rgba(color);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
            }
            cairo_set_line_width(cr, width);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

            cairo_move_to(cr, points[0].x, points[0].y);
            if (points.size() == 2) {
                cairo_line_to(cr, points[1].x, points[1].y);
            } else {
                for (size_t i = 1; i + 1 < points.size(); ++i) {
                    double xc = (points[i].x + points[i + 1].x) * 0.5;
                    double yc = (points[i].y + points[i + 1].y) * 0.5;
                    cairo_curve_to(cr, points[i].x, points[i].y,
                                   points[i].x, points[i].y, xc, yc);
                }
                cairo_line_to(cr, points.back().x, points.back().y);
            }
            cairo_stroke(cr);
        } else if (kind == StrokeKind::Shape) {
            RGBA c = hex_to_rgba(color);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
            cairo_set_line_width(cr, width);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

            if (shape_type == ToolType::Line) {
                cairo_move_to(cr, x1, y1);
                cairo_line_to(cr, x2, y2);
                cairo_stroke(cr);
            } else if (shape_type == ToolType::Rect) {
                double rx = std::min(x1, x2);
                double ry = std::min(y1, y2);
                double rw = std::abs(x2 - x1);
                double rh = std::abs(y2 - y1);
                if (rw > 0.5 || rh > 0.5) {
                    cairo_rectangle(cr, rx, ry, std::max(rw, 1.0), std::max(rh, 1.0));
                    cairo_stroke(cr);
                }
            } else if (shape_type == ToolType::Circle) {
                double cx = (x1 + x2) * 0.5;
                double cy = (y1 + y2) * 0.5;
                double rx = std::abs(x2 - x1) * 0.5;
                double ry = std::abs(y2 - y1) * 0.5;
                if (rx > 0.5 && ry > 0.5) {
                    cairo_save(cr);
                    cairo_translate(cr, cx, cy);
                    cairo_scale(cr, 1.0, ry / rx);
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, 0.0, 0.0, rx, 0.0, 2.0 * M_PI);
                    cairo_restore(cr);
                    cairo_stroke(cr);
                }
            } else if (shape_type == ToolType::Arrow) {
                cairo_move_to(cr, x1, y1);
                cairo_line_to(cr, x2, y2);
                cairo_stroke(cr);

                double angle = std::atan2(y2 - y1, x2 - x1);
                double head_len = std::max(width * 3.5, 20.0);
                double spread = M_PI / 6.0;
                for (int sign : {-1, 1}) {
                    double lx = x2 - head_len * std::cos(angle + sign * spread);
                    double ly = y2 - head_len * std::sin(angle + sign * spread);
                    cairo_move_to(cr, x2, y2);
                    cairo_line_to(cr, lx, ly);
                    cairo_stroke(cr);
                }
            }
        } else if (kind == StrokeKind::Text) {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, font_size);

            cairo_text_extents_t ext;
            cairo_text_extents(cr, text.c_str(), &ext);
            cached_text_w = std::max(ext.width, 10.0);
            cached_text_h = std::max(ext.height, font_size * 0.75);
            cached_x_bearing = ext.x_bearing;
            cached_y_bearing = (ext.height > 0) ? ext.y_bearing : -font_size * 0.75;
            text_metrics_valid = true;

            if (!bg_color.empty() && bg_color != "transparent") {
                RGBA bg = hex_to_rgba(bg_color);
                cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
                double pad = 8.0;
                cairo_rectangle(cr,
                                text_x + ext.x_bearing - pad,
                                text_y + ext.y_bearing - pad,
                                ext.width + pad * 2.0,
                                ext.height + pad * 2.0);
                cairo_fill(cr);
            }

            RGBA fg = hex_to_rgba(color);
            cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
            cairo_move_to(cr, text_x, text_y);
            cairo_show_text(cr, text.c_str());
        }
    }

    // Hit test for selecting / moving an item
    bool hit_test(double px, double py, bool allow_interior_bbox = false) const {
        if (!is_selectable()) return false;
        double tol = std::max(width * 0.5 + 8.0, 10.0);

        if (kind == StrokeKind::Shape) {
            if (shape_type == ToolType::Line || shape_type == ToolType::Arrow) {
                double d = point_to_segment_dist(px, py, x1, y1, x2, y2);
                if (d <= tol) return true;
                if (shape_type == ToolType::Arrow) {
                    double angle = std::atan2(y2 - y1, x2 - x1);
                    double head_len = std::max(width * 3.5, 20.0);
                    double spread = M_PI / 6.0;
                    for (int sign : {-1, 1}) {
                        double lx = x2 - head_len * std::cos(angle + sign * spread);
                        double ly = y2 - head_len * std::sin(angle + sign * spread);
                        if (point_to_segment_dist(px, py, x2, y2, lx, ly) <= tol) return true;
                    }
                }
                return false;
            } else if (shape_type == ToolType::Rect) {
                BBox b = raw_bounds();
                if (!b.padded(tol).contains(px, py)) return false;
                if (allow_interior_bbox) return true;
                // Check proximity to any of the 4 edges or inside small shapes
                if (b.width() < 24 || b.height() < 24) return true;
                bool near_x = (std::abs(px - b.x1) <= tol || std::abs(px - b.x2) <= tol) &&
                              (py >= b.y1 - tol && py <= b.y2 + tol);
                bool near_y = (std::abs(py - b.y1) <= tol || std::abs(py - b.y2) <= tol) &&
                              (px >= b.x1 - tol && px <= b.x2 + tol);
                return near_x || near_y;
            } else if (shape_type == ToolType::Circle) {
                BBox b = raw_bounds();
                if (!b.padded(tol).contains(px, py)) return false;
                if (allow_interior_bbox) return true;
                double cx = (x1 + x2) * 0.5;
                double cy = (y1 + y2) * 0.5;
                double rx = std::max(std::abs(x2 - x1) * 0.5, 1.0);
                double ry = std::max(std::abs(y2 - y1) * 0.5, 1.0);
                if (rx < 15 || ry < 15) return true;
                double nx = (px - cx) / rx;
                double ny = (py - cy) / ry;
                double norm_dist = std::hypot(nx, ny);
                double avg_r = (rx + ry) * 0.5;
                double pixel_delta = std::abs(norm_dist - 1.0) * avg_r;
                return pixel_delta <= tol;
            }
        } else if (kind == StrokeKind::Text) {
            BBox b = raw_bounds().padded(4.0);
            return b.contains(px, py);
        } else if (kind == StrokeKind::Freehand) {
            BBox b = raw_bounds();
            if (!b.padded(tol).contains(px, py)) return false;
            if (allow_interior_bbox) return true;
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                if (point_to_segment_dist(px, py, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y) <= tol) {
                    return true;
                }
            }
        }
        return false;
    }

    // Return interactive resize handles for this item
    std::vector<HandlePoint> get_handles() const {
        std::vector<HandlePoint> handles;
        if (kind == StrokeKind::Shape && (shape_type == ToolType::Line || shape_type == ToolType::Arrow)) {
            handles.push_back({HandleId::LineStart, x1, y1});
            handles.push_back({HandleId::LineEnd, x2, y2});
            return handles;
        }
        BBox b = raw_bounds();
        double cx = (b.x1 + b.x2) * 0.5;
        double cy = (b.y1 + b.y2) * 0.5;
        handles.push_back({HandleId::NW, b.x1, b.y1});
        handles.push_back({HandleId::NE, b.x2, b.y1});
        handles.push_back({HandleId::SE, b.x2, b.y2});
        handles.push_back({HandleId::SW, b.x1, b.y2});
        if (kind != StrokeKind::Text) {
            handles.push_back({HandleId::N, cx, b.y1});
            handles.push_back({HandleId::E, b.x2, cy});
            handles.push_back({HandleId::S, cx, b.y2});
            handles.push_back({HandleId::W, b.x1, cy});
        }
        return handles;
    }

    HandleId hit_handle(double px, double py) const {
        auto handles = get_handles();
        for (const auto& h : handles) {
            if (std::hypot(px - h.x, py - h.y) <= HANDLE_HIT_RADIUS) {
                return h.id;
            }
        }
        return HandleId::None;
    }

    // Move the item by (dx, dy) from an initial snapshot
    void apply_move_from(const DrawItem& orig, double dx, double dy) {
        if (kind == StrokeKind::Shape) {
            x1 = orig.x1 + dx;
            y1 = orig.y1 + dy;
            x2 = orig.x2 + dx;
            y2 = orig.y2 + dy;
        } else if (kind == StrokeKind::Text) {
            text_x = orig.text_x + dx;
            text_y = orig.text_y + dy;
        } else if (kind == StrokeKind::Freehand) {
            points.resize(orig.points.size());
            for (size_t i = 0; i < orig.points.size(); ++i) {
                points[i].x = orig.points[i].x + dx;
                points[i].y = orig.points[i].y + dy;
            }
        }
    }

    // Resize the item by dragging handle `hid` by (dx, dy) from `orig`
    void apply_resize_from(const DrawItem& orig, HandleId hid, double dx, double dy) {
        if (kind == StrokeKind::Shape && (shape_type == ToolType::Line || shape_type == ToolType::Arrow)) {
            if (hid == HandleId::LineStart) {
                x1 = orig.x1 + dx;
                y1 = orig.y1 + dy;
                x2 = orig.x2;
                y2 = orig.y2;
                return;
            } else if (hid == HandleId::LineEnd) {
                x1 = orig.x1;
                y1 = orig.y1;
                x2 = orig.x2 + dx;
                y2 = orig.y2 + dy;
                return;
            }
        }

        BBox ob = orig.raw_bounds();
        double nx1 = ob.x1, ny1 = ob.y1, nx2 = ob.x2, ny2 = ob.y2;

        if (hid == HandleId::NW || hid == HandleId::W || hid == HandleId::SW) nx1 += dx;
        if (hid == HandleId::NE || hid == HandleId::E || hid == HandleId::SE) nx2 += dx;
        if (hid == HandleId::NW || hid == HandleId::N || hid == HandleId::NE) ny1 += dy;
        if (hid == HandleId::SW || hid == HandleId::S || hid == HandleId::SE) ny2 += dy;

        if (kind == StrokeKind::Shape) {
            // Preserve orientation or allow flipping smoothly
            x1 = nx1;
            y1 = ny1;
            x2 = nx2;
            y2 = ny2;
        } else if (kind == StrokeKind::Text) {
            double orig_diag = std::max(std::hypot(ob.width(), ob.height()), 8.0);
            double new_diag = std::max(std::hypot(nx2 - nx1, ny2 - ny1), 8.0);
            double scale = new_diag / orig_diag;
            font_size = std::clamp(orig.font_size * scale, 10.0, 240.0);
            text_x = orig.text_x;
            text_y = orig.text_y;
            text_metrics_valid = false;
            update_text_metrics();
            BBox nb = raw_bounds();
            // Lock the opposite anchor corner so resizing is rock-solid from any corner
            if (hid == HandleId::NE || hid == HandleId::SE) {
                text_x += (ob.x1 - nb.x1);
            } else {
                text_x += (ob.x2 - nb.x2);
            }
            if (hid == HandleId::SW || hid == HandleId::SE) {
                text_y += (ob.y1 - nb.y1);
            } else {
                text_y += (ob.y2 - nb.y2);
            }
        } else if (kind == StrokeKind::Freehand) {
            double ow = std::max(ob.width(), 1.0);
            double oh = std::max(ob.height(), 1.0);
            double nw = nx2 - nx1;
            double nh = ny2 - ny1;
            if ( std::abs(nw) < 4.0 ) nw = (nw >= 0 ? 4.0 : -4.0);
            if ( std::abs(nh) < 4.0 ) nh = (nh >= 0 ? 4.0 : -4.0);
            points.resize(orig.points.size());
            for (size_t i = 0; i < orig.points.size(); ++i) {
                double tx = (orig.points[i].x - ob.x1) / ow;
                double ty = (orig.points[i].y - ob.y1) / oh;
                points[i].x = nx1 + tx * nw;
                points[i].y = ny1 + ty * nh;
            }
        }
    }

    // Draw selection box and interactive handles
    void draw_selection_overlay(cairo_t* cr) const {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        if (kind == StrokeKind::Shape && (shape_type == ToolType::Line || shape_type == ToolType::Arrow)) {
            // Subtle dashed connector line
            double dashes[] = {4.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_set_line_width(cr, 1.2);
            cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 0.85);
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0.0);
        } else {
            BBox b = raw_bounds().padded(3.0);
            double dashes[] = {5.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_set_line_width(cr, 1.5);
            cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 0.90);
            cairo_rectangle(cr, b.x1, b.y1, b.width(), b.height());
            cairo_stroke(cr);
            cairo_set_dash(cr, nullptr, 0, 0.0);
        }

        // Draw handles
        auto handles = get_handles();
        for (const auto& h : handles) {
            // Outer shadow
            cairo_arc(cr, h.x, h.y + 1.0, HANDLE_RADIUS + 1.0, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
            cairo_fill(cr);

            // White fill
            cairo_arc(cr, h.x, h.y, HANDLE_RADIUS, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(cr);

            // Accent border
            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 1.0);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);
        }

        cairo_restore(cr);
    }
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

// ─── Main Application Window State ───────────────────────────────────────────

class ScreenDrawWindow {
public:
    GtkWidget* window = nullptr;
    int mon_width = 1920;
    int mon_height = 1080;

    // Document state & Undo/Redo history (snapshots of items + selection)
    std::vector<DrawItem> strokes;
    std::vector<std::vector<DrawItem>> undo_stack;
    std::vector<std::vector<DrawItem>> redo_stack;

    // Selection & Move/Resize state
    int selected_index = -1;
    bool is_manipulating = false; // moving or resizing selected item
    HandleId active_handle = HandleId::None;
    double drag_start_x = 0.0;
    double drag_start_y = 0.0;
    DrawItem drag_orig_item;
    bool drag_modified = false;

    // Active drawing state
    bool is_drawing = false;
    std::vector<Point> current_points;
    Point shape_start = {0, 0};
    Point shape_end = {0, 0};

    // Tool state
    ToolType current_tool = ToolType::Pen;
    std::string current_color = "#FF3B30";
    double stroke_width = 5.0;
    double eraser_radius = 16.0;
    bool is_visible = false;

    // Text typing state
    bool is_typing = false;
    double typing_x = 0.0;
    double typing_y = 0.0;
    std::string typing_text;
    guint blink_timer_id = 0;
    double text_size = 24.0;
    std::string text_color = "#FF3B30";
    std::string text_bg_color = "transparent";

    // UI state
    std::string submenu_open; // "", "pen_options", "eraser_options", "text_options"
    std::vector<SubmenuItem> submenu_items;
    std::vector<ToolbarButton> toolbar_buttons;
    std::string hover_button;
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    guint32 last_toolbar_click_time = 0;
    bool passthrough_mode = false;

    // Offscreen Cairo surfaces for blazing fast rendering
    cairo_surface_t* canvas_surface = nullptr;
    int canvas_skip_index = -1; // When actively moving/resizing an item, exclude it from static canvas_surface
    cairo_surface_t* toolbar_cache_surface = nullptr;
    bool toolbar_cache_dirty = true;
    int cached_toolbar_width = -1;

    // Timers & Cursors
    guint focus_keepalive_id = 0;
    std::string current_cursor_name;

    ScreenDrawWindow() {
        setup_window();
        build_toolbar();
        connect_events();
    }

    ~ScreenDrawWindow() {
        stop_focus_keepalive();
        stop_cursor_blink();
        if (canvas_surface) cairo_surface_destroy(canvas_surface);
        if (toolbar_cache_surface) cairo_surface_destroy(toolbar_cache_surface);
    }

    void push_undo_snapshot() {
        undo_stack.push_back(strokes);
        if (undo_stack.size() > 100) {
            undo_stack.erase(undo_stack.begin());
        }
        redo_stack.clear();
    }

    void setup_window() {
        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Screen Draw");

        GdkScreen* screen = gtk_widget_get_screen(window);
        GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
        if (visual) {
            gtk_widget_set_visual(window, visual);
        }

        gtk_widget_set_app_paintable(window, TRUE);
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(window), TRUE);

        GdkDisplay* display = gdk_display_get_default();
        GdkMonitor* monitor = gdk_display_get_primary_monitor(display);
        if (!monitor && gdk_display_get_n_monitors(display) > 0) {
            monitor = gdk_display_get_monitor(display, 0);
        }
        if (monitor) {
            GdkRectangle geom;
            gdk_monitor_get_geometry(monitor, &geom);
            mon_width = geom.width;
            mon_height = geom.height;
            gtk_window_set_default_size(GTK_WINDOW(window), mon_width, mon_height);
            gtk_window_move(GTK_WINDOW(window), geom.x, geom.y);
            gtk_window_resize(GTK_WINDOW(window), mon_width, mon_height);
        }

        gtk_widget_add_events(window,
                              GDK_BUTTON_PRESS_MASK |
                              GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK |
                              GDK_KEY_PRESS_MASK |
                              GDK_LEAVE_NOTIFY_MASK |
                              GDK_FOCUS_CHANGE_MASK);
    }

    void build_toolbar() {
        toolbar_buttons.clear();
        struct Def { const char* name; const char* tip; };
        static const Def defs[] = {
            {"select", "Select / Move & Resize (S)"},
            {"pen",    "Pen (P)"},
            {"eraser", "Eraser (E)"},
            {"__sep1", ""},
            {"text",   "Text (T)"},
            {"line",   "Line (L)"},
            {"rect",   "Rectangle (R)"},
            {"circle", "Circle (O)"},
            {"arrow",  "Arrow (A)"},
            {"__sep2", ""},
            {"undo",   "Undo (Ctrl+Z)"},
            {"redo",   "Redo (Ctrl+Y)"},
            {"clear",  "Clear (Ctrl+X)"},
            {"__sep3", ""},
            {"cursor", "Interact (C)"},
            {"close",  "Close (F9)"},
        };
        for (const auto& d : defs) {
            ToolbarButton b;
            b.name = d.name;
            b.tip = d.tip;
            b.w = (b.name.rfind("__sep", 0) == 0) ? 2.0 : TOOLBAR_BTN_SIZE;
            b.h = TOOLBAR_BTN_SIZE;
            toolbar_buttons.push_back(b);
        }
        toolbar_cache_dirty = true;
    }

    void layout_toolbar(int width) {
        if (width == cached_toolbar_width) return;
        cached_toolbar_width = width;
        toolbar_cache_dirty = true;

        double total_w = 0.0;
        for (const auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) == 0) {
                total_w += 12.0;
            } else {
                total_w += TOOLBAR_BTN_SIZE + TOOLBAR_GAP;
            }
        }
        double cx = (width - total_w) * 0.5;
        for (auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) == 0) {
                b.x = cx + 4.0;
                b.y = TOOLBAR_PADDING + 8.0;
                b.w = 2.0;
                b.h = TOOLBAR_BTN_SIZE - 16.0;
                cx += 12.0;
            } else {
                b.x = cx;
                b.y = TOOLBAR_PADDING;
                b.w = TOOLBAR_BTN_SIZE;
                b.h = TOOLBAR_BTN_SIZE;
                cx += TOOLBAR_BTN_SIZE + TOOLBAR_GAP;
            }
        }
    }

    BBox toolbar_bounds() const {
        const ToolbarButton* first = nullptr;
        const ToolbarButton* last = nullptr;
        for (const auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) != 0) {
                if (!first) first = &b;
                last = &b;
            }
        }
        if (!first || !last) return {0, 0, (double)mon_width, TOOLBAR_HEIGHT + 10};
        double bar_x = first->x - 18.0;
        double bar_w = (last->x + last->w) - first->x + 36.0;
        return {bar_x, 0.0, bar_x + bar_w, TOOLBAR_HEIGHT + 12.0};
    }

    // ── Canvas Management ─────────────────────────────────────────────────

    void ensure_canvas(int w, int h) {
        if (!canvas_surface ||
            cairo_image_surface_get_width(canvas_surface) != w ||
            cairo_image_surface_get_height(canvas_surface) != h) {
            cairo_surface_t* new_s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            if (canvas_surface) {
                cairo_t* cr = cairo_create(new_s);
                cairo_set_source_surface(cr, canvas_surface, 0.0, 0.0);
                cairo_paint(cr);
                cairo_destroy(cr);
                cairo_surface_destroy(canvas_surface);
            }
            canvas_surface = new_s;
        }
    }

    void rebuild_canvas(int skip_idx = -1) {
        if (!canvas_surface) return;
        canvas_skip_index = skip_idx;
        cairo_t* cr = cairo_create(canvas_surface);
        // Clear existing surface in-place without reallocating full-screen memory
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        for (size_t i = 0; i < strokes.size(); ++i) {
            if ((int)i == skip_idx) continue;
            strokes[i].draw(cr);
        }
        cairo_destroy(cr);
    }

    // ── Dirty Region Invalidation ─────────────────────────────────────────

    void invalidate_bbox(const BBox& b) {
        if (!window) return;
        int x = std::max(0, (int)std::floor(b.x1) - 4);
        int y = std::max(0, (int)std::floor(b.y1) - 4);
        int r = std::min(mon_width, (int)std::ceil(b.x2) + 4);
        int bot = std::min(mon_height, (int)std::ceil(b.y2) + 4);
        if (r > x && bot > y) {
            gtk_widget_queue_draw_area(window, x, y, r - x, bot - y);
        }
    }

    void invalidate_toolbar() {
        toolbar_cache_dirty = true;
        BBox tb = toolbar_bounds();
        tb.y2 += 36.0; // Include hover tooltip region below toolbar
        invalidate_bbox(tb);
    }

    void invalidate_full() {
        if (window) gtk_widget_queue_draw(window);
    }

    // ── Main Rendering ────────────────────────────────────────────────────

    static gboolean on_draw_cb(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
        return static_cast<ScreenDrawWindow*>(user_data)->on_draw(widget, cr);
    }

    gboolean on_draw(GtkWidget* widget, cairo_t* cr) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(widget, &alloc);
        int w = alloc.width;
        int h = alloc.height;
        ensure_canvas(w, h);
        layout_toolbar(w);

        GdkRectangle clip_rect;
        bool has_clip = gdk_cairo_get_clip_rectangle(cr, &clip_rect);
        if (!has_clip) {
            clip_rect = {0, 0, w, h};
        }

        // 1. Clear dirty region to transparent
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

        // 2. Subtle tint so user knows overlay is active (skip in passthrough mode)
        if (!passthrough_mode) {
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.03);
            cairo_paint(cr);
        }

        // 3. Paint committed strokes from offscreen surface
        cairo_set_source_surface(cr, canvas_surface, 0.0, 0.0);
        cairo_paint(cr);

        // 4. If we are actively moving/resizing a selected item (which was excluded from canvas_surface),
        //    draw it dynamically now!
        if (canvas_skip_index >= 0 && canvas_skip_index < (int)strokes.size()) {
            strokes[canvas_skip_index].draw(cr);
            // If any erasers exist above this stroke in z-order, apply them on top or keep z-order accurate
        }

        // 5. Paint in-progress stroke while drawing
        if (is_drawing) {
            draw_current_stroke(cr);
        }

        // 6. Paint active typing text
        if (is_typing) {
            draw_typing_text(cr);
        }

        // 7. Paint selection bounding box & resize handles for selected shape
        if (selected_index >= 0 && selected_index < (int)strokes.size() && !passthrough_mode) {
            const auto& sel = strokes[selected_index];
            if (sel.visual_bounds(true).intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
                sel.draw_selection_overlay(cr);
            }
        }

        // 8. Eraser cursor circle
        if (current_tool == ToolType::Eraser && !passthrough_mode) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
            cairo_set_line_width(cr, 1.5);
            cairo_new_sub_path(cr);
            cairo_arc(cr, mouse_x, mouse_y, eraser_radius, 0.0, 2.0 * M_PI);
            cairo_stroke(cr);
        }

        // 9. Toolbar (cached to an offscreen surface for instant blitting)
        BBox tb = toolbar_bounds();
        tb.y2 += 36.0;
        if (tb.intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
            draw_toolbar_cached(cr, w);
            if (!hover_button.empty() && submenu_open.empty()) {
                draw_toolbar_tooltip(cr, w);
            }
        }

        // 10. Submenu
        if (!submenu_open.empty() && clip_rect.y < TOOLBAR_HEIGHT + 200) {
            if (submenu_open == "pen_options") {
                draw_pen_submenu(cr, w);
            } else if (submenu_open == "eraser_options") {
                draw_eraser_submenu(cr, w);
            } else if (submenu_open == "text_options") {
                draw_text_submenu(cr, w);
            }
        }

        return FALSE;
    }

    void draw_typing_text(cairo_t* cr) {
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, text_size);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, typing_text.c_str(), &ext);

        if (!text_bg_color.empty() && text_bg_color != "transparent" && !typing_text.empty()) {
            RGBA bg = hex_to_rgba(text_bg_color);
            cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
            double pad = 8.0;
            cairo_rectangle(cr,
                            typing_x + ext.x_bearing - pad,
                            typing_y + ext.y_bearing - pad,
                            ext.width + pad * 2.0,
                            ext.height + pad * 2.0);
            cairo_fill(cr);
        }

        RGBA fg = hex_to_rgba(text_color);
        cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
        cairo_move_to(cr, typing_x, typing_y);
        if (!typing_text.empty()) {
            cairo_show_text(cr, typing_text.c_str());
        }

        double cursor_x = typing_x + ext.x_advance;
        double cursor_y = typing_y;
        gint64 now_us = g_get_monotonic_time();
        if (((now_us / 500000) % 2) == 0) {
            cairo_move_to(cr, cursor_x, cursor_y);
            cairo_line_to(cr, cursor_x, cursor_y - text_size * 0.8);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);
        }
    }

    void draw_current_stroke(cairo_t* cr) {
        if (current_tool == ToolType::Pen && current_points.size() >= 2) {
            DrawItem item;
            item.kind = StrokeKind::Freehand;
            item.points = current_points;
            item.color = current_color;
            item.width = stroke_width;
            item.is_eraser = false;
            item.draw(cr);
        } else if (current_tool == ToolType::Eraser && current_points.size() >= 2) {
            // Visual eraser trail preview
            cairo_save(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.25);
            cairo_set_line_width(cr, eraser_radius * 2.0);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_move_to(cr, current_points[0].x, current_points[0].y);
            for (size_t i = 1; i < current_points.size(); ++i) {
                cairo_line_to(cr, current_points[i].x, current_points[i].y);
            }
            cairo_stroke(cr);
            cairo_restore(cr);
        } else if ((current_tool == ToolType::Line || current_tool == ToolType::Rect ||
                    current_tool == ToolType::Circle || current_tool == ToolType::Arrow)) {
            DrawItem item;
            item.kind = StrokeKind::Shape;
            item.shape_type = current_tool;
            item.x1 = shape_start.x;
            item.y1 = shape_start.y;
            item.x2 = shape_end.x;
            item.y2 = shape_end.y;
            item.color = current_color;
            item.width = stroke_width;
            item.draw(cr);
        }
    }

    // ── Toolbar Rendering (Offscreen Cached) ──────────────────────────────

    std::string tool_to_name(ToolType t) const {
        switch (t) {
            case ToolType::Select: return "select";
            case ToolType::Pen:    return "pen";
            case ToolType::Eraser: return "eraser";
            case ToolType::Text:   return "text";
            case ToolType::Line:   return "line";
            case ToolType::Rect:   return "rect";
            case ToolType::Circle: return "circle";
            case ToolType::Arrow:  return "arrow";
        }
        return "pen";
    }

    void draw_toolbar_cached(cairo_t* cr, int width) {
        if (!toolbar_cache_surface ||
            cairo_image_surface_get_width(toolbar_cache_surface) != width ||
            toolbar_cache_dirty) {
            if (toolbar_cache_surface && cairo_image_surface_get_width(toolbar_cache_surface) != width) {
                cairo_surface_destroy(toolbar_cache_surface);
                toolbar_cache_surface = nullptr;
            }
            if (!toolbar_cache_surface) {
                toolbar_cache_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, (int)TOOLBAR_HEIGHT + 16);
            }
            cairo_t* tcr = cairo_create(toolbar_cache_surface);
            cairo_set_operator(tcr, CAIRO_OPERATOR_CLEAR);
            cairo_paint(tcr);
            cairo_set_operator(tcr, CAIRO_OPERATOR_OVER);
            draw_toolbar_direct(tcr);
            cairo_destroy(tcr);
            toolbar_cache_dirty = false;
        }
        cairo_set_source_surface(cr, toolbar_cache_surface, 0.0, 0.0);
        cairo_paint(cr);
    }

    void draw_toolbar_tooltip(cairo_t* cr, int width) {
        const ToolbarButton* btn = find_button(hover_button);
        if (!btn || btn->tip.empty()) return;

        cairo_save(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, btn->tip.c_str(), &ext);

        double pad_x = 10.0;
        double pad_y = 5.0;
        double tw = ext.width + pad_x * 2.0;
        double th = 24.0;
        double tx = std::clamp(btn->x + btn->w * 0.5 - tw * 0.5, 8.0, width - tw - 8.0);
        double ty = TOOLBAR_HEIGHT + 6.0;

        rounded_rect(cr, tx, ty, tw, th, 6.0);
        cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.92);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
        cairo_move_to(cr, tx + pad_x - ext.x_bearing, ty + (th - ext.height) * 0.5 - ext.y_bearing - pad_y * 0.1);
        cairo_show_text(cr, btn->tip.c_str());
        cairo_restore(cr);
    }

    void draw_toolbar_direct(cairo_t* cr) {
        const ToolbarButton* first = nullptr;
        const ToolbarButton* last = nullptr;
        for (const auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) != 0) {
                if (!first) first = &b;
                last = &b;
            }
        }
        if (!first || !last) return;

        double bar_x = first->x - 10.0;
        double bar_w = (last->x + last->w) - first->x + 20.0;
        double bar_y = 0.0;
        double bar_h = TOOLBAR_HEIGHT;

        // Soft Shadow
        cairo_save(cr);
        for (int i = 0; i < 6; ++i) {
            rounded_rect(cr, bar_x - i, bar_y - i, bar_w + 2 * i, bar_h + 2 * i, TOOLBAR_RADIUS + i);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.03);
            cairo_fill(cr);
        }
        cairo_restore(cr);

        // Background Gradient
        rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, TOOLBAR_RADIUS);
        cairo_pattern_t* grad = cairo_pattern_create_linear(bar_x, bar_y, bar_x, bar_y + bar_h);
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.16, 0.16, 0.18, 0.95);
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.10, 0.10, 0.12, 0.95);
        cairo_set_source(cr, grad);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        // Border
        rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, TOOLBAR_RADIUS);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        std::string active_tool_name = tool_to_name(current_tool);

        for (const auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) == 0) {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
                cairo_rectangle(cr, b.x, b.y, b.w, b.h);
                cairo_fill(cr);
                continue;
            }

            bool is_active = false;
            if (b.name == "cursor") {
                is_active = passthrough_mode;
            } else {
                is_active = (!passthrough_mode && b.name == active_tool_name);
            }
            bool is_hover = (hover_button == b.name);

            std::string act_color = (b.name == "text") ? text_color : current_color;

            if (is_active) {
                rounded_rect(cr, b.x + 2, b.y + 2, b.w - 4, b.h - 4, 8.0);
                RGBA c = hex_to_rgba(act_color, 0.25);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.25);
                cairo_fill(cr);
            } else if (is_hover) {
                rounded_rect(cr, b.x + 2, b.y + 2, b.w - 4, b.h - 4, 8.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
                cairo_fill(cr);
            }

            draw_button_icon(cr, b.name, b.x, b.y, b.w, b.h, is_active);

            if (is_active) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, b.x + b.w * 0.5, b.y + b.h - 2.0, 2.5, 0.0, 2.0 * M_PI);
                RGBA c = hex_to_rgba(act_color, 1.0);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
                cairo_fill(cr);
            }
        }
    }

    void draw_button_icon(cairo_t* cr, const std::string& name, double x, double y, double w, double h, bool active) {
        double alpha = active ? 0.95 : 0.65;
        cairo_set_line_width(cr, 2.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        double cx = x + w * 0.5;
        double cy = y + h * 0.5;

        if (name == "select") {
            // 4-way move / selection cross icon
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            double r = 9.5;
            cairo_move_to(cr, cx - r, cy);
            cairo_line_to(cr, cx + r, cy);
            cairo_move_to(cr, cx, cy - r);
            cairo_line_to(cr, cx, cy + r);
            cairo_stroke(cr);

            // Small arrowheads at 4 ends
            double a = 3.2;
            // Left
            cairo_move_to(cr, cx - r + a, cy - a);
            cairo_line_to(cr, cx - r, cy);
            cairo_line_to(cr, cx - r + a, cy + a);
            // Right
            cairo_move_to(cr, cx + r - a, cy - a);
            cairo_line_to(cr, cx + r, cy);
            cairo_line_to(cr, cx + r - a, cy + a);
            // Top
            cairo_move_to(cr, cx - a, cy - r + a);
            cairo_line_to(cr, cx, cy - r);
            cairo_line_to(cr, cx + a, cy - r + a);
            // Bottom
            cairo_move_to(cr, cx - a, cy + r - a);
            cairo_line_to(cr, cx, cy + r);
            cairo_line_to(cr, cx + a, cy + r - a);
            cairo_stroke(cr);
        } else if (name == "text") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 20.0);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, "T", &ext);
            cairo_move_to(cr, cx - ext.width * 0.5 - ext.x_bearing, cy - ext.height * 0.5 - ext.y_bearing);
            cairo_show_text(cr, "T");
        } else if (name == "pen") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, -M_PI / 4.0);
            cairo_rectangle(cr, -3.0, -12.0, 6.0, 18.0);
            cairo_stroke(cr);
            cairo_move_to(cr, -3.0, 6.0);
            cairo_line_to(cr, 0.0, 11.0);
            cairo_line_to(cr, 3.0, 6.0);
            cairo_stroke(cr);
            cairo_restore(cr);

            RGBA c = hex_to_rgba(current_color);
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + w - 8.0, y + h - 8.0, 5.0, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + w - 8.0, y + h - 8.0, 5.0, 0.0, 2.0 * M_PI);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        } else if (name == "eraser") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_rotate(cr, -M_PI / 6.0);
            rounded_rect(cr, -12.0, -6.0, 24.0, 12.0, 3.0);
            cairo_stroke(cr);
            cairo_move_to(cr, -4.0, -6.0);
            cairo_line_to(cr, -4.0, 6.0);
            cairo_stroke(cr);
            cairo_restore(cr);
        } else if (name == "line") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_move_to(cr, x + 10.0, y + h - 10.0);
            cairo_line_to(cr, x + w - 10.0, y + 10.0);
            cairo_stroke(cr);
        } else if (name == "rect") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            rounded_rect(cr, x + 9.0, y + 11.0, w - 18.0, h - 22.0, 2.0);
            cairo_stroke(cr);
        } else if (name == "circle") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx, cy, std::min(w, h) * 0.5 - 9.0, 0.0, 2.0 * M_PI);
            cairo_stroke(cr);
        } else if (name == "arrow") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            double sx = x + 10.0, sy = y + h - 10.0;
            double ex = x + w - 10.0, ey = y + 10.0;
            cairo_move_to(cr, sx, sy);
            cairo_line_to(cr, ex, ey);
            cairo_stroke(cr);
            double angle = std::atan2(ey - sy, ex - sx);
            double hl = 10.0;
            for (int sign : {-1, 1}) {
                cairo_move_to(cr, ex, ey);
                cairo_line_to(cr,
                              ex - hl * std::cos(angle + sign * M_PI / 5.0),
                              ey - hl * std::sin(angle + sign * M_PI / 5.0));
                cairo_stroke(cr);
            }
        } else if (name == "undo") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
            cairo_new_sub_path(cr);
            cairo_arc(cr, cx + 2.0, cy, 8.0, M_PI * 0.7, M_PI * 2.3);
            cairo_stroke(cr);
            double px = cx + 2.0 + 8.0 * std::cos(M_PI * 0.7);
            double py = cy + 8.0 * std::sin(M_PI * 0.7);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px - 5.0, py - 1.0);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px + 1.0, py - 6.0);
            cairo_stroke(cr);
        } else if (name == "redo") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
            cairo_new_sub_path(cr);
            cairo_arc_negative(cr, cx - 2.0, cy, 8.0, M_PI * 0.3, -M_PI * 1.3);
            cairo_stroke(cr);
            double px = cx - 2.0 + 8.0 * std::cos(M_PI * 0.3);
            double py = cy + 8.0 * std::sin(M_PI * 0.3);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px + 5.0, py - 1.0);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px - 1.0, py - 6.0);
            cairo_stroke(cr);
        } else if (name == "clear") {
            cairo_set_source_rgba(cr, 1.0, 0.35, 0.35, 0.80);
            cairo_set_line_width(cr, 1.8);
            double top = cy - 9.0;
            double lid_y = cy - 5.0;
            double bot = cy + 8.0;
            rounded_rect(cr, cx - 7.0, lid_y, 14.0, bot - lid_y, 2.0);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - 9.0, lid_y);
            cairo_line_to(cr, cx + 9.0, lid_y);
            cairo_stroke(cr);
            cairo_move_to(cr, cx - 3.0, lid_y);
            cairo_line_to(cr, cx - 2.0, top);
            cairo_line_to(cr, cx + 2.0, top);
            cairo_line_to(cr, cx + 3.0, lid_y);
            cairo_stroke(cr);
            for (int dx : {-3, 0, 3}) {
                cairo_move_to(cr, cx + dx, lid_y + 3.0);
                cairo_line_to(cr, cx + dx, bot - 3.0);
                cairo_stroke(cr);
            }
        } else if (name == "cursor") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
            double px = cx - 5.0, py = cy - 9.0;
            cairo_new_sub_path(cr);
            cairo_move_to(cr, px, py);
            cairo_line_to(cr, px, py + 16.0);
            cairo_line_to(cr, px + 5.0, py + 12.0);
            cairo_line_to(cr, px + 8.0, py + 17.0);
            cairo_line_to(cr, px + 11.0, py + 15.0);
            cairo_line_to(cr, px + 7.0, py + 10.0);
            cairo_line_to(cr, px + 12.0, py + 8.0);
            cairo_close_path(cr);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.4);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        } else if (name == "close") {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
            cairo_set_line_width(cr, 2.5);
            double d = 7.0;
            cairo_move_to(cr, cx - d, cy - d);
            cairo_line_to(cr, cx + d, cy + d);
            cairo_move_to(cr, cx + d, cy - d);
            cairo_line_to(cr, cx - d, cy + d);
            cairo_stroke(cr);
        }
    }

    // ── Submenus ──────────────────────────────────────────────────────────

    const ToolbarButton* find_button(const std::string& name) const {
        for (const auto& b : toolbar_buttons) {
            if (b.name == name) return &b;
        }
        return nullptr;
    }

    void draw_pen_submenu(cairo_t* cr, int width) {
        const ToolbarButton* pen_btn = find_button(tool_to_name(current_tool));
        if (!pen_btn) pen_btn = find_button("pen");
        if (!pen_btn) return;

        double color_row_w = NUM_COLORS * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP;
        double stroke_row_w = NUM_STROKES * (SM_STROKE_PILL_W + 4.0) - 4.0;
        double sm_content_w = std::max(color_row_w, stroke_row_w);
        double sm_w = sm_content_w + 32.0;
        double sm_h = 108.0;
        double sm_x = std::clamp(pen_btn->x + pen_btn->w * 0.5 - sm_w * 0.5, 8.0, width - sm_w - 8.0);
        double sm_y = TOOLBAR_HEIGHT + 10.0;

        for (int i = 0; i < 5; ++i) {
            rounded_rect(cr, sm_x - i, sm_y - i, sm_w + 2 * i, sm_h + 2 * i, 14.0 + i);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.04);
            cairo_fill(cr);
        }

        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_pattern_t* grad = cairo_pattern_create_linear(sm_x, sm_y, sm_x, sm_y + sm_h);
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.18, 0.18, 0.20, 0.96);
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.12, 0.12, 0.14, 0.96);
        cairo_set_source(cr, grad);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        double tri_cx = pen_btn->x + pen_btn->w * 0.5;
        double tri_cy = sm_y;
        cairo_move_to(cr, tri_cx - 8.0, tri_cy);
        cairo_line_to(cr, tri_cx, tri_cy - 6.0);
        cairo_line_to(cr, tri_cx + 8.0, tri_cy);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.96);
        cairo_fill(cr);

        submenu_items.clear();
        double pad_x = sm_x + 16.0;
        double cur_y = sm_y + 14.0;

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "COLOR");
        cur_y += 8.0;

        for (int i = 0; i < NUM_COLORS; ++i) {
            std::string hex_c = COLOR_PRESETS[i].hex;
            double sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP);
            double sy = cur_y;
            RGBA c = hex_to_rgba(hex_c);

            if (hex_c == current_color) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5 + 3.0, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            }

            cairo_new_sub_path(cr);
            cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                      SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
            cairo_fill(cr);

            if (hex_c == "#FFFFFF" || hex_c == "#FFCC00") {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            submenu_items.push_back({"color", hex_c, 0, sx, sy, SM_COLOR_SWATCH, SM_COLOR_SWATCH});
        }

        cur_y += SM_COLOR_SWATCH + 14.0;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "STROKE");
        cur_y += 8.0;

        for (int i = 0; i < NUM_STROKES; ++i) {
            int sw = STROKE_PRESETS[i];
            double sx = pad_x + i * (SM_STROKE_PILL_W + 4.0);
            double sy = cur_y;
            bool is_sel = ((int)stroke_width == sw);

            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
            if (is_sel) {
                RGBA c = hex_to_rgba(current_color, 0.3);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.3);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
            }
            cairo_fill(cr);

            if (is_sel) {
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            double dot_r = std::max(sw / 2.5, 1.5);
            cairo_new_sub_path(cr);
            cairo_arc(cr, sx + SM_STROKE_PILL_W * 0.5, sy + SM_STROKE_PILL_H * 0.5, dot_r, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            cairo_fill(cr);

            submenu_items.push_back({"pen_stroke", "", sw, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H});
        }
    }

    void draw_eraser_submenu(cairo_t* cr, int width) {
        const ToolbarButton* eraser_btn = find_button("eraser");
        if (!eraser_btn) return;

        double sm_w = NUM_ERASER_RADII * (SM_STROKE_PILL_W + 4.0) + 28.0;
        double sm_h = 70.0;
        double sm_x = std::clamp(eraser_btn->x + eraser_btn->w * 0.5 - sm_w * 0.5, 10.0, width - sm_w - 10.0);
        double sm_y = TOOLBAR_HEIGHT + 8.0;

        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.96);
        cairo_fill(cr);
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        double tri_cx = eraser_btn->x + eraser_btn->w * 0.5;
        double tri_cy = sm_y;
        cairo_move_to(cr, tri_cx - 8.0, tri_cy);
        cairo_line_to(cr, tri_cx, tri_cy - 6.0);
        cairo_line_to(cr, tri_cx + 8.0, tri_cy);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.96);
        cairo_fill(cr);

        submenu_items.clear();
        double pad_x = sm_x + 16.0;
        double cur_y = sm_y + 14.0;

        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "RADIUS");
        cur_y += 8.0;

        for (int i = 0; i < NUM_ERASER_RADII; ++i) {
            int rad = ERASER_RADIUS_PRESETS[i];
            double sx = pad_x + i * (SM_STROKE_PILL_W + 4.0);
            double sy = cur_y;
            bool is_sel = ((int)eraser_radius == rad);

            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, is_sel ? 0.30 : 0.06);
            cairo_fill(cr);

            if (is_sel) {
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            double dot_r = std::max(rad / 3.5, 1.5);
            cairo_new_sub_path(cr);
            cairo_arc(cr, sx + SM_STROKE_PILL_W * 0.5, sy + SM_STROKE_PILL_H * 0.5, dot_r, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            cairo_fill(cr);

            submenu_items.push_back({"eraser_radius", "", rad, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H});
        }
    }

    void draw_text_submenu(cairo_t* cr, int width) {
        const ToolbarButton* text_btn = find_button("text");
        if (!text_btn) return;

        int n_bg_colors = NUM_COLORS + 1;
        double color_row_w = NUM_COLORS * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP;
        double bg_row_w = n_bg_colors * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP;
        double size_row_w = NUM_TEXT_SIZES * (SM_STROKE_PILL_W + 4.0) - 4.0;

        double sm_content_w = std::max({color_row_w, bg_row_w, size_row_w});
        double sm_w = sm_content_w + 32.0;
        double sm_h = 160.0;
        double sm_x = std::clamp(text_btn->x + text_btn->w * 0.5 - sm_w * 0.5, 10.0, width - sm_w - 10.0);
        double sm_y = TOOLBAR_HEIGHT + 8.0;

        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_pattern_t* grad = cairo_pattern_create_linear(sm_x, sm_y, sm_x, sm_y + sm_h);
        cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.18, 0.18, 0.20, 0.96);
        cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.12, 0.12, 0.14, 0.96);
        cairo_set_source(cr, grad);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);

        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);

        double tri_cx = text_btn->x + text_btn->w * 0.5;
        double tri_cy = sm_y;
        cairo_move_to(cr, tri_cx - 8.0, tri_cy);
        cairo_line_to(cr, tri_cx, tri_cy - 6.0);
        cairo_line_to(cr, tri_cx + 8.0, tri_cy);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.18, 0.18, 0.20, 0.96);
        cairo_fill(cr);

        submenu_items.clear();
        double pad_x = sm_x + 16.0;
        double cur_y = sm_y + 14.0;

        // 1. TEXT COLOR
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "TEXT COLOR");
        cur_y += 8.0;

        for (int i = 0; i < NUM_COLORS; ++i) {
            std::string hex_c = COLOR_PRESETS[i].hex;
            double sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP);
            double sy = cur_y;
            RGBA c = hex_to_rgba(hex_c);

            if (hex_c == text_color) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5 + 3.0, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            }

            cairo_new_sub_path(cr);
            cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                      SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
            cairo_fill(cr);

            if (hex_c == "#FFFFFF" || hex_c == "#FFCC00") {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            submenu_items.push_back({"text_color", hex_c, 0, sx, sy, SM_COLOR_SWATCH, SM_COLOR_SWATCH});
        }

        cur_y += SM_COLOR_SWATCH + 14.0;

        // 2. BACKGROUND
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "BACKGROUND");
        cur_y += 8.0;

        for (int i = 0; i <= NUM_COLORS; ++i) {
            std::string hex_c = (i == 0) ? "transparent" : COLOR_PRESETS[i - 1].hex;
            double sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP);
            double sy = cur_y;

            if (hex_c == text_bg_color) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5 + 3.0, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
                cairo_set_line_width(cr, 2.0);
                cairo_stroke(cr);
            }

            if (hex_c == "transparent") {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 1.0);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1.0, 0.3, 0.3, 1.0);
                cairo_set_line_width(cr, 2.0);
                cairo_move_to(cr, sx + 4.0, sy + 4.0);
                cairo_line_to(cr, sx + SM_COLOR_SWATCH - 4.0, sy + SM_COLOR_SWATCH - 4.0);
                cairo_stroke(cr);
            } else {
                RGBA c = hex_to_rgba(hex_c);
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                          SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, 1.0);
                cairo_fill(cr);
                if (hex_c == "#FFFFFF" || hex_c == "#FFCC00") {
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, sx + SM_COLOR_SWATCH * 0.5, sy + SM_COLOR_SWATCH * 0.5,
                              SM_COLOR_SWATCH * 0.5, 0.0, 2.0 * M_PI);
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
                    cairo_set_line_width(cr, 1.0);
                    cairo_stroke(cr);
                }
            }

            submenu_items.push_back({"text_bg_color", hex_c, 0, sx, sy, SM_COLOR_SWATCH, SM_COLOR_SWATCH});
        }

        cur_y += SM_COLOR_SWATCH + 14.0;

        // 3. SIZE
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
        cairo_set_font_size(cr, 10.0);
        cairo_move_to(cr, pad_x, cur_y);
        cairo_show_text(cr, "SIZE");
        cur_y += 8.0;

        for (int i = 0; i < NUM_TEXT_SIZES; ++i) {
            int sz = TEXT_SIZE_PRESETS[i];
            double sx = pad_x + i * (SM_STROKE_PILL_W + 4.0);
            double sy = cur_y;
            bool is_sel = ((int)text_size == sz);

            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
            if (is_sel) {
                RGBA c = hex_to_rgba(current_color, 0.3);
                cairo_set_source_rgba(cr, c.r, c.g, c.b, 0.3);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
            }
            cairo_fill(cr);

            if (is_sel) {
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6.0);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            }

            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 12.0);
            std::string label = std::to_string(sz);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, label.c_str(), &ext);
            cairo_move_to(cr,
                          sx + SM_STROKE_PILL_W * 0.5 - ext.width * 0.5 - ext.x_bearing,
                          sy + SM_STROKE_PILL_H * 0.5 - ext.height * 0.5 - ext.y_bearing);
            cairo_show_text(cr, label.c_str());

            submenu_items.push_back({"text_size", "", sz, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H});
        }
    }

    // ── Hit Testing Helpers ───────────────────────────────────────────────

    const ToolbarButton* hit_toolbar(double x, double y) const {
        for (const auto& b : toolbar_buttons) {
            if (b.name.rfind("__sep", 0) == 0) continue;
            if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
                return &b;
            }
        }
        return nullptr;
    }

    const SubmenuItem* hit_submenu(double x, double y) const {
        for (const auto& item : submenu_items) {
            if (x >= item.x && x <= item.x + item.w && y >= item.y && y <= item.y + item.h) {
                return &item;
            }
        }
        return nullptr;
    }

    bool in_toolbar_zone(double y) const {
        return y <= TOOLBAR_HEIGHT;
    }

    bool in_submenu_zone(double /*x*/, double y) const {
        if (submenu_open.empty()) return false;
        if (submenu_open == "eraser_options") return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 90.0;
        if (submenu_open == "text_options")   return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 180.0;
        return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 130.0;
    }

    int find_topmost_selectable_at(double x, double y) const {
        for (int i = (int)strokes.size() - 1; i >= 0; --i) {
            if (strokes[i].hit_test(x, y, false)) {
                return i;
            }
        }
        // Second pass allowing bounding box interior hit if in Select tool
        if (current_tool == ToolType::Select) {
            for (int i = (int)strokes.size() - 1; i >= 0; --i) {
                if (strokes[i].hit_test(x, y, true)) {
                    return i;
                }
            }
        }
        return -1;
    }

    // ── Event Handlers ────────────────────────────────────────────────────

    void connect_events() {
        g_signal_connect(window, "draw", G_CALLBACK(on_draw_cb), this);
        g_signal_connect(window, "button-press-event", G_CALLBACK(on_button_press_cb), this);
        g_signal_connect(window, "button-release-event", G_CALLBACK(on_button_release_cb), this);
        g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion_cb), this);
        g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press_cb), this);
        g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_cb), this);
        g_signal_connect(window, "focus-out-event", G_CALLBACK(on_focus_out_cb), this);
    }

    static gboolean on_configure_cb(GtkWidget* /*w*/, GdkEventConfigure* ev, gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        self->ensure_canvas(ev->width, ev->height);
        return FALSE;
    }

    static gboolean on_button_press_cb(GtkWidget* w, GdkEventButton* ev, gpointer data) {
        return static_cast<ScreenDrawWindow*>(data)->on_button_press(w, ev);
    }

    gboolean on_button_press(GtkWidget* /*widget*/, GdkEventButton* ev) {
        if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return FALSE;
        double x = ev->x;
        double y = ev->y;

        // 1. Toolbar click
        if (in_toolbar_zone(y)) {
            gtk_window_present_with_time(GTK_WINDOW(window), ev->time);
            const ToolbarButton* btn = hit_toolbar(x, y);
            if (!btn && passthrough_mode) {
                exit_passthrough();
            }
            if (btn) {
                if (is_typing) commit_text();
                if (ev->time - last_toolbar_click_time < 150) return TRUE;
                last_toolbar_click_time = ev->time;
                handle_toolbar_click(btn->name);
            }
            return TRUE;
        }

        // 2. Submenu click
        if (in_submenu_zone(x, y)) {
            gtk_window_present_with_time(GTK_WINDOW(window), ev->time);
            const SubmenuItem* item = hit_submenu(x, y);
            if (item) {
                handle_submenu_click(*item);
            }
            return TRUE;
        }

        // 3. Close open submenu if clicking outside
        if (!submenu_open.empty()) {
            submenu_open.clear();
            submenu_items.clear();
            if (passthrough_mode) update_input_shape();
            invalidate_full();
            return TRUE;
        }

        // 4. Commit active text if clicking elsewhere
        if (is_typing) {
            commit_text();
            if (current_tool != ToolType::Text) return TRUE;
        }

        // 5. Check if user clicked a resize handle or body of the currently selected shape!
        //    This works both in Select tool AND right after drawing a shape!
        if (selected_index >= 0 && selected_index < (int)strokes.size()) {
            const auto& sel = strokes[selected_index];
            HandleId hid = sel.hit_handle(x, y);
            if (hid != HandleId::None) {
                start_manipulation(selected_index, hid, x, y);
                return TRUE;
            }
            if (sel.hit_test(x, y, true)) {
                start_manipulation(selected_index, HandleId::Body, x, y);
                return TRUE;
            }
        }

        // 6. If in Select tool, check if user clicked any drawn shape to select & move it
        if (current_tool == ToolType::Select) {
            int hit_idx = find_topmost_selectable_at(x, y);
            if (hit_idx != selected_index) {
                int old_sel = selected_index;
                selected_index = hit_idx;
                if (old_sel >= 0 && old_sel < (int)strokes.size()) {
                    invalidate_bbox(strokes[old_sel].visual_bounds(true));
                }
                if (selected_index >= 0) {
                    invalidate_bbox(strokes[selected_index].visual_bounds(true));
                }
            }
            if (selected_index >= 0) {
                HandleId hid = strokes[selected_index].hit_handle(x, y);
                start_manipulation(selected_index, (hid != HandleId::None) ? hid : HandleId::Body, x, y);
            }
            update_cursor(x, y);
            return TRUE;
        }

        // Deselect any previously selected shape when starting a new stroke
        if (selected_index >= 0) {
            BBox old_b = strokes[selected_index].visual_bounds(true);
            selected_index = -1;
            invalidate_bbox(old_b);
        }

        // 7. Text tool
        if (current_tool == ToolType::Text) {
            // Also allow clicking an existing text or shape in Text mode? If user clicked empty space, start typing:
            is_typing = true;
            typing_x = x;
            typing_y = y;
            typing_text.clear();
            start_cursor_blink();
            invalidate_full();
            return TRUE;
        }

        // 8. Start drawing Pen, Eraser, or Shape
        is_drawing = true;
        if (current_tool == ToolType::Pen || current_tool == ToolType::Eraser) {
            current_points.clear();
            current_points.push_back({x, y});
        } else {
            shape_start = {x, y};
            shape_end = {x, y};
        }
        return TRUE;
    }

    void start_manipulation(int idx, HandleId hid, double x, double y) {
        selected_index = idx;
        is_manipulating = true;
        active_handle = hid;
        drag_start_x = x;
        drag_start_y = y;
        drag_orig_item = strokes[idx];
        drag_modified = false;
        // Rebuild static canvas once without the actively manipulated item so motion frames are ultra-fast
        rebuild_canvas(idx);
        invalidate_bbox(strokes[idx].visual_bounds(true));
        update_cursor(x, y);
    }

    static gboolean on_button_release_cb(GtkWidget* w, GdkEventButton* ev, gpointer data) {
        return static_cast<ScreenDrawWindow*>(data)->on_button_release(w, ev);
    }

    gboolean on_button_release(GtkWidget* /*widget*/, GdkEventButton* ev) {
        if (ev->button != 1) return FALSE;

        if (is_manipulating) {
            is_manipulating = false;
            active_handle = HandleId::None;
            if (drag_modified && selected_index >= 0 && selected_index < (int)strokes.size()) {
                // Push the pre-drag state to undo stack
                DrawItem final_item = strokes[selected_index];
                strokes[selected_index] = drag_orig_item;
                push_undo_snapshot();
                strokes[selected_index] = final_item;
            }
            rebuild_canvas(-1);
            if (selected_index >= 0 && selected_index < (int)strokes.size()) {
                invalidate_bbox(strokes[selected_index].visual_bounds(true));
            }
            update_cursor(ev->x, ev->y);
            return TRUE;
        }

        if (!is_drawing) return FALSE;
        is_drawing = false;

        if (current_tool == ToolType::Pen && current_points.size() >= 2) {
            DrawItem item;
            item.kind = StrokeKind::Freehand;
            item.points = current_points;
            item.color = current_color;
            item.width = stroke_width;
            item.is_eraser = false;
            commit_stroke(item, false);
        } else if (current_tool == ToolType::Eraser && current_points.size() >= 2) {
            DrawItem item;
            item.kind = StrokeKind::Freehand;
            item.points = current_points;
            item.color = "#000000";
            item.width = eraser_radius * 2.0;
            item.is_eraser = true;
            commit_stroke(item, false);
        } else if (current_tool == ToolType::Line || current_tool == ToolType::Rect ||
                   current_tool == ToolType::Circle || current_tool == ToolType::Arrow) {
            double dist = std::hypot(shape_end.x - shape_start.x, shape_end.y - shape_start.y);
            if (dist >= 3.0) {
                DrawItem item;
                item.kind = StrokeKind::Shape;
                item.shape_type = current_tool;
                item.x1 = shape_start.x;
                item.y1 = shape_start.y;
                item.x2 = shape_end.x;
                item.y2 = shape_end.y;
                item.color = current_color;
                item.width = stroke_width;
                // Automatically select newly drawn shape so user can immediately move or resize it!
                commit_stroke(item, true);
            } else {
                // Click without drag on a shape tool: check if user clicked an existing shape to select it!
                int hit_idx = find_topmost_selectable_at(ev->x, ev->y);
                if (hit_idx >= 0) {
                    selected_index = hit_idx;
                    invalidate_bbox(strokes[selected_index].visual_bounds(true));
                }
            }
        }

        current_points.clear();
        update_cursor(ev->x, ev->y);
        return TRUE;
    }

    void commit_stroke(const DrawItem& item, bool select_after) {
        push_undo_snapshot();
        strokes.push_back(item);
        if (canvas_surface) {
            cairo_t* cr = cairo_create(canvas_surface);
            item.draw(cr);
            cairo_destroy(cr);
        }
        if (select_after && item.is_selectable()) {
            selected_index = (int)strokes.size() - 1;
        } else {
            selected_index = -1;
        }
        invalidate_bbox(item.visual_bounds(true));
    }

    void commit_text() {
        if (is_typing) {
            // Trim check
            bool has_non_space = false;
            for (char c : typing_text) {
                if (c != ' ' && c != '\t' && c != '\n') {
                    has_non_space = true;
                    break;
                }
            }
            if (has_non_space) {
                DrawItem item;
                item.kind = StrokeKind::Text;
                item.text = typing_text;
                item.text_x = typing_x;
                item.text_y = typing_y;
                item.color = text_color;
                item.font_size = text_size;
                item.bg_color = text_bg_color;
                item.update_text_metrics();
                commit_stroke(item, true);
            }
        }
        is_typing = false;
        typing_text.clear();
        stop_cursor_blink();
        invalidate_full();
    }

    // ── Motion Handling (Coalesced & Dirty-Rect Optimized) ────────────────

    static gboolean on_motion_cb(GtkWidget* w, GdkEventMotion* ev, gpointer data) {
        return static_cast<ScreenDrawWindow*>(data)->on_motion(w, ev);
    }

    gboolean on_motion(GtkWidget* /*widget*/, GdkEventMotion* ev) {
        double x = ev->x;
        double y = ev->y;
        double old_x = mouse_x;
        double old_y = mouse_y;
        mouse_x = x;
        mouse_y = y;

        // 1. Active Move / Resize of a selected shape
        if (is_manipulating && selected_index >= 0 && selected_index < (int)strokes.size()) {
            BBox before_box = strokes[selected_index].visual_bounds(true);
            double dx = x - drag_start_x;
            double dy = y - drag_start_y;
            if (std::hypot(dx, dy) > 0.5) {
                drag_modified = true;
            }
            if (active_handle == HandleId::Body) {
                strokes[selected_index].apply_move_from(drag_orig_item, dx, dy);
            } else {
                strokes[selected_index].apply_resize_from(drag_orig_item, active_handle, dx, dy);
            }
            BBox after_box = strokes[selected_index].visual_bounds(true);
            invalidate_bbox(before_box.united(after_box));
            return TRUE;
        }

        // 2. Active Drawing
        if (is_drawing) {
            if (current_tool == ToolType::Pen || current_tool == ToolType::Eraser) {
                if (!current_points.empty()) {
                    double lx = current_points.back().x;
                    double ly = current_points.back().y;
                    double dx = x - lx;
                    double dy = y - ly;
                    if (dx * dx + dy * dy < 3.0) return TRUE;
                }
                current_points.push_back({x, y});
                size_t n = current_points.size();
                if (current_tool == ToolType::Eraser && canvas_surface && n >= 2) {
                    const Point& p1 = current_points[n - 2];
                    const Point& p2 = current_points[n - 1];
                    cairo_t* ecr = cairo_create(canvas_surface);
                    cairo_set_operator(ecr, CAIRO_OPERATOR_CLEAR);
                    cairo_set_line_width(ecr, eraser_radius * 2.0);
                    cairo_set_line_cap(ecr, CAIRO_LINE_CAP_ROUND);
                    cairo_move_to(ecr, p1.x, p1.y);
                    cairo_line_to(ecr, p2.x, p2.y);
                    cairo_stroke(ecr);
                    cairo_destroy(ecr);
                }
                double pad = (current_tool == ToolType::Eraser ? eraser_radius * 2.0 : stroke_width) + 8.0;
                BBox dirty = {x, y, x, y};
                for (size_t i = (n > 4 ? n - 4 : 0); i < n; ++i) {
                    dirty.x1 = std::min(dirty.x1, current_points[i].x);
                    dirty.y1 = std::min(dirty.y1, current_points[i].y);
                    dirty.x2 = std::max(dirty.x2, current_points[i].x);
                    dirty.y2 = std::max(dirty.y2, current_points[i].y);
                }
                invalidate_bbox(dirty.padded(pad));
            } else {
                BBox prev_box = {std::min(shape_start.x, shape_end.x), std::min(shape_start.y, shape_end.y),
                                 std::max(shape_start.x, shape_end.x), std::max(shape_start.y, shape_end.y)};
                shape_end = {x, y};
                BBox new_box = {std::min(shape_start.x, shape_end.x), std::min(shape_start.y, shape_end.y),
                                std::max(shape_start.x, shape_end.x), std::max(shape_start.y, shape_end.y)};
                double pad = std::max(stroke_width * 4.0, 28.0);
                invalidate_bbox(prev_box.united(new_box).padded(pad));
            }
            return TRUE;
        }

        // 3. Hover detection on toolbar
        std::string old_hover = hover_button;
        const ToolbarButton* hit = in_toolbar_zone(y) ? hit_toolbar(x, y) : nullptr;
        hover_button = hit ? hit->name : "";
        if (hover_button != old_hover) {
            invalidate_toolbar();
        }

        // 4. Update cursor based on zone / handles / hovered shapes
        update_cursor(x, y);

        // 5. Eraser cursor circle redraw
        if (current_tool == ToolType::Eraser && !passthrough_mode) {
            double r = eraser_radius + 6.0;
            BBox b1 = {old_x - r, old_y - r, old_x + r, old_y + r};
            BBox b2 = {x - r, y - r, x + r, y + r};
            invalidate_bbox(b1.united(b2));
        }

        return TRUE;
    }

    // ── Cursor Management ─────────────────────────────────────────────────

    void set_cursor_by_name(const char* name) {
        if (current_cursor_name == name) return;
        current_cursor_name = name;
        GdkWindow* gdk_win = gtk_widget_get_window(window);
        if (!gdk_win) return;
        GdkDisplay* display = gtk_widget_get_display(window);
        GdkCursor* cur = gdk_cursor_new_from_name(display, name);
        if (!cur) {
            cur = gdk_cursor_new_for_display(display, GDK_CROSSHAIR);
        }
        gdk_window_set_cursor(gdk_win, cur);
        if (cur) g_object_unref(cur);
    }

    static const char* cursor_for_handle(HandleId hid) {
        switch (hid) {
            case HandleId::NW:
            case HandleId::SE: return "nwse-resize";
            case HandleId::NE:
            case HandleId::SW: return "nesw-resize";
            case HandleId::N:
            case HandleId::S:  return "ns-resize";
            case HandleId::E:
            case HandleId::W:  return "ew-resize";
            case HandleId::LineStart:
            case HandleId::LineEnd: return "crosshair";
            case HandleId::Body: return "move";
            default: return "default";
        }
    }

    void update_cursor(double x, double y) {
        if (in_toolbar_zone(y) || in_submenu_zone(x, y)) {
            set_cursor_by_name("pointer");
            return;
        }
        if (is_manipulating) {
            set_cursor_by_name(cursor_for_handle(active_handle));
            return;
        }
        if (selected_index >= 0 && selected_index < (int)strokes.size()) {
            HandleId hid = strokes[selected_index].hit_handle(x, y);
            if (hid != HandleId::None) {
                set_cursor_by_name(cursor_for_handle(hid));
                return;
            }
            if (strokes[selected_index].hit_test(x, y, true)) {
                set_cursor_by_name("move");
                return;
            }
        }
        if (current_tool == ToolType::Select) {
            int idx = find_topmost_selectable_at(x, y);
            set_cursor_by_name(idx >= 0 ? "move" : "default");
            return;
        }
        if (current_tool == ToolType::Eraser) {
            set_cursor_by_name("cell");
            return;
        }
        if (current_tool == ToolType::Text) {
            set_cursor_by_name("text");
            return;
        }
        set_cursor_by_name("crosshair");
    }

    // ── Text Cursor Blink Timer ───────────────────────────────────────────

    static gboolean on_blink_cb(gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        if (self->is_typing) {
            self->invalidate_full();
            return TRUE;
        }
        self->blink_timer_id = 0;
        return FALSE;
    }

    void start_cursor_blink() {
        stop_cursor_blink();
        blink_timer_id = g_timeout_add(500, on_blink_cb, this);
    }

    void stop_cursor_blink() {
        if (blink_timer_id != 0) {
            g_source_remove(blink_timer_id);
            blink_timer_id = 0;
        }
    }

    // ── Keyboard Shortcuts ────────────────────────────────────────────────

    static gboolean on_key_press_cb(GtkWidget* w, GdkEventKey* ev, gpointer data) {
        return static_cast<ScreenDrawWindow*>(data)->on_key_press(w, ev);
    }

    gboolean on_key_press(GtkWidget* /*widget*/, GdkEventKey* ev) {
        guint keyval = ev->keyval;
        bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
        const char* keyname = gdk_keyval_name(keyval);
        std::string kname = keyname ? keyname : "";

        if (is_typing) {
            if (ctrl) {
                if (kname == "z" || kname == "Z") {
                    commit_text();
                    undo();
                } else if (kname == "y" || kname == "Y") {
                    commit_text();
                    redo();
                }
                return TRUE;
            }
            if (kname == "Return" || kname == "KP_Enter" || kname == "Escape") {
                commit_text();
            } else if (kname == "BackSpace") {
                if (!typing_text.empty()) {
                    // Pop last UTF-8 character
                    while (!typing_text.empty() && (typing_text.back() & 0xC0) == 0x80) {
                        typing_text.pop_back();
                    }
                    if (!typing_text.empty()) typing_text.pop_back();
                    invalidate_full();
                }
            } else {
                guint32 uni = gdk_keyval_to_unicode(keyval);
                if (uni >= 32 && uni != 127) {
                    char buf[8];
                    int len = g_unichar_to_utf8(uni, buf);
                    typing_text.append(buf, len);
                    invalidate_full();
                }
            }
            return TRUE;
        }

        if (kname == "F9") {
            toggle_overlay();
        } else if (kname == "Escape") {
            if (!submenu_open.empty()) {
                submenu_open.clear();
                submenu_items.clear();
                invalidate_full();
            } else if (selected_index >= 0) {
                BBox b = strokes[selected_index].visual_bounds(true);
                selected_index = -1;
                invalidate_bbox(b);
            }
        } else if ((kname == "Delete" || kname == "BackSpace") && selected_index >= 0 && selected_index < (int)strokes.size()) {
            push_undo_snapshot();
            strokes.erase(strokes.begin() + selected_index);
            selected_index = -1;
            rebuild_canvas(-1);
            invalidate_full();
        } else if (ctrl && (kname == "z" || kname == "Z")) {
            undo();
        } else if (ctrl && (kname == "y" || kname == "Y")) {
            redo();
        } else if (ctrl && (kname == "x" || kname == "X")) {
            clear_canvas();
        } else if (!ctrl && (kname == "s" || kname == "S" || kname == "v" || kname == "V")) {
            select_tool(ToolType::Select);
        } else if (!ctrl && (kname == "p" || kname == "P")) {
            select_tool(ToolType::Pen);
        } else if (!ctrl && (kname == "e" || kname == "E")) {
            select_tool(ToolType::Eraser);
        } else if (!ctrl && (kname == "t" || kname == "T")) {
            select_tool(ToolType::Text);
        } else if (!ctrl && (kname == "l" || kname == "L")) {
            select_tool(ToolType::Line);
        } else if (!ctrl && (kname == "r" || kname == "R")) {
            select_tool(ToolType::Rect);
        } else if (!ctrl && (kname == "o" || kname == "O")) {
            select_tool(ToolType::Circle);
        } else if (!ctrl && (kname == "a" || kname == "A")) {
            select_tool(ToolType::Arrow);
        } else if (!ctrl && (kname == "c" || kname == "C")) {
            enter_passthrough();
        }

        return TRUE;
    }

    // ── Toolbar & Submenu Actions ─────────────────────────────────────────

    void handle_toolbar_click(const std::string& name) {
        toolbar_cache_dirty = true;

        if (name == "select") {
            select_tool(ToolType::Select);
        } else if (name == "pen") {
            if (passthrough_mode) exit_passthrough();
            if (current_tool != ToolType::Pen) {
                current_tool = ToolType::Pen;
                submenu_open.clear();
                submenu_items.clear();
            } else {
                submenu_open = (submenu_open == "pen_options") ? "" : "pen_options";
                submenu_items.clear();
            }
        } else if (name == "text") {
            if (passthrough_mode) exit_passthrough();
            if (current_tool != ToolType::Text) {
                current_tool = ToolType::Text;
                submenu_open.clear();
                submenu_items.clear();
            } else {
                submenu_open = (submenu_open == "text_options") ? "" : "text_options";
                submenu_items.clear();
            }
        } else if (name == "eraser") {
            if (passthrough_mode) exit_passthrough();
            if (current_tool != ToolType::Eraser) {
                current_tool = ToolType::Eraser;
                submenu_open.clear();
                submenu_items.clear();
            } else {
                submenu_open = (submenu_open == "eraser_options") ? "" : "eraser_options";
                submenu_items.clear();
            }
        } else if (name == "line") {
            select_tool(ToolType::Line);
        } else if (name == "rect") {
            select_tool(ToolType::Rect);
        } else if (name == "circle") {
            select_tool(ToolType::Circle);
        } else if (name == "arrow") {
            select_tool(ToolType::Arrow);
        } else if (name == "undo") {
            undo();
        } else if (name == "redo") {
            redo();
        } else if (name == "clear") {
            clear_canvas();
        } else if (name == "cursor") {
            if (passthrough_mode) exit_passthrough();
            else enter_passthrough();
        } else if (name == "close") {
            hide_overlay();
        }

        invalidate_full();
        if (passthrough_mode) {
            update_input_shape();
        }
    }

    void handle_submenu_click(const SubmenuItem& item) {
        toolbar_cache_dirty = true;
        if (item.type == "color") {
            current_color = item.str_val;
            if (selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind != StrokeKind::Text) {
                push_undo_snapshot();
                strokes[selected_index].color = current_color;
                rebuild_canvas(-1);
            }
        } else if (item.type == "pen_stroke") {
            stroke_width = item.int_val;
            if (selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind != StrokeKind::Text) {
                push_undo_snapshot();
                strokes[selected_index].width = stroke_width;
                rebuild_canvas(-1);
            }
        } else if (item.type == "eraser_radius") {
            eraser_radius = item.int_val;
        } else if (item.type == "text_color") {
            text_color = item.str_val;
            if (selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind == StrokeKind::Text) {
                push_undo_snapshot();
                strokes[selected_index].color = text_color;
                rebuild_canvas(-1);
            }
        } else if (item.type == "text_bg_color") {
            text_bg_color = item.str_val;
            if (selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind == StrokeKind::Text) {
                push_undo_snapshot();
                strokes[selected_index].bg_color = text_bg_color;
                strokes[selected_index].text_metrics_valid = false;
                rebuild_canvas(-1);
            }
        } else if (item.type == "text_size") {
            text_size = item.int_val;
            if (selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind == StrokeKind::Text) {
                push_undo_snapshot();
                strokes[selected_index].font_size = text_size;
                strokes[selected_index].text_metrics_valid = false;
                rebuild_canvas(-1);
            }
        }
        invalidate_full();
    }

    void select_tool(ToolType t) {
        if (is_typing) commit_text();
        current_tool = t;
        submenu_open.clear();
        submenu_items.clear();
        toolbar_cache_dirty = true;
        if (t == ToolType::Eraser || t == ToolType::Pen) {
            selected_index = -1;
        }
        if (passthrough_mode) {
            exit_passthrough();
        }
        update_cursor(mouse_x, mouse_y);
        invalidate_full();
    }

    void undo() {
        if (!undo_stack.empty()) {
            redo_stack.push_back(strokes);
            strokes = undo_stack.back();
            undo_stack.pop_back();
            selected_index = -1;
            rebuild_canvas(-1);
            invalidate_full();
        }
    }

    void redo() {
        if (!redo_stack.empty()) {
            undo_stack.push_back(strokes);
            strokes = redo_stack.back();
            redo_stack.pop_back();
            selected_index = -1;
            rebuild_canvas(-1);
            invalidate_full();
        }
    }

    void clear_canvas() {
        if (!strokes.empty()) {
            push_undo_snapshot();
            strokes.clear();
            selected_index = -1;
            rebuild_canvas(-1);
            invalidate_full();
        }
    }

    // ── Passthrough & Overlay Visibility ──────────────────────────────────

    void update_input_shape() {
        GdkWindow* gdk_win = gtk_widget_get_window(window);
        if (!gdk_win) return;

        if (!passthrough_mode) {
            cairo_rectangle_int_t full_rect = {0, 0, mon_width, mon_height};
            cairo_region_t* reg = cairo_region_create_rectangle(&full_rect);
            gdk_window_input_shape_combine_region(gdk_win, reg, 0, 0);
            cairo_region_destroy(reg);
            return;
        }

        cairo_region_t* reg = cairo_region_create();
        BBox tb = toolbar_bounds();
        cairo_rectangle_int_t tb_rect = {
            (int)std::floor(tb.x1), 0,
            (int)std::ceil(tb.width()), (int)TOOLBAR_HEIGHT + 8
        };
        cairo_region_union_rectangle(reg, &tb_rect);

        if (!submenu_open.empty()) {
            int sm_h = 130;
            if (submenu_open == "eraser_options") sm_h = 90;
            else if (submenu_open == "text_options") sm_h = 180;
            cairo_rectangle_int_t sm_rect = {0, (int)TOOLBAR_HEIGHT, mon_width, sm_h};
            cairo_region_union_rectangle(reg, &sm_rect);
        }

        gdk_window_input_shape_combine_region(gdk_win, reg, 0, 0);
        cairo_region_destroy(reg);
    }

    void toggle_overlay() {
        if (passthrough_mode) {
            exit_passthrough();
        } else if (is_visible) {
            hide_overlay();
        } else {
            show_overlay();
        }
    }

    void show_overlay() {
        is_visible = true;
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_widget_show_all(window);
        gtk_window_present(GTK_WINDOW(window));
        update_cursor(mouse_x, mouse_y);
        start_focus_keepalive();
    }

    void hide_overlay() {
        if (is_typing) commit_text();
        is_visible = false;
        passthrough_mode = false;
        submenu_open.clear();
        submenu_items.clear();
        toolbar_cache_dirty = true;
        stop_focus_keepalive();
        update_input_shape();
        gtk_widget_hide(window);
    }

    void enter_passthrough() {
        if (is_typing) commit_text();
        passthrough_mode = true;
        selected_index = -1;
        submenu_open.clear();
        submenu_items.clear();
        toolbar_cache_dirty = true;
        update_input_shape();
        invalidate_full();
    }

    static gboolean reclaim_focus_cb(gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        if (self->is_visible) {
            gtk_window_present(GTK_WINDOW(self->window));
            gtk_widget_grab_focus(self->window);
        }
        return FALSE;
    }

    void exit_passthrough() {
        passthrough_mode = false;
        toolbar_cache_dirty = true;
        update_input_shape();
        current_cursor_name.clear();
        update_cursor(mouse_x, mouse_y);
        invalidate_full();
        g_idle_add(reclaim_focus_cb, this);
    }

    static gboolean reassert_top_cb(gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        if (self->is_visible) {
            gtk_window_set_keep_above(GTK_WINDOW(self->window), TRUE);
            GdkWindow* gdk_win = gtk_widget_get_window(self->window);
            if (gdk_win) gdk_window_raise(gdk_win);
        }
        return FALSE;
    }

    static gboolean on_focus_out_cb(GtkWidget* /*w*/, GdkEventFocus* /*ev*/, gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        if (self->is_visible) {
            gtk_window_set_keep_above(GTK_WINDOW(self->window), TRUE);
            g_idle_add(reassert_top_cb, self);
        }
        return FALSE;
    }

    static gboolean focus_keepalive_cb(gpointer data) {
        auto* self = static_cast<ScreenDrawWindow*>(data);
        if (!self->is_visible) {
            self->focus_keepalive_id = 0;
            return FALSE;
        }
        gtk_window_set_keep_above(GTK_WINDOW(self->window), TRUE);
        return TRUE;
    }

    void start_focus_keepalive() {
        stop_focus_keepalive();
        focus_keepalive_id = g_timeout_add(500, focus_keepalive_cb, this);
    }

    void stop_focus_keepalive() {
        if (focus_keepalive_id != 0) {
            g_source_remove(focus_keepalive_id);
            focus_keepalive_id = 0;
        }
    }
};

// ─── GNOME Custom Keybinding Setup ───────────────────────────────────────────

static void run_cmd(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    gint exit_status = 0;
    g_spawn_sync(nullptr, argv.data(), nullptr,
                 (GSpawnFlags)(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                 nullptr, nullptr, nullptr, nullptr, &exit_status, nullptr);
}

static void setup_global_hotkey() {
    std::string toggle_cmd =
        std::string("gdbus call --session --dest ") + DBUS_NAME +
        " --object-path " + DBUS_PATH +
        " --method " + DBUS_IFACE + ".Toggle";

    gchar* stdout_str = nullptr;
    const gchar* get_argv[] = {"gsettings", "get", GS_SCHEMA, GS_KEY, nullptr};
    if (!g_spawn_sync(nullptr, const_cast<gchar**>(get_argv), nullptr,
                      G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                      &stdout_str, nullptr, nullptr, nullptr) || !stdout_str) {
        return;
    }

    std::string current(stdout_str);
    g_free(stdout_str);

    std::string new_path = std::string(KEYBINDING_PATH_PREFIX) + KEYBINDING_SLOT + "/";
    std::string schema_path = std::string(GS_CUSTOM_SCHEMA) + ":" + new_path;

    if (current.find(KEYBINDING_SLOT) != std::string::npos) {
        run_cmd({"gsettings", "set", schema_path, "command", toggle_cmd});
        std::printf("[\u2713] F9 hotkey already registered\n");
        return;
    }

    std::vector<std::string> paths;
    if (current.find("@as []") == std::string::npos && current.find("[]") == std::string::npos) {
        size_t pos = 0;
        while ((pos = current.find('\'', pos)) != std::string::npos) {
            size_t end = current.find('\'', pos + 1);
            if (end == std::string::npos) break;
            paths.push_back(current.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
    }
    paths.push_back(new_path);

    run_cmd({"gsettings", "set", schema_path, "name", "Screen Draw Toggle"});
    run_cmd({"gsettings", "set", schema_path, "command", toggle_cmd});
    run_cmd({"gsettings", "set", schema_path, "binding", "F9"});

    std::string paths_str = "[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i > 0) paths_str += ", ";
        paths_str += "'" + paths[i] + "'";
    }
    paths_str += "]";
    run_cmd({"gsettings", "set", GS_SCHEMA, GS_KEY, paths_str});
    std::printf("[\u2713] F9 hotkey registered via GNOME custom keybindings\n");
}

// ─── D-Bus Service ───────────────────────────────────────────────────────────

struct AppContext {
    GApplication* app = nullptr;
    std::unique_ptr<ScreenDrawWindow> win;
    GDBusNodeInfo* node_info = nullptr;
    guint owner_id = 0;
};

static void on_dbus_method_call(GDBusConnection* /*conn*/,
                                const gchar* /*sender*/,
                                const gchar* /*object_path*/,
                                const gchar* /*interface_name*/,
                                const gchar* method_name,
                                GVariant* /*parameters*/,
                                GDBusMethodInvocation* invocation,
                                gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    std::string m = method_name ? method_name : "";
    if (m == "Toggle" && ctx->win) {
        ctx->win->toggle_overlay();
    } else if (m == "Show" && ctx->win) {
        ctx->win->show_overlay();
    } else if (m == "Hide" && ctx->win) {
        ctx->win->hide_overlay();
    } else if (m == "Quit" && ctx->app) {
        g_application_quit(ctx->app);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static const GDBusInterfaceVTable dbus_vtable = {
    on_dbus_method_call,
    nullptr,
    nullptr,
    {nullptr}
};

static gboolean initial_hide_cb(gpointer data) {
    auto* win = static_cast<ScreenDrawWindow*>(data);
    win->hide_overlay();
    return FALSE;
}

static void on_app_activate(GApplication* app, gpointer user_data) {
    auto* ctx = static_cast<AppContext*>(user_data);
    if (!ctx->win) {
        g_application_hold(app);
        ctx->win = std::make_unique<ScreenDrawWindow>();
        g_signal_connect_swapped(ctx->win->window, "destroy", G_CALLBACK(g_application_release), app);
        gtk_widget_show_all(ctx->win->window);
        g_idle_add(initial_hide_cb, ctx->win.get());

        // Register D-Bus object
        GError* err = nullptr;
        ctx->node_info = g_dbus_node_info_new_for_xml(DBUS_XML, &err);
        GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
        if (bus && ctx->node_info) {
            g_dbus_connection_register_object(
                bus,
                DBUS_PATH,
                ctx->node_info->interfaces[0],
                &dbus_vtable,
                ctx,
                nullptr,
                nullptr);
            ctx->owner_id = g_bus_own_name_on_connection(
                bus,
                DBUS_NAME,
                G_BUS_NAME_OWNER_FLAGS_NONE,
                nullptr,
                nullptr,
                nullptr,
                nullptr);
            std::printf("[\u2713] D-Bus service registered\n");
        }
    } else {
        ctx->win->toggle_overlay();
    }
}

int main(int argc, char** argv) {
    // Force X11 backend (XWayland) so fullscreen transparent overlay stays always-on-top on GNOME Wayland
    setenv("GDK_BACKEND", "x11", 1);

    std::printf("┌──────────────────────────────────────────┐\n");
    std::printf("│  Screen Draw — Fast Native C++ Overlay   │\n");
    std::printf("├──────────────────────────────────────────┤\n");
    std::printf("│  F9       Toggle overlay                 │\n");
    std::printf("│  S / V    Select, Move & Resize shapes   │\n");
    std::printf("│  P        Pen tool                       │\n");
    std::printf("│  E        Eraser tool                    │\n");
    std::printf("│  T        Text tool                      │\n");
    std::printf("│  L/R/O/A  Line/Rect/Circle/Arrow         │\n");
    std::printf("│  Del      Delete selected shape          │\n");
    std::printf("│  Ctrl+Z   Undo                           │\n");
    std::printf("│  Ctrl+Y   Redo                           │\n");
    std::printf("│  Ctrl+X   Clear canvas                   │\n");
    std::printf("│  C        Interact mode                  │\n");
    std::printf("│  Esc      Deselect / Close submenu       │\n");
    std::printf("└──────────────────────────────────────────┘\n");

    std::signal(SIGINT, SIG_DFL);
    setup_global_hotkey();

    GtkApplication* app = gtk_application_new("com.tools.screendraw", G_APPLICATION_DEFAULT_FLAGS);
    AppContext ctx;
    ctx.app = G_APPLICATION(app);

    g_signal_connect(app, "activate", G_CALLBACK(on_app_activate), &ctx);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (ctx.owner_id) g_bus_unown_name(ctx.owner_id);
    if (ctx.node_info) g_dbus_node_info_unref(ctx.node_info);
    g_object_unref(app);
    return status;
}
