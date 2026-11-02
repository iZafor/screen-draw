#include "geometry.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

int64_t monotonic_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

RGBA hex_to_rgba(const std::string& hex_str, double alpha) {
    if (hex_str.empty()) return {1.0, 0.0, 0.0, alpha};
    const char* s = hex_str.c_str();
    if (s[0] == '#') s++;
    if (std::strlen(s) < 6) return {1.0, 0.0, 0.0, alpha};
    unsigned int rv = 0, gv = 0, bv = 0;
    std::sscanf(s, "%02x%02x%02x", &rv, &gv, &bv);
    return {rv / 255.0, gv / 255.0, bv / 255.0, alpha};
}

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (w <= 0 || h <= 0) return;
    r = std::min({r, w * 0.5, h * 0.5});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
    cairo_close_path(cr);
}

double point_to_segment_dist(double px, double py, double x1, double y1, double x2, double y2) {
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

bool segment_intersects_box(double x1, double y1, double x2, double y2, const BBox& box) {
    if (std::max(x1, x2) < box.x1 || std::min(x1, x2) > box.x2 ||
        std::max(y1, y2) < box.y1 || std::min(y1, y2) > box.y2) {
        return false;
    }
    double dx = x2 - x1;
    double dy = y2 - y1;
    auto cross = [&](double px, double py) {
        return dx * (py - y1) - dy * (px - x1);
    };
    double c1 = cross(box.x1, box.y1);
    double c2 = cross(box.x2, box.y1);
    double c3 = cross(box.x2, box.y2);
    double c4 = cross(box.x1, box.y2);
    if ((c1 > 0 && c2 > 0 && c3 > 0 && c4 > 0) ||
        (c1 < 0 && c2 < 0 && c3 < 0 && c4 < 0)) {
        return false;
    }
    return true;
}

void append_utf8(std::string& out, uint32_t cp) {
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

size_t cursor_index_from_x(const std::string& str, double font_sz, double start_x, double target_x) {
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
