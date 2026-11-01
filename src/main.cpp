// Screen Draw — Native C++17 / Xlib / Cairo Screen Annotation Overlay
// Fast, hardware-composited, dirty-region optimized UI with movable & resizable shapes.
// No GTK / GLib dependency: talks to the X server (or XWayland) directly via Xlib.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <cairo.h>

// Xlib's `None` macro collides with HandleId::None below.
static constexpr unsigned long XNone = None;
#undef None

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <clocale>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ─── IPC Constants ───────────────────────────────────────────────────────────
// A running instance listens on a per-user Unix socket; launching the binary
// again (optionally with --toggle/--show/--hide/--quit) forwards the command.

static std::string ipc_socket_path() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) return std::string(rt) + "/screen-draw.sock";
    return "/tmp/screen-draw-" + std::to_string((unsigned long)getuid()) + ".sock";
}

static int64_t monotonic_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

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
    // X11 connection & window
    Display* dpy = nullptr;
    int screen_num = 0;
    Window root = 0;
    Window window = 0;
    Visual* visual = nullptr;
    int depth = 0;
    Colormap colormap = 0;
    GC gc = nullptr;
    XIM xim = nullptr;
    XIC xic = nullptr;
    int win_x = 0;
    int win_y = 0;
    int mon_width = 1920;
    int mon_height = 1080;
    Time last_event_time = CurrentTime;
    std::map<unsigned int, Cursor> cursor_cache;

    // Back buffer (shared memory XImage when available, plain XImage otherwise)
    XImage* back_image = nullptr;
    XShmSegmentInfo shm_info{};
    bool back_uses_shm = false;
    cairo_surface_t* back_surface = nullptr;
    int back_w = 0;
    int back_h = 0;
    cairo_region_t* dirty_region = nullptr;

    // Atoms
    Atom a_wm_protocols = 0, a_wm_delete = 0, a_net_wm_name = 0, a_utf8 = 0;
    Atom a_net_wm_state = 0, a_net_wm_state_above = 0, a_net_wm_state_skip_taskbar = 0;
    Atom a_net_wm_state_skip_pager = 0, a_net_wm_window_type = 0, a_net_wm_window_type_utility = 0;
    Atom a_net_active_window = 0, a_motif_wm_hints = 0, a_net_wm_pid = 0;

    // Deferred (idle) actions processed after the current event batch
    bool pending_reclaim_focus = false;
    bool pending_reassert_top = false;

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
    bool has_focus = false;

    // Text typing state
    bool is_typing = false;
    double typing_x = 0.0;
    double typing_y = 0.0;
    std::string typing_text;
    size_t typing_cursor_pos = 0;
    bool is_typing_drag = false;
    int editing_index = -1; // index of an existing Text item being re-edited (-1 = new text)
    Time last_click_time = 0;
    double last_click_x = -1000.0;
    double last_click_y = -1000.0;
    int64_t blink_next_us = 0; // 0 = blink timer inactive
    int64_t blink_base_us = 0;
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
    Time last_toolbar_click_time = 0;
    bool passthrough_mode = false;

    // Offscreen Cairo surfaces for blazing fast rendering
    cairo_surface_t* canvas_surface = nullptr;
    int canvas_skip_index = -1; // When actively moving/resizing an item, exclude it from static canvas_surface
    cairo_surface_t* toolbar_cache_surface = nullptr;
    bool toolbar_cache_dirty = true;
    int cached_toolbar_width = -1;

    // Timers & Cursors
    int64_t focus_keepalive_next_us = 0; // 0 = keepalive timer inactive
    std::string current_cursor_name;
    bool has_shape_ext = false;
    bool quit_requested = false;

    explicit ScreenDrawWindow(Display* display) : dpy(display) {
        setup_window();
        build_toolbar();
    }

    ~ScreenDrawWindow() {
        stop_focus_keepalive();
        stop_cursor_blink();
        if (canvas_surface) cairo_surface_destroy(canvas_surface);
        if (toolbar_cache_surface) cairo_surface_destroy(toolbar_cache_surface);
        destroy_back_buffer();
        if (dirty_region) cairo_region_destroy(dirty_region);
        if (xic) XDestroyIC(xic);
        if (xim) XCloseIM(xim);
        for (auto& kv : cursor_cache) XFreeCursor(dpy, kv.second);
        if (gc) XFreeGC(dpy, gc);
        if (window) XDestroyWindow(dpy, window);
        if (colormap) XFreeColormap(dpy, colormap);
    }

    void push_undo_snapshot() {
        undo_stack.push_back(strokes);
        if (undo_stack.size() > 100) {
            undo_stack.erase(undo_stack.begin());
        }
        redo_stack.clear();
    }

    void setup_window() {
        screen_num = DefaultScreen(dpy);
        root = RootWindow(dpy, screen_num);

        auto atom = [&](const char* n) { return XInternAtom(dpy, n, False); };
        a_wm_protocols = atom("WM_PROTOCOLS");
        a_wm_delete = atom("WM_DELETE_WINDOW");
        a_net_wm_name = atom("_NET_WM_NAME");
        a_utf8 = atom("UTF8_STRING");
        a_net_wm_state = atom("_NET_WM_STATE");
        a_net_wm_state_above = atom("_NET_WM_STATE_ABOVE");
        a_net_wm_state_skip_taskbar = atom("_NET_WM_STATE_SKIP_TASKBAR");
        a_net_wm_state_skip_pager = atom("_NET_WM_STATE_SKIP_PAGER");
        a_net_wm_window_type = atom("_NET_WM_WINDOW_TYPE");
        a_net_wm_window_type_utility = atom("_NET_WM_WINDOW_TYPE_UTILITY");
        a_net_active_window = atom("_NET_ACTIVE_WINDOW");
        a_motif_wm_hints = atom("_MOTIF_WM_HINTS");
        a_net_wm_pid = atom("_NET_WM_PID");

        // 32-bit ARGB visual for a translucent overlay (requires a compositor)
        XVisualInfo vinfo;
        if (XMatchVisualInfo(dpy, screen_num, 32, TrueColor, &vinfo)) {
            visual = vinfo.visual;
            depth = vinfo.depth;
        } else {
            visual = DefaultVisual(dpy, screen_num);
            depth = DefaultDepth(dpy, screen_num);
            std::fprintf(stderr, "[!] No 32-bit ARGB visual available; overlay will not be transparent\n");
        }
        colormap = XCreateColormap(dpy, root, visual, AllocNone);

        // Geometry of the primary monitor (RandR 1.5), falling back to the whole screen
        win_x = 0;
        win_y = 0;
        mon_width = DisplayWidth(dpy, screen_num);
        mon_height = DisplayHeight(dpy, screen_num);
        int rr_event = 0, rr_error = 0, rr_major = 0, rr_minor = 0;
        if (XRRQueryExtension(dpy, &rr_event, &rr_error) &&
            XRRQueryVersion(dpy, &rr_major, &rr_minor) &&
            (rr_major > 1 || (rr_major == 1 && rr_minor >= 5))) {
            int n = 0;
            XRRMonitorInfo* mons = XRRGetMonitors(dpy, root, True, &n);
            if (mons && n > 0) {
                int pick = 0;
                for (int i = 0; i < n; ++i) {
                    if (mons[i].primary) { pick = i; break; }
                }
                win_x = mons[pick].x;
                win_y = mons[pick].y;
                mon_width = mons[pick].width;
                mon_height = mons[pick].height;
            }
            if (mons) XRRFreeMonitors(mons);
        }

        XSetWindowAttributes attrs{};
        attrs.colormap = colormap;
        attrs.border_pixel = 0;
        attrs.background_pixel = 0; // fully transparent on an ARGB visual
        attrs.event_mask = ExposureMask | StructureNotifyMask |
                           ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                           KeyPressMask | FocusChangeMask;
        window = XCreateWindow(dpy, root, win_x, win_y, mon_width, mon_height, 0,
                               depth, InputOutput, visual,
                               CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
        gc = XCreateGC(dpy, window, 0, nullptr);

        // Title & class
        const char* title = "Screen Draw";
        XStoreName(dpy, window, title);
        XChangeProperty(dpy, window, a_net_wm_name, a_utf8, 8, PropModeReplace,
                        (const unsigned char*)title, (int)std::strlen(title));
        XClassHint class_hint;
        class_hint.res_name = const_cast<char*>("screen-draw");
        class_hint.res_class = const_cast<char*>("Screen-draw");
        XSetClassHint(dpy, window, &class_hint);

        // Fixed position & size
        XSizeHints* size_hints = XAllocSizeHints();
        size_hints->flags = USPosition | USSize | PMinSize | PMaxSize;
        size_hints->x = win_x;
        size_hints->y = win_y;
        size_hints->width = size_hints->min_width = size_hints->max_width = mon_width;
        size_hints->height = size_hints->min_height = size_hints->max_height = mon_height;
        XSetWMNormalHints(dpy, window, size_hints);
        XFree(size_hints);

        // Accept keyboard focus
        XWMHints* wm_hints = XAllocWMHints();
        wm_hints->flags = InputHint | StateHint;
        wm_hints->input = True;
        wm_hints->initial_state = NormalState;
        XSetWMHints(dpy, window, wm_hints);
        XFree(wm_hints);

        XSetWMProtocols(dpy, window, &a_wm_delete, 1);

        // Undecorated utility window
        long motif_hints[5] = {2 /* MWM_HINTS_DECORATIONS */, 0, 0, 0, 0};
        XChangeProperty(dpy, window, a_motif_wm_hints, a_motif_wm_hints, 32, PropModeReplace,
                        (unsigned char*)motif_hints, 5);
        XChangeProperty(dpy, window, a_net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&a_net_wm_window_type_utility, 1);
        long pid = (long)getpid();
        XChangeProperty(dpy, window, a_net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char*)&pid, 1);

        int shape_event = 0, shape_error = 0;
        has_shape_ext = XShapeQueryExtension(dpy, &shape_event, &shape_error);

        setup_input_method();
        ensure_canvas(mon_width, mon_height);
    }

    void setup_input_method() {
        // XIM gives us proper UTF-8 text input (layouts, dead keys, compose)
        if (XSupportsLocale()) {
            xim = XOpenIM(dpy, nullptr, nullptr, nullptr);
            if (!xim) {
                XSetLocaleModifiers("@im=none");
                xim = XOpenIM(dpy, nullptr, nullptr, nullptr);
            }
        }
        if (!xim) return;
        xic = XCreateIC(xim,
                        XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, window,
                        XNFocusWindow, window,
                        nullptr);
        if (!xic) return;
        long filter_events = 0;
        if (!XGetICValues(xic, XNFilterEvents, &filter_events, nullptr)) {
            XWindowAttributes wa;
            XGetWindowAttributes(dpy, window, &wa);
            XSelectInput(dpy, window, wa.your_event_mask | filter_events);
        }
    }

    // ── X11 Window Management Helpers ─────────────────────────────────────

    void send_wm_message(Atom type, long l0, long l1, long l2, long l3) {
        XEvent e{};
        e.xclient.type = ClientMessage;
        e.xclient.window = window;
        e.xclient.message_type = type;
        e.xclient.format = 32;
        e.xclient.data.l[0] = l0;
        e.xclient.data.l[1] = l1;
        e.xclient.data.l[2] = l2;
        e.xclient.data.l[3] = l3;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    }

    void set_keep_above() {
        if (!is_visible) return;
        send_wm_message(a_net_wm_state, 1 /* _NET_WM_STATE_ADD */, (long)a_net_wm_state_above, 0, 1);
    }

    void present_window(Time t = CurrentTime) {
        if (!is_visible) return;
        XRaiseWindow(dpy, window);
        // Source indication 2 (pager) so the WM honours the request without focus-stealing checks
        send_wm_message(a_net_active_window, 2, (long)t, 0, 0);
    }

    void map_window() {
        // _NET_WM_STATE must be set before mapping (WMs drop it on withdraw)
        Atom states[3] = {a_net_wm_state_above, a_net_wm_state_skip_taskbar, a_net_wm_state_skip_pager};
        XChangeProperty(dpy, window, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)states, 3);
        XMoveResizeWindow(dpy, window, win_x, win_y, mon_width, mon_height);
        XMapRaised(dpy, window);
    }

    // ── Back Buffer & Presentation ────────────────────────────────────────

    static bool& x_error_trap_flag() {
        static bool flag = false;
        return flag;
    }

    static int trap_x_error(Display*, XErrorEvent*) {
        x_error_trap_flag() = true;
        return 0;
    }

    void destroy_back_buffer() {
        if (back_surface) {
            cairo_surface_destroy(back_surface);
            back_surface = nullptr;
        }
        if (back_image) {
            if (back_uses_shm) {
                XShmDetach(dpy, &shm_info);
                XSync(dpy, False);
                back_image->data = nullptr;
                XDestroyImage(back_image);
                shmdt(shm_info.shmaddr);
            } else {
                XDestroyImage(back_image); // frees malloc'd pixel data
            }
            back_image = nullptr;
        }
        back_uses_shm = false;
        back_w = back_h = 0;
    }

    bool create_shm_back_buffer(int w, int h) {
        if (!XShmQueryExtension(dpy)) return false;
        XImage* img = XShmCreateImage(dpy, visual, depth, ZPixmap, nullptr, &shm_info, w, h);
        if (!img) return false;
        if (img->bits_per_pixel != 32) {
            XDestroyImage(img);
            return false;
        }
        shm_info.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height, IPC_CREAT | 0600);
        if (shm_info.shmid < 0) {
            XDestroyImage(img);
            return false;
        }
        shm_info.shmaddr = (char*)shmat(shm_info.shmid, nullptr, 0);
        if (shm_info.shmaddr == (char*)-1) {
            shmctl(shm_info.shmid, IPC_RMID, nullptr);
            XDestroyImage(img);
            return false;
        }
        img->data = shm_info.shmaddr;
        shm_info.readOnly = False;

        XSync(dpy, False);
        x_error_trap_flag() = false;
        auto old_handler = XSetErrorHandler(trap_x_error);
        XShmAttach(dpy, &shm_info);
        XSync(dpy, False);
        XSetErrorHandler(old_handler);
        shmctl(shm_info.shmid, IPC_RMID, nullptr); // freed once both sides detach

        if (x_error_trap_flag()) {
            img->data = nullptr;
            XDestroyImage(img);
            shmdt(shm_info.shmaddr);
            return false;
        }
        back_image = img;
        back_uses_shm = true;
        return true;
    }

    void ensure_back_buffer(int w, int h) {
        if (back_surface && back_w == w && back_h == h) return;
        destroy_back_buffer();
        if (w <= 0 || h <= 0) return;

        if (!create_shm_back_buffer(w, h)) {
            int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
            char* data = (char*)std::calloc((size_t)stride * h, 1);
            back_image = XCreateImage(dpy, visual, depth, ZPixmap, 0, data, w, h, 32, stride);
            if (!back_image) {
                std::free(data);
                return;
            }
        }
        // Cairo's ARGB32 is native-endian; tell Xlib so it can byte-swap for the server if needed
        static const uint32_t endian_probe = 1;
        back_image->byte_order = (*(const uint8_t*)&endian_probe == 1) ? LSBFirst : MSBFirst;

        back_surface = cairo_image_surface_create_for_data(
            (unsigned char*)back_image->data, CAIRO_FORMAT_ARGB32, w, h, back_image->bytes_per_line);
        back_w = w;
        back_h = h;
    }

    void add_dirty_rect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        if (!dirty_region) dirty_region = cairo_region_create();
        cairo_rectangle_int_t r = {x, y, w, h};
        cairo_region_union_rectangle(dirty_region, &r);
    }

    // Repaint the accumulated dirty region into the back buffer and push it to the window
    void present_dirty() {
        if (!dirty_region || cairo_region_is_empty(dirty_region)) return;
        if (!is_visible) {
            cairo_region_destroy(dirty_region);
            dirty_region = nullptr;
            return;
        }

        int w = mon_width;
        int h = mon_height;
        ensure_back_buffer(w, h);
        if (!back_surface) return;

        cairo_rectangle_int_t bounds = {0, 0, w, h};
        cairo_region_intersect_rectangle(dirty_region, &bounds);
        if (cairo_region_num_rectangles(dirty_region) > 24) {
            // Too fragmented: repaint the bounding box in one go
            cairo_rectangle_int_t ext;
            cairo_region_get_extents(dirty_region, &ext);
            cairo_region_destroy(dirty_region);
            dirty_region = cairo_region_create_rectangle(&ext);
        }

        cairo_rectangle_int_t clip_rect;
        cairo_region_get_extents(dirty_region, &clip_rect);
        int n = cairo_region_num_rectangles(dirty_region);

        cairo_t* cr = cairo_create(back_surface);
        for (int i = 0; i < n; ++i) {
            cairo_rectangle_int_t r;
            cairo_region_get_rectangle(dirty_region, i, &r);
            cairo_rectangle(cr, r.x, r.y, r.width, r.height);
        }
        cairo_clip(cr);
        on_draw(cr, w, h, clip_rect);
        cairo_destroy(cr);
        cairo_surface_flush(back_surface);

        for (int i = 0; i < n; ++i) {
            cairo_rectangle_int_t r;
            cairo_region_get_rectangle(dirty_region, i, &r);
            if (back_uses_shm) {
                XShmPutImage(dpy, window, gc, back_image, r.x, r.y, r.x, r.y, r.width, r.height, False);
            } else {
                XPutImage(dpy, window, gc, back_image, r.x, r.y, r.x, r.y, r.width, r.height);
            }
        }
        // The server must finish reading shared memory before we draw into it again
        if (back_uses_shm) XSync(dpy, False);

        cairo_region_destroy(dirty_region);
        dirty_region = nullptr;
    }

    // ── Event Dispatch, Idle Work & Timers ────────────────────────────────

    void handle_event(XEvent& ev) {
        if (XFilterEvent(&ev, XNone)) return; // consumed by the input method

        switch (ev.type) {
            case MapNotify:
                if (ev.xmap.window == window && is_visible && !passthrough_mode) {
                    pending_reclaim_focus = true;
                }
                break;
            case Expose:
                add_dirty_rect(ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height);
                if (ev.xexpose.count == 0 && is_visible && !passthrough_mode && !has_focus) {
                    pending_reclaim_focus = true;
                }
                break;
            case ConfigureNotify:
                if (ev.xconfigure.window == window &&
                    (ev.xconfigure.width != mon_width || ev.xconfigure.height != mon_height)) {
                    mon_width = ev.xconfigure.width;
                    mon_height = ev.xconfigure.height;
                    ensure_canvas(mon_width, mon_height);
                    if (passthrough_mode) update_input_shape();
                    invalidate_full();
                }
                break;
            case ButtonPress:
                last_event_time = ev.xbutton.time;
                if (ev.xbutton.button == Button1) {
                    on_button_press(ev.xbutton.x, ev.xbutton.y, ev.xbutton.time);
                }
                break;
            case ButtonRelease:
                last_event_time = ev.xbutton.time;
                if (ev.xbutton.button == Button1) {
                    on_button_release(ev.xbutton.x, ev.xbutton.y);
                }
                break;
            case MotionNotify:
                last_event_time = ev.xmotion.time;
                on_motion(ev.xmotion.x, ev.xmotion.y);
                break;
            case KeyPress:
                last_event_time = ev.xkey.time;
                on_key_press(ev.xkey);
                break;
            case FocusIn:
                if (ev.xfocus.detail != NotifyPointer) {
                    has_focus = true;
                }
                if (xic) XSetICFocus(xic);
                break;
            case FocusOut:
                if (xic) XUnsetICFocus(xic);
                if (ev.xfocus.detail != NotifyInferior && ev.xfocus.detail != NotifyPointer) {
                    has_focus = false;
                    on_focus_out();
                }
                break;
            case ClientMessage:
                if (ev.xclient.message_type == a_wm_protocols &&
                    (Atom)ev.xclient.data.l[0] == a_wm_delete) {
                    quit_requested = true;
                }
                break;
            default:
                break;
        }
    }

    void run_idle() {
        if (pending_reclaim_focus) {
            pending_reclaim_focus = false;
            reclaim_focus();
        }
        if (pending_reassert_top) {
            pending_reassert_top = false;
            reassert_top();
        }
    }

    void run_timers() {
        int64_t now = monotonic_us();
        if (blink_next_us && now >= blink_next_us) {
            if (is_typing) {
                invalidate_full();
                blink_next_us = now + 500000;
            } else {
                blink_next_us = 0;
            }
        }
        if (focus_keepalive_next_us && now >= focus_keepalive_next_us) {
            if (is_visible) {
                set_keep_above();
                if (!passthrough_mode && !has_focus) {
                    present_window(CurrentTime);
                }
                focus_keepalive_next_us = now + (has_focus ? 500000 : 100000);
            } else {
                focus_keepalive_next_us = 0;
            }
        }
    }

    // Milliseconds until the next timer fires, or -1 if no timer is active
    int next_timeout_ms() const {
        int64_t next = 0;
        if (blink_next_us) next = blink_next_us;
        if (focus_keepalive_next_us && (!next || focus_keepalive_next_us < next)) next = focus_keepalive_next_us;
        if (!next) return -1;
        int64_t diff = next - monotonic_us();
        if (diff <= 0) return 0;
        return (int)((diff + 999) / 1000);
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
        // Keep a text item that is being edited out of the static canvas (it is drawn as live typing)
        if (skip_idx < 0 && is_typing && editing_index >= 0) skip_idx = editing_index;
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
            add_dirty_rect(x, y, r - x, bot - y);
        }
    }

    void invalidate_toolbar() {
        toolbar_cache_dirty = true;
        BBox tb = toolbar_bounds();
        tb.y2 += 36.0; // Include hover tooltip region below toolbar
        invalidate_bbox(tb);
    }

    void invalidate_full() {
        if (window) add_dirty_rect(0, 0, mon_width, mon_height);
    }

    // ── Main Rendering ────────────────────────────────────────────────────

    void on_draw(cairo_t* cr, int w, int h, const cairo_rectangle_int_t& clip_rect) {
        ensure_canvas(w, h);
        layout_toolbar(w);

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
        if (canvas_skip_index >= 0 && canvas_skip_index < (int)strokes.size() &&
            !(is_typing && canvas_skip_index == editing_index)) {
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

        double cursor_x = typing_x;
        if (typing_cursor_pos > 0) {
            cairo_text_extents_t cur_ext;
            size_t safe_pos = std::min(typing_cursor_pos, typing_text.size());
            std::string sub = typing_text.substr(0, safe_pos);
            cairo_text_extents(cr, sub.c_str(), &cur_ext);
            cursor_x += cur_ext.x_advance;
        }
        double cursor_y = typing_y;
        int64_t now_us = monotonic_us();
        int64_t elapsed_us = (blink_base_us > 0) ? (now_us - blink_base_us) : now_us;
        if (((elapsed_us / 500000) % 2) == 0) {
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

    int find_topmost_text_at(double x, double y) const {
        for (int i = (int)strokes.size() - 1; i >= 0; --i) {
            if (strokes[i].kind == StrokeKind::Text && strokes[i].hit_test(x, y, true)) {
                return i;
            }
        }
        return -1;
    }

    size_t cursor_index_from_x(const std::string& str, double font_sz, double start_x, double target_x) const {
        if (str.empty()) return 0;

        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr = cairo_create(tmp);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_sz);

        std::vector<size_t> boundaries;
        boundaries.push_back(0);
        for (size_t i = 0; i < str.size(); ) {
            unsigned char c = (unsigned char)str[i];
            size_t len = 1;
            if ((c & 0x80) == 0) len = 1;
            else if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            if (i + len > str.size()) len = str.size() - i;
            i += len;
            boundaries.push_back(i);
        }

        std::vector<double> positions;
        positions.reserve(boundaries.size());
        for (size_t b : boundaries) {
            if (b == 0) {
                positions.push_back(start_x);
            } else {
                cairo_text_extents_t ext;
                cairo_text_extents(cr, str.substr(0, b).c_str(), &ext);
                positions.push_back(start_x + ext.x_advance);
            }
        }

        cairo_destroy(cr);
        cairo_surface_destroy(tmp);

        if (target_x <= positions.front()) return 0;
        if (target_x >= positions.back()) return str.size();

        for (size_t i = 0; i + 1 < positions.size(); ++i) {
            double p0 = positions[i];
            double p1 = positions[i + 1];
            double mid = (p0 + p1) * 0.5;
            if (target_x < mid) {
                return boundaries[i];
            }
        }
        return str.size();
    }

    BBox typing_visual_bounds() const {
        if (!is_typing) return {0, 0, 0, 0};
        cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* cr = cairo_create(tmp);
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, text_size);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, typing_text.c_str(), &ext);
        cairo_destroy(cr);
        cairo_surface_destroy(tmp);

        double w = std::max({ext.width, ext.x_advance, 14.0});
        double h = std::max(ext.height, text_size * 0.75);
        double xb = std::min(ext.x_bearing, 0.0);
        double yb = (ext.height > 0) ? ext.y_bearing : -text_size * 0.75;
        double pad = (text_bg_color != "transparent" && !text_bg_color.empty()) ? 8.0 : 4.0;
        return {typing_x + xb - pad, typing_y + yb - pad,
                typing_x + xb + w + pad, typing_y + yb + h + pad};
    }

    // ── Text Re-Editing ───────────────────────────────────────────────────

    // Re-open an existing Text item for editing
    void begin_edit_text(int idx, double click_x = -1.0) {
        if (idx < 0 || idx >= (int)strokes.size() || strokes[idx].kind != StrokeKind::Text) return;
        const DrawItem& item = strokes[idx];
        BBox old_b = item.visual_bounds(true);
        if (selected_index >= 0 && selected_index < (int)strokes.size()) {
            invalidate_bbox(strokes[selected_index].visual_bounds(true));
        }

        is_typing = true;
        is_typing_drag = false;
        editing_index = idx;
        typing_x = item.text_x;
        typing_y = item.text_y;
        typing_text = item.text;
        if (click_x >= 0) {
            typing_cursor_pos = cursor_index_from_x(typing_text, item.font_size, typing_x, click_x);
        } else {
            typing_cursor_pos = typing_text.size();
        }
        // Adopt the item's style so the text submenu edits it while typing
        text_color = item.color;
        text_size = item.font_size;
        text_bg_color = item.bg_color.empty() ? "transparent" : item.bg_color;
        selected_index = -1;
        is_manipulating = false;
        active_handle = HandleId::None;
        toolbar_cache_dirty = true;

        rebuild_canvas(idx); // hide the original while it is live-edited
        start_cursor_blink();
        invalidate_bbox(old_b);
        invalidate_full();
    }

    // Commit any current typing, then edit the topmost text item under (x, y)
    bool begin_edit_text_at(double x, double y) {
        if (is_typing) commit_text(); // may remove/reindex items, so hit-test afterwards
        int idx = find_topmost_text_at(x, y);
        if (idx < 0) return false;
        begin_edit_text(idx, x);
        update_cursor(x, y);
        return true;
    }

    // ── Event Handlers ────────────────────────────────────────────────────

    bool on_button_press(double x, double y, Time time) {
        if (!has_focus && !passthrough_mode) {
            present_window(time);
        }

        // 1. Toolbar click
        if (in_toolbar_zone(y)) {
            present_window(time);
            const ToolbarButton* btn = hit_toolbar(x, y);
            if (!btn && passthrough_mode) {
                exit_passthrough();
            }
            if (btn) {
                if (is_typing) commit_text();
                if (time - last_toolbar_click_time < 150) return true;
                last_toolbar_click_time = time;
                handle_toolbar_click(btn->name);
            }
            return true;
        }

        // 2. Submenu click
        if (in_submenu_zone(x, y)) {
            present_window(time);
            const SubmenuItem* item = hit_submenu(x, y);
            if (item) {
                handle_submenu_click(*item);
            }
            return true;
        }

        // 3. Close open submenu if clicking outside
        if (!submenu_open.empty()) {
            submenu_open.clear();
            submenu_items.clear();
            if (passthrough_mode) update_input_shape();
            invalidate_full();
            return true;
        }

        // 4. Double-click on a text item (any tool) re-opens it for editing
        bool is_double_click = (time - last_click_time) < 400 &&
                               std::hypot(x - last_click_x, y - last_click_y) < 6.0;
        last_click_time = time;
        last_click_x = x;
        last_click_y = y;
        if (is_double_click && begin_edit_text_at(x, y)) {
            last_click_time = 0; // don't chain into a triple-click
            return true;
        }

        // 5. If typing, clicking inside active text positions the cursor; clicking outside commits
        if (is_typing) {
            if (typing_visual_bounds().padded(8.0).contains(x, y)) {
                typing_cursor_pos = cursor_index_from_x(typing_text, text_size, typing_x, x);
                is_typing_drag = true;
                start_cursor_blink();
                invalidate_full();
                return true;
            }
            commit_text();
            if (current_tool != ToolType::Text) return true;
        }

        // 6. Check if user clicked a resize handle or body of the currently selected shape!
        //    This works both in Select tool AND right after drawing a shape!
        if (selected_index >= 0 && selected_index < (int)strokes.size()) {
            const auto& sel = strokes[selected_index];
            HandleId hid = sel.hit_handle(x, y);
            if (hid != HandleId::None) {
                start_manipulation(selected_index, hid, x, y);
                return true;
            }
            if (sel.hit_test(x, y, true)) {
                if (current_tool == ToolType::Text && sel.kind == StrokeKind::Text) {
                    begin_edit_text(selected_index, x);
                    return true;
                }
                start_manipulation(selected_index, HandleId::Body, x, y);
                return true;
            }
        }

        // 7. If in Select tool, check if user clicked any drawn shape to select & move it
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
            return true;
        }

        // Deselect any previously selected shape when starting a new stroke
        if (selected_index >= 0) {
            BBox old_b = strokes[selected_index].visual_bounds(true);
            selected_index = -1;
            invalidate_bbox(old_b);
        }

        // 8. Text tool: click an existing text to edit it, or empty space to start new text
        if (current_tool == ToolType::Text) {
            if (begin_edit_text_at(x, y)) return true;
            is_typing = true;
            is_typing_drag = false;
            editing_index = -1;
            typing_x = x;
            typing_y = y;
            typing_text.clear();
            typing_cursor_pos = 0;
            start_cursor_blink();
            invalidate_full();
            return true;
        }

        // 9. Start drawing Pen, Eraser, or Shape
        is_drawing = true;
        if (current_tool == ToolType::Pen || current_tool == ToolType::Eraser) {
            current_points.clear();
            current_points.push_back({x, y});
        } else {
            shape_start = {x, y};
            shape_end = {x, y};
        }
        return true;
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

    bool on_button_release(double ev_x, double ev_y) {
        if (is_typing_drag) {
            is_typing_drag = false;
        }

        if (is_manipulating) {
            HandleId released_handle = active_handle;
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
            // Text tool: a plain click (no drag) on a selected text's body edits it
            if (!drag_modified && released_handle == HandleId::Body && current_tool == ToolType::Text &&
                selected_index >= 0 && selected_index < (int)strokes.size() &&
                strokes[selected_index].kind == StrokeKind::Text) {
                begin_edit_text(selected_index, ev_x);
            }
            update_cursor(ev_x, ev_y);
            return true;
        }

        if (!is_drawing) return false;
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
                int hit_idx = find_topmost_selectable_at(ev_x, ev_y);
                if (hit_idx >= 0) {
                    selected_index = hit_idx;
                    invalidate_bbox(strokes[selected_index].visual_bounds(true));
                }
            }
        }

        current_points.clear();
        update_cursor(ev_x, ev_y);
        return true;
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
            if (editing_index >= 0 && editing_index < (int)strokes.size()) {
                // Re-edit of an existing text item: update it in place (or delete it if emptied)
                int idx = editing_index;
                editing_index = -1;
                is_typing = false;
                is_typing_drag = false;
                typing_cursor_pos = 0;
                DrawItem& item = strokes[idx];
                BBox old_b = item.visual_bounds(true);
                if (!has_non_space) {
                    push_undo_snapshot();
                    strokes.erase(strokes.begin() + idx);
                    selected_index = -1;
                } else {
                    bool changed = item.text != typing_text || item.color != text_color ||
                                   item.font_size != text_size || item.bg_color != text_bg_color;
                    if (changed) {
                        push_undo_snapshot(); // snapshot holds the pre-edit item
                        item.text = typing_text;
                        item.color = text_color;
                        item.font_size = text_size;
                        item.bg_color = text_bg_color;
                        item.text_metrics_valid = false;
                        item.update_text_metrics();
                    }
                    selected_index = idx;
                }
                invalidate_bbox(old_b);
                typing_text.clear();
                typing_cursor_pos = 0;
                stop_cursor_blink();
                rebuild_canvas(-1);
                invalidate_full();
                return;
            }
            editing_index = -1;
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
        is_typing_drag = false;
        typing_text.clear();
        typing_cursor_pos = 0;
        editing_index = -1;
        stop_cursor_blink();
        invalidate_full();
    }

    // ── Motion Handling (Coalesced & Dirty-Rect Optimized) ────────────────

    bool on_motion(double x, double y) {
        double old_x = mouse_x;
        double old_y = mouse_y;
        mouse_x = x;
        mouse_y = y;

        // 0. Dragging text cursor while typing
        if (is_typing && is_typing_drag) {
            typing_cursor_pos = cursor_index_from_x(typing_text, text_size, typing_x, x);
            start_cursor_blink();
            invalidate_full();
            return true;
        }

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
            return true;
        }

        // 2. Active Drawing
        if (is_drawing) {
            if (current_tool == ToolType::Pen || current_tool == ToolType::Eraser) {
                if (!current_points.empty()) {
                    double lx = current_points.back().x;
                    double ly = current_points.back().y;
                    double dx = x - lx;
                    double dy = y - ly;
                    if (dx * dx + dy * dy < 3.0) return true;
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
            return true;
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

        return true;
    }

    // ── Cursor Management ─────────────────────────────────────────────────

    // Map CSS cursor names to X core cursor shapes (Xlib loads themed
    // versions of these automatically when libXcursor is available).
    static unsigned int font_cursor_for(const std::string& name) {
        if (name == "pointer") return XC_hand2;
        if (name == "move") return XC_fleur;
        if (name == "nwse-resize") return XC_bottom_right_corner;
        if (name == "nesw-resize") return XC_bottom_left_corner;
        if (name == "ns-resize") return XC_sb_v_double_arrow;
        if (name == "ew-resize") return XC_sb_h_double_arrow;
        if (name == "cell") return XC_plus;
        if (name == "text") return XC_xterm;
        if (name == "default") return XC_left_ptr;
        return XC_crosshair;
    }

    void set_cursor_by_name(const char* name) {
        if (current_cursor_name == name) return;
        current_cursor_name = name;
        if (!window) return;
        unsigned int shape = font_cursor_for(current_cursor_name);
        auto it = cursor_cache.find(shape);
        Cursor cur;
        if (it != cursor_cache.end()) {
            cur = it->second;
        } else {
            cur = XCreateFontCursor(dpy, shape);
            cursor_cache[shape] = cur;
        }
        XDefineCursor(dpy, window, cur);
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
        if (is_typing && typing_visual_bounds().padded(8.0).contains(x, y)) {
            set_cursor_by_name("text");
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
    // Driven by the main loop (see run_timers / next_timeout_ms).

    void start_cursor_blink() {
        blink_base_us = monotonic_us();
        blink_next_us = blink_base_us + 500000;
    }

    void stop_cursor_blink() {
        blink_next_us = 0;
        blink_base_us = 0;
    }

    // ── Keyboard Shortcuts ────────────────────────────────────────────────

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool on_key_press(XKeyEvent& kev) {
        char buf[128];
        KeySym keysym = NoSymbol;
        std::string text; // UTF-8 text produced by this key press (may be empty)
        if (xic) {
            Status status = 0;
            int len = Xutf8LookupString(xic, &kev, buf, sizeof(buf) - 1, &keysym, &status);
            if (status != XLookupKeySym && status != XLookupBoth) keysym = NoSymbol;
            if ((status == XLookupChars || status == XLookupBoth) && len > 0) text.assign(buf, len);
        } else {
            int len = XLookupString(&kev, buf, sizeof(buf) - 1, &keysym, nullptr);
            for (int i = 0; i < len; ++i) append_utf8(text, (unsigned char)buf[i]); // Latin-1 → UTF-8
        }
        bool ctrl = (kev.state & ControlMask) != 0;
        const char* keyname = (keysym != NoSymbol) ? XKeysymToString(keysym) : nullptr;
        std::string kname = keyname ? keyname : "";

        if (is_typing) {
            auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };

            if (ctrl) {
                if (kname == "z" || kname == "Z") {
                    commit_text();
                    undo();
                } else if (kname == "y" || kname == "Y") {
                    commit_text();
                    redo();
                } else if (kname == "BackSpace") {
                    if (typing_cursor_pos > 0) {
                        size_t old_pos = typing_cursor_pos;
                        while (typing_cursor_pos > 0 && is_space(typing_text[typing_cursor_pos - 1])) {
                            --typing_cursor_pos;
                        }
                        while (typing_cursor_pos > 0 && !is_space(typing_text[typing_cursor_pos - 1])) {
                            --typing_cursor_pos;
                            while (typing_cursor_pos > 0 && ((unsigned char)typing_text[typing_cursor_pos] & 0xC0) == 0x80) {
                                --typing_cursor_pos;
                            }
                        }
                        typing_text.erase(typing_cursor_pos, old_pos - typing_cursor_pos);
                        start_cursor_blink();
                        invalidate_full();
                    }
                } else if (kname == "Delete" || kname == "KP_Delete") {
                    if (typing_cursor_pos < typing_text.size()) {
                        size_t next_pos = typing_cursor_pos;
                        while (next_pos < typing_text.size() && is_space(typing_text[next_pos])) {
                            ++next_pos;
                        }
                        while (next_pos < typing_text.size() && !is_space(typing_text[next_pos])) {
                            ++next_pos;
                            while (next_pos < typing_text.size() && ((unsigned char)typing_text[next_pos] & 0xC0) == 0x80) {
                                ++next_pos;
                            }
                        }
                        typing_text.erase(typing_cursor_pos, next_pos - typing_cursor_pos);
                        start_cursor_blink();
                        invalidate_full();
                    }
                } else if (kname == "Left" || kname == "KP_Left") {
                    while (typing_cursor_pos > 0 && is_space(typing_text[typing_cursor_pos - 1])) {
                        --typing_cursor_pos;
                    }
                    while (typing_cursor_pos > 0 && !is_space(typing_text[typing_cursor_pos - 1])) {
                        --typing_cursor_pos;
                        while (typing_cursor_pos > 0 && ((unsigned char)typing_text[typing_cursor_pos] & 0xC0) == 0x80) {
                            --typing_cursor_pos;
                        }
                    }
                    start_cursor_blink();
                    invalidate_full();
                } else if (kname == "Right" || kname == "KP_Right") {
                    while (typing_cursor_pos < typing_text.size() && !is_space(typing_text[typing_cursor_pos])) {
                        ++typing_cursor_pos;
                        while (typing_cursor_pos < typing_text.size() && ((unsigned char)typing_text[typing_cursor_pos] & 0xC0) == 0x80) {
                            ++typing_cursor_pos;
                        }
                    }
                    while (typing_cursor_pos < typing_text.size() && is_space(typing_text[typing_cursor_pos])) {
                        ++typing_cursor_pos;
                    }
                    start_cursor_blink();
                    invalidate_full();
                } else if (kname == "Home" || kname == "KP_Home" || kname == "Up" || kname == "KP_Up") {
                    typing_cursor_pos = 0;
                    start_cursor_blink();
                    invalidate_full();
                } else if (kname == "End" || kname == "KP_End" || kname == "Down" || kname == "KP_Down") {
                    typing_cursor_pos = typing_text.size();
                    start_cursor_blink();
                    invalidate_full();
                }
                return true;
            }

            if (kname == "Return" || kname == "KP_Enter" || kname == "Escape") {
                commit_text();
            } else if (kname == "Left" || kname == "KP_Left") {
                if (typing_cursor_pos > 0) {
                    --typing_cursor_pos;
                    while (typing_cursor_pos > 0 && ((unsigned char)typing_text[typing_cursor_pos] & 0xC0) == 0x80) {
                        --typing_cursor_pos;
                    }
                    start_cursor_blink();
                    invalidate_full();
                }
            } else if (kname == "Right" || kname == "KP_Right") {
                if (typing_cursor_pos < typing_text.size()) {
                    ++typing_cursor_pos;
                    while (typing_cursor_pos < typing_text.size() && ((unsigned char)typing_text[typing_cursor_pos] & 0xC0) == 0x80) {
                        ++typing_cursor_pos;
                    }
                    start_cursor_blink();
                    invalidate_full();
                }
            } else if (kname == "Home" || kname == "KP_Home" || kname == "Up" || kname == "KP_Up") {
                typing_cursor_pos = 0;
                start_cursor_blink();
                invalidate_full();
            } else if (kname == "End" || kname == "KP_End" || kname == "Down" || kname == "KP_Down") {
                typing_cursor_pos = typing_text.size();
                start_cursor_blink();
                invalidate_full();
            } else if (kname == "BackSpace") {
                if (typing_cursor_pos > 0) {
                    size_t prev_pos = typing_cursor_pos - 1;
                    while (prev_pos > 0 && ((unsigned char)typing_text[prev_pos] & 0xC0) == 0x80) {
                        --prev_pos;
                    }
                    typing_text.erase(prev_pos, typing_cursor_pos - prev_pos);
                    typing_cursor_pos = prev_pos;
                    start_cursor_blink();
                    invalidate_full();
                }
            } else if (kname == "Delete" || kname == "KP_Delete") {
                if (typing_cursor_pos < typing_text.size()) {
                    size_t next_pos = typing_cursor_pos + 1;
                    while (next_pos < typing_text.size() && ((unsigned char)typing_text[next_pos] & 0xC0) == 0x80) {
                        ++next_pos;
                    }
                    typing_text.erase(typing_cursor_pos, next_pos - typing_cursor_pos);
                    start_cursor_blink();
                    invalidate_full();
                }
            } else {
                // Accept printable text only (no control characters / DEL)
                unsigned char first = text.empty() ? 0 : (unsigned char)text[0];
                if (first >= 32 && first != 127) {
                    typing_cursor_pos = std::min(typing_cursor_pos, typing_text.size());
                    typing_text.insert(typing_cursor_pos, text);
                    typing_cursor_pos += text.size();
                    start_cursor_blink();
                    invalidate_full();
                }
            }
            return true;
        }

        if (kname == "F9") {
            toggle_overlay();
        } else if ((kname == "Return" || kname == "KP_Enter" || kname == "F2") &&
                   selected_index >= 0 && selected_index < (int)strokes.size() &&
                   strokes[selected_index].kind == StrokeKind::Text) {
            begin_edit_text(selected_index);
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

        return true;
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
        if (!window || !has_shape_ext) return;

        if (!passthrough_mode) {
            XRectangle full_rect = {0, 0, (unsigned short)mon_width, (unsigned short)mon_height};
            XShapeCombineRectangles(dpy, window, ShapeInput, 0, 0, &full_rect, 1, ShapeSet, Unsorted);
            return;
        }

        std::vector<XRectangle> rects;
        BBox tb = toolbar_bounds();
        XRectangle tb_rect = {
            (short)std::floor(tb.x1), 0,
            (unsigned short)std::ceil(tb.width()), (unsigned short)(TOOLBAR_HEIGHT + 8)
        };
        rects.push_back(tb_rect);

        if (!submenu_open.empty()) {
            int sm_h = 130;
            if (submenu_open == "eraser_options") sm_h = 90;
            else if (submenu_open == "text_options") sm_h = 180;
            XRectangle sm_rect = {0, (short)TOOLBAR_HEIGHT, (unsigned short)mon_width, (unsigned short)sm_h};
            rects.push_back(sm_rect);
        }

        XShapeCombineRectangles(dpy, window, ShapeInput, 0, 0, rects.data(), (int)rects.size(), ShapeSet, Unsorted);
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
        map_window();
        current_cursor_name.clear();
        update_cursor(mouse_x, mouse_y);
        invalidate_full();
        present_dirty();
        present_window(CurrentTime);
        start_focus_keepalive();
    }

    void hide_overlay() {
        if (is_typing) commit_text();
        is_visible = false;
        has_focus = false;
        passthrough_mode = false;
        submenu_open.clear();
        submenu_items.clear();
        toolbar_cache_dirty = true;
        stop_focus_keepalive();
        update_input_shape();
        XWithdrawWindow(dpy, window, screen_num);
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

    void reclaim_focus() {
        if (is_visible && !passthrough_mode) {
            present_window(CurrentTime);
        }
    }

    void exit_passthrough() {
        passthrough_mode = false;
        toolbar_cache_dirty = true;
        update_input_shape();
        current_cursor_name.clear();
        update_cursor(mouse_x, mouse_y);
        invalidate_full();
        pending_reclaim_focus = true;
        start_focus_keepalive();
    }

    void reassert_top() {
        if (is_visible) {
            set_keep_above();
            XRaiseWindow(dpy, window);
        }
    }

    void on_focus_out() {
        if (is_visible) {
            set_keep_above();
            pending_reassert_top = true;
        }
    }

    void start_focus_keepalive() {
        focus_keepalive_next_us = monotonic_us() + 50000;
    }

    void stop_focus_keepalive() {
        focus_keepalive_next_us = 0;
    }
};

// ─── GNOME Custom Keybinding Setup ───────────────────────────────────────────

static bool program_on_path(const char* prog) {
    return std::system((std::string("command -v ") + prog + " >/dev/null 2>&1").c_str()) == 0;
}

static void run_cmd(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

static std::string capture_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

static std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
}

// Command the desktop should run to toggle the overlay of the running instance
static std::string toggle_command() {
    std::string exe;
    if (const char* appimage = std::getenv("APPIMAGE")) {
        exe = appimage;
    } else {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            exe = buf;
        }
    }
    if (exe.empty()) return "screen-draw --toggle";

    // Prefer the bare name when it resolves to this very binary via PATH
    std::string on_path = capture_cmd("command -v screen-draw 2>/dev/null");
    while (!on_path.empty() && (on_path.back() == '\n' || on_path.back() == '\r')) on_path.pop_back();
    if (!on_path.empty()) {
        char a[4096], b[4096];
        if (realpath(on_path.c_str(), a) && realpath(exe.c_str(), b) && std::strcmp(a, b) == 0) {
            return "screen-draw --toggle";
        }
    }
    return shell_quote(exe) + " --toggle";
}

static void setup_global_hotkey() {
    if (!program_on_path("gsettings")) return;

    std::string current = capture_cmd(std::string("gsettings get ") + GS_SCHEMA + " " + GS_KEY + " 2>/dev/null");
    if (current.empty()) return; // not a GNOME session / schema missing

    std::string toggle_cmd = toggle_command();
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

// ─── Single-Instance IPC (Unix socket) ───────────────────────────────────────

static bool fill_sockaddr(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Returns true if a running instance accepted the command
static bool send_ipc_command(const std::string& cmd) {
    sockaddr_un addr;
    if (!fill_sockaddr(addr, ipc_socket_path())) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    std::string msg = cmd + "\n";
    ssize_t sent = send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);
    close(fd);
    return sent == (ssize_t)msg.size();
}

static int create_ipc_listener(const std::string& path) {
    sockaddr_un addr;
    if (!fill_sockaddr(addr, path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(path.c_str()); // remove a stale socket from a crashed instance
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0600);
    return fd;
}

static void handle_ipc_client(int listen_fd, ScreenDrawWindow& win) {
    for (;;) {
        int cfd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) return;
        timeval tv = {0, 200000};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[64];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        close(cfd);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string cmd(buf);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();

        if (cmd == "toggle") win.toggle_overlay();
        else if (cmd == "show") { if (!win.is_visible) win.show_overlay(); else if (win.passthrough_mode) win.exit_passthrough(); }
        else if (cmd == "hide") { if (win.is_visible) win.hide_overlay(); }
        else if (cmd == "quit") win.quit_requested = true;
    }
}

// ─── Main ────────────────────────────────────────────────────────────────────

static int x_error_handler(Display* d, XErrorEvent* e) {
    char text[256];
    XGetErrorText(d, e->error_code, text, sizeof(text));
    std::fprintf(stderr, "[!] X error: %s (request %d.%d)\n", text, e->request_code, e->minor_code);
    return 0; // non-fatal, like GDK
}

static void print_usage(const char* prog) {
    std::printf("Usage: %s [--toggle | --show | --hide | --quit]\n"
                "  (no option)  start in the background, or toggle a running instance\n"
                "  --toggle     toggle the overlay (starts the app if needed)\n"
                "  --show       show the overlay (starts the app if needed)\n"
                "  --hide       hide the overlay of a running instance\n"
                "  --quit       quit a running instance\n", prog);
}

int main(int argc, char** argv) {
    std::string command; // "" = plain launch
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--toggle" || a == "-t") command = "toggle";
        else if (a == "--show") command = "show";
        else if (a == "--hide") command = "hide";
        else if (a == "--quit" || a == "-q") command = "quit";
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { print_usage(argv[0]); return 2; }
    }

    // Forward to an already running instance (a plain launch toggles it)
    if (send_ipc_command(command.empty() ? "toggle" : command)) return 0;
    if (command == "hide" || command == "quit") return 0; // nothing running

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
    std::fflush(stdout);

    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGPIPE, SIG_IGN);

    // Locale for XIM-based UTF-8 text input
    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");

    // Connects to XWayland on GNOME Wayland (via $DISPLAY) so the fullscreen
    // transparent overlay can stay always-on-top.
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "[!] Cannot open X display (is $DISPLAY set? XWayland is required on Wayland)\n");
        return 1;
    }
    XSetErrorHandler(x_error_handler);

    std::string sock_path = ipc_socket_path();
    int listen_fd = create_ipc_listener(sock_path);
    if (listen_fd < 0) {
        std::fprintf(stderr, "[!] Could not create control socket at %s; F9 toggling will not work\n", sock_path.c_str());
    } else {
        std::printf("[\u2713] Control socket listening at %s\n", sock_path.c_str());
    }

    setup_global_hotkey();

    auto win = std::make_unique<ScreenDrawWindow>(dpy);
    if (command == "toggle" || command == "show") win->show_overlay();

    int xfd = ConnectionNumber(dpy);
    while (!win->quit_requested) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            win->handle_event(ev);
        }
        win->run_idle();
        win->run_timers();
        win->present_dirty();
        XFlush(dpy);
        if (win->quit_requested) break;
        if (XPending(dpy) > 0) continue;

        pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = {xfd, POLLIN, 0};
        if (listen_fd >= 0) fds[nfds++] = {listen_fd, POLLIN, 0};
        int r = poll(fds, nfds, win->next_timeout_ms());
        if (r > 0 && listen_fd >= 0 && (fds[1].revents & POLLIN)) {
            handle_ipc_client(listen_fd, *win);
        }
    }

    win.reset();
    if (listen_fd >= 0) {
        close(listen_fd);
        unlink(sock_path.c_str());
    }
    XCloseDisplay(dpy);
    return 0;
}
