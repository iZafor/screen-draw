#include "window.hpp"

void ScreenDrawWindow::build_toolbar() {
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

void ScreenDrawWindow::layout_toolbar(int width) {
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

BBox ScreenDrawWindow::toolbar_bounds() const {
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

std::string ScreenDrawWindow::tool_to_name(ToolType t) const {
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

void ScreenDrawWindow::draw_toolbar_cached(cairo_t* cr, int width) {
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

void ScreenDrawWindow::draw_toolbar_tooltip(cairo_t* cr, int width) {
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

void ScreenDrawWindow::draw_toolbar_direct(cairo_t* cr) {
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

void ScreenDrawWindow::draw_button_icon(cairo_t* cr, const std::string& name, double x, double y, double w, double h, bool active) {
    double alpha = active ? 0.95 : 0.65;
    cairo_set_line_width(cr, 2.0);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    double cx = x + w * 0.5;
    double cy = y + h * 0.5;

    if (name == "select") {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);
        double r = 9.5;
        cairo_move_to(cr, cx - r, cy);
        cairo_line_to(cr, cx + r, cy);
        cairo_move_to(cr, cx, cy - r);
        cairo_line_to(cr, cx, cy + r);
        cairo_stroke(cr);

        double a = 3.2;
        cairo_move_to(cr, cx - r + a, cy - a);
        cairo_line_to(cr, cx - r, cy);
        cairo_line_to(cr, cx - r + a, cy + a);
        cairo_move_to(cr, cx + r - a, cy - a);
        cairo_line_to(cr, cx + r, cy);
        cairo_line_to(cr, cx + r - a, cy + a);
        cairo_move_to(cr, cx - a, cy - r + a);
        cairo_line_to(cr, cx, cy - r);
        cairo_line_to(cr, cx + a, cy - r + a);
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

const ToolbarButton* ScreenDrawWindow::find_button(const std::string& name) const {
    for (const auto& b : toolbar_buttons) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

void ScreenDrawWindow::draw_pen_submenu(cairo_t* cr, int width) {
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

void ScreenDrawWindow::draw_eraser_submenu(cairo_t* cr, int width) {
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

void ScreenDrawWindow::draw_text_submenu(cairo_t* cr, int width) {
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

const ToolbarButton* ScreenDrawWindow::hit_toolbar(double x, double y) const {
    for (const auto& b : toolbar_buttons) {
        if (b.name.rfind("__sep", 0) == 0) continue;
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
            return &b;
        }
    }
    return nullptr;
}

const SubmenuItem* ScreenDrawWindow::hit_submenu(double x, double y) const {
    for (const auto& item : submenu_items) {
        if (x >= item.x && x <= item.x + item.w && y >= item.y && y <= item.y + item.h) {
            return &item;
        }
    }
    return nullptr;
}

bool ScreenDrawWindow::in_toolbar_zone(double y) const {
    return y <= TOOLBAR_HEIGHT;
}

bool ScreenDrawWindow::in_submenu_zone(double /*x*/, double y) const {
    if (submenu_open.empty()) return false;
    if (submenu_open == "eraser_options") return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 90.0;
    if (submenu_open == "text_options")   return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 180.0;
    return y > TOOLBAR_HEIGHT && y < TOOLBAR_HEIGHT + 130.0;
}

void ScreenDrawWindow::handle_toolbar_click(const std::string& name) {
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

void ScreenDrawWindow::handle_submenu_click(const SubmenuItem& item) {
    toolbar_cache_dirty = true;
    if (item.type == "color") {
        current_color = item.str_val;
        bool any = false;
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].kind != StrokeKind::Text) {
                if (!any) { push_undo_snapshot(); any = true; }
                strokes[idx].color = current_color;
            }
        }
        if (any) rebuild_canvas(-1);
    } else if (item.type == "pen_stroke") {
        stroke_width = item.int_val;
        bool any = false;
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].kind != StrokeKind::Text) {
                if (!any) { push_undo_snapshot(); any = true; }
                strokes[idx].width = stroke_width;
            }
        }
        if (any) rebuild_canvas(-1);
    } else if (item.type == "eraser_radius") {
        eraser_radius = item.int_val;
    } else if (item.type == "text_color") {
        text_color = item.str_val;
        bool any = false;
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].kind == StrokeKind::Text) {
                if (!any) { push_undo_snapshot(); any = true; }
                strokes[idx].color = text_color;
            }
        }
        if (any) rebuild_canvas(-1);
    } else if (item.type == "text_bg_color") {
        text_bg_color = item.str_val;
        bool any = false;
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].kind == StrokeKind::Text) {
                if (!any) { push_undo_snapshot(); any = true; }
                strokes[idx].bg_color = text_bg_color;
                strokes[idx].text_metrics_valid = false;
            }
        }
        if (any) rebuild_canvas(-1);
    } else if (item.type == "text_size") {
        text_size = item.int_val;
        bool any = false;
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].kind == StrokeKind::Text) {
                if (!any) { push_undo_snapshot(); any = true; }
                strokes[idx].font_size = text_size;
                strokes[idx].text_metrics_valid = false;
            }
        }
        if (any) rebuild_canvas(-1);
    }
    invalidate_full();
}
