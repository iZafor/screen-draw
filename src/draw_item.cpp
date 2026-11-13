#include "draw_item.hpp"
#include "latex_math.hpp"

void DrawItem::update_text_metrics() const {
    if (kind != StrokeKind::Text) return;
    cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(tmp);
    if (contains_latex_math(text)) {
        MathMetrics m = measure_latex_text(cr, text, font_size);
        cached_text_w = m.width;
        cached_text_h = m.height;
        cached_x_bearing = m.x_bearing;
        cached_y_bearing = m.y_bearing;
    } else {
        cairo_select_font_face(cr, "Inter", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, font_size);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, text.c_str(), &ext);
        cached_text_w = std::max(ext.width, 10.0);
        cached_text_h = std::max(ext.height, font_size * 0.75);
        cached_x_bearing = ext.x_bearing;
        cached_y_bearing = (ext.height > 0) ? ext.y_bearing : -font_size * 0.75;
    }
    text_metrics_valid = true;
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);
}

BBox DrawItem::raw_bounds() const {
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

BBox DrawItem::visual_bounds(bool include_handles) const {
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

void DrawItem::draw(cairo_t* cr) const {
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
        if (contains_latex_math(text)) {
            MathMetrics m = measure_latex_text(cr, text, font_size);
            cached_text_w = m.width;
            cached_text_h = m.height;
            cached_x_bearing = m.x_bearing;
            cached_y_bearing = m.y_bearing;
            text_metrics_valid = true;

            if (!bg_color.empty() && bg_color != "transparent") {
                RGBA bg = hex_to_rgba(bg_color);
                cairo_set_source_rgba(cr, bg.r, bg.g, bg.b, bg.a);
                double pad = 8.0;
                cairo_rectangle(cr,
                                text_x + m.x_bearing - pad,
                                text_y + m.y_bearing - pad,
                                m.width + pad * 2.0,
                                m.height + pad * 2.0);
                cairo_fill(cr);
            }

            RGBA fg = hex_to_rgba(color);
            cairo_set_source_rgba(cr, fg.r, fg.g, fg.b, fg.a);
            draw_latex_text(cr, text, text_x, text_y, font_size);
        } else {
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
}

bool DrawItem::hit_test(double px, double py, bool allow_interior_bbox) const {
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

bool DrawItem::intersects_box(const BBox& box) const {
    if (!is_selectable()) return false;
    BBox rb = raw_bounds();
    double tol = width * 0.5 + 2.0;
    if (!rb.padded(tol).intersects(box.x1, box.y1, box.width(), box.height())) {
        return false;
    }
    if (kind == StrokeKind::Shape) {
        if (shape_type == ToolType::Line || shape_type == ToolType::Arrow) {
            BBox pbox = box.padded(tol);
            if (segment_intersects_box(x1, y1, x2, y2, pbox)) return true;
            if (shape_type == ToolType::Arrow) {
                double angle = std::atan2(y2 - y1, x2 - x1);
                double head_len = std::max(width * 3.5, 20.0);
                double spread = M_PI / 6.0;
                for (int sign : {-1, 1}) {
                    double lx = x2 - head_len * std::cos(angle + sign * spread);
                    double ly = y2 - head_len * std::sin(angle + sign * spread);
                    if (segment_intersects_box(x2, y2, lx, ly, pbox)) return true;
                }
            }
            return false;
        }
        return true;
    } else if (kind == StrokeKind::Text) {
        return true;
    } else if (kind == StrokeKind::Freehand) {
        BBox pbox = box.padded(tol);
        if (points.size() == 1) {
            return pbox.contains(points[0].x, points[0].y);
        }
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            if (segment_intersects_box(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y, pbox)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

std::vector<HandlePoint> DrawItem::get_handles() const {
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

HandleId DrawItem::hit_handle(double px, double py) const {
    auto handles = get_handles();
    for (const auto& h : handles) {
        if (std::hypot(px - h.x, py - h.y) <= HANDLE_HIT_RADIUS) {
            return h.id;
        }
    }
    return HandleId::None;
}

void DrawItem::apply_move_from(const DrawItem& orig, double dx, double dy) {
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

void DrawItem::apply_resize_from(const DrawItem& orig, HandleId hid, double dx, double dy) {
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
        if (std::abs(nw) < 4.0) nw = (nw >= 0 ? 4.0 : -4.0);
        if (std::abs(nh) < 4.0) nh = (nh >= 0 ? 4.0 : -4.0);
        points.resize(orig.points.size());
        for (size_t i = 0; i < orig.points.size(); ++i) {
            double tx = (orig.points[i].x - ob.x1) / ow;
            double ty = (orig.points[i].y - ob.y1) / oh;
            points[i].x = nx1 + tx * nw;
            points[i].y = ny1 + ty * nh;
        }
    }
}

void DrawItem::draw_selection_overlay(cairo_t* cr, bool show_handles) const {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (kind == StrokeKind::Shape && (shape_type == ToolType::Line || shape_type == ToolType::Arrow)) {
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

    if (show_handles) {
        auto handles = get_handles();
        for (const auto& h : handles) {
            cairo_arc(cr, h.x, h.y + 1.0, HANDLE_RADIUS + 1.0, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
            cairo_fill(cr);

            cairo_arc(cr, h.x, h.y, HANDLE_RADIUS, 0.0, 2.0 * M_PI);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(cr);

            cairo_set_source_rgba(cr, 0.0, 0.48, 1.0, 1.0);
            cairo_set_line_width(cr, 2.0);
            cairo_stroke(cr);
        }
    }

    cairo_restore(cr);
}
