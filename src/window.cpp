#include "window.hpp"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

ScreenDrawWindow::ScreenDrawWindow(Display* display) : dpy(display) {
    setup_window();
    build_toolbar();
}

ScreenDrawWindow::ScreenDrawWindow(int width, int height)
    : dpy(nullptr), mon_width(width), mon_height(height) {
    ensure_canvas(mon_width, mon_height);
    build_toolbar();
    layout_toolbar(mon_width);
}

ScreenDrawWindow::~ScreenDrawWindow() {
    stop_focus_keepalive();
    stop_cursor_blink();
    if (canvas_surface) cairo_surface_destroy(canvas_surface);
    if (toolbar_cache_surface) cairo_surface_destroy(toolbar_cache_surface);
    destroy_back_buffer();
    if (dirty_region) cairo_region_destroy(dirty_region);
    if (xic) XDestroyIC(xic);
    if (xim) XCloseIM(xim);
    if (dpy) {
        for (auto& kv : cursor_cache) XFreeCursor(dpy, kv.second);
        if (gc) XFreeGC(dpy, gc);
        if (window) XDestroyWindow(dpy, window);
        if (colormap) XFreeColormap(dpy, colormap);
    }
}

void ScreenDrawWindow::push_undo_snapshot() {
    undo_stack.push_back(strokes);
    if (undo_stack.size() > 100) {
        undo_stack.erase(undo_stack.begin());
    }
    redo_stack.clear();
}

void ScreenDrawWindow::setup_window() {
    if (!dpy) return;
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
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       KeyPressMask | FocusChangeMask;
    window = XCreateWindow(dpy, root, win_x, win_y, mon_width, mon_height, 0,
                           depth, InputOutput, visual,
                           CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    gc = XCreateGC(dpy, window, 0, nullptr);

    const char* title = "Screen Draw";
    XStoreName(dpy, window, title);
    XChangeProperty(dpy, window, a_net_wm_name, a_utf8, 8, PropModeReplace,
                    (const unsigned char*)title, (int)std::strlen(title));
    XClassHint class_hint;
    class_hint.res_name = const_cast<char*>("screen-draw");
    class_hint.res_class = const_cast<char*>("Screen-draw");
    XSetClassHint(dpy, window, &class_hint);

    XSizeHints* size_hints = XAllocSizeHints();
    size_hints->flags = USPosition | USSize | PMinSize | PMaxSize;
    size_hints->x = win_x;
    size_hints->y = win_y;
    size_hints->width = size_hints->min_width = size_hints->max_width = mon_width;
    size_hints->height = size_hints->min_height = size_hints->max_height = mon_height;
    XSetWMNormalHints(dpy, window, size_hints);
    XFree(size_hints);

    XWMHints* wm_hints = XAllocWMHints();
    wm_hints->flags = InputHint | StateHint;
    wm_hints->input = True;
    wm_hints->initial_state = NormalState;
    XSetWMHints(dpy, window, wm_hints);
    XFree(wm_hints);

    XSetWMProtocols(dpy, window, &a_wm_delete, 1);

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

void ScreenDrawWindow::setup_input_method() {
    if (!dpy) return;
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

void ScreenDrawWindow::send_wm_message(Atom type, long l0, long l1, long l2, long l3) {
    if (!dpy || !window) return;
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

void ScreenDrawWindow::set_keep_above() {
    if (!is_visible) return;
    send_wm_message(a_net_wm_state, 1 /* _NET_WM_STATE_ADD */, (long)a_net_wm_state_above, 0, 1);
}

void ScreenDrawWindow::present_window(Time t) {
    if (!is_visible || !dpy || !window) return;
    XRaiseWindow(dpy, window);
    send_wm_message(a_net_active_window, 2, (long)t, 0, 0);
}

void ScreenDrawWindow::map_window() {
    if (!dpy || !window) return;
    Atom states[3] = {a_net_wm_state_above, a_net_wm_state_skip_taskbar, a_net_wm_state_skip_pager};
    XChangeProperty(dpy, window, a_net_wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)states, 3);
    XMoveResizeWindow(dpy, window, win_x, win_y, mon_width, mon_height);
    XMapRaised(dpy, window);
}

static bool& x_error_trap_flag() {
    static bool flag = false;
    return flag;
}

static int trap_x_error(Display*, XErrorEvent*) {
    x_error_trap_flag() = true;
    return 0;
}

void ScreenDrawWindow::destroy_back_buffer() {
    if (back_surface) {
        cairo_surface_destroy(back_surface);
        back_surface = nullptr;
    }
    if (back_image) {
        if (back_uses_shm && dpy) {
            XShmDetach(dpy, &shm_info);
            XSync(dpy, False);
            back_image->data = nullptr;
            XDestroyImage(back_image);
            shmdt(shm_info.shmaddr);
        } else {
            XDestroyImage(back_image);
        }
        back_image = nullptr;
    }
    back_uses_shm = false;
    back_w = back_h = 0;
}

bool ScreenDrawWindow::create_shm_back_buffer(int w, int h) {
    if (!dpy || !XShmQueryExtension(dpy)) return false;
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
    shmctl(shm_info.shmid, IPC_RMID, nullptr);

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

void ScreenDrawWindow::ensure_back_buffer(int w, int h) {
    if (!dpy) return;
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
    static const uint32_t endian_probe = 1;
    back_image->byte_order = (*(const uint8_t*)&endian_probe == 1) ? LSBFirst : MSBFirst;

    back_surface = cairo_image_surface_create_for_data(
        (unsigned char*)back_image->data, CAIRO_FORMAT_ARGB32, w, h, back_image->bytes_per_line);
    back_w = w;
    back_h = h;
}

void ScreenDrawWindow::add_dirty_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!dirty_region) dirty_region = cairo_region_create();
    cairo_rectangle_int_t r = {x, y, w, h};
    cairo_region_union_rectangle(dirty_region, &r);
}

void ScreenDrawWindow::present_dirty() {
    if (!dirty_region || cairo_region_is_empty(dirty_region)) return;
    if (!is_visible || !dpy || !window) {
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
    if (back_uses_shm) XSync(dpy, False);

    cairo_region_destroy(dirty_region);
    dirty_region = nullptr;
}

void ScreenDrawWindow::handle_event(XEvent& ev) {
    if (dpy && XFilterEvent(&ev, XNone)) return;

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
                on_button_press(ev.xbutton.x, ev.xbutton.y, ev.xbutton.time, ev.xbutton.state);
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

void ScreenDrawWindow::run_idle() {
    if (pending_reclaim_focus) {
        pending_reclaim_focus = false;
        reclaim_focus();
    }
    if (pending_reassert_top) {
        pending_reassert_top = false;
        reassert_top();
    }
}

void ScreenDrawWindow::run_timers() {
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

int ScreenDrawWindow::next_timeout_ms() const {
    int64_t next = 0;
    if (blink_next_us) next = blink_next_us;
    if (focus_keepalive_next_us && (!next || focus_keepalive_next_us < next)) next = focus_keepalive_next_us;
    if (!next) return -1;
    int64_t diff = next - monotonic_us();
    if (diff <= 0) return 0;
    return (int)((diff + 999) / 1000);
}

void ScreenDrawWindow::ensure_canvas(int w, int h) {
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

void ScreenDrawWindow::rebuild_canvas(int skip_idx) {
    if (!canvas_surface) return;
    if (skip_idx < 0 && is_typing && editing_index >= 0) skip_idx = editing_index;
    canvas_skip_index = skip_idx;
    cairo_t* cr = cairo_create(canvas_surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (size_t i = 0; i < strokes.size(); ++i) {
        if ((int)i == skip_idx) continue;
        if (canvas_skip_selection && is_selected((int)i)) continue;
        strokes[i].draw(cr);
    }
    cairo_destroy(cr);
}

void ScreenDrawWindow::invalidate_bbox(const BBox& b) {
    if (!window) return;
    int x = std::max(0, (int)std::floor(b.x1) - 4);
    int y = std::max(0, (int)std::floor(b.y1) - 4);
    int r = std::min(mon_width, (int)std::ceil(b.x2) + 4);
    int bot = std::min(mon_height, (int)std::ceil(b.y2) + 4);
    if (r > x && bot > y) {
        add_dirty_rect(x, y, r - x, bot - y);
    }
}

void ScreenDrawWindow::invalidate_toolbar() {
    toolbar_cache_dirty = true;
    BBox tb = toolbar_bounds();
    tb.y2 += 36.0;
    invalidate_bbox(tb);
}

void ScreenDrawWindow::invalidate_full() {
    if (window) add_dirty_rect(0, 0, mon_width, mon_height);
}

void ScreenDrawWindow::on_draw(cairo_t* cr, int w, int h, const cairo_rectangle_int_t& clip_rect) {
    ensure_canvas(w, h);
    layout_toolbar(w);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (!passthrough_mode) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.03);
        cairo_paint(cr);
    }

    cairo_set_source_surface(cr, canvas_surface, 0.0, 0.0);
    cairo_paint(cr);

    if (canvas_skip_selection) {
        for (int idx : selected_indices) {
            if (idx >= 0 && idx < (int)strokes.size() && !(is_typing && idx == editing_index)) {
                strokes[idx].draw(cr);
            }
        }
    } else if (canvas_skip_index >= 0 && canvas_skip_index < (int)strokes.size() &&
               !(is_typing && canvas_skip_index == editing_index)) {
        strokes[canvas_skip_index].draw(cr);
    }

    if (is_drawing) {
        draw_current_stroke(cr);
    }

    if (is_typing) {
        draw_typing_text(cr);
    }

    if (!selected_indices.empty() && !passthrough_mode) {
        if (selected_indices.size() == 1) {
            int idx = selected_indices[0];
            if (idx >= 0 && idx < (int)strokes.size()) {
                const auto& sel = strokes[idx];
                if (sel.visual_bounds(true).intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
                    sel.draw_selection_overlay(cr, true);
                }
            }
        } else {
            for (int idx : selected_indices) {
                if (idx >= 0 && idx < (int)strokes.size()) {
                    const auto& sel = strokes[idx];
                    if (sel.visual_bounds(false).intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
                        sel.draw_selection_overlay(cr, false);
                    }
                }
            }
            BBox gb = selection_raw_bounds().padded(6.0);
            if (gb.padded(4.0).intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
                cairo_save(cr);
                cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
                double dashes[] = {6.0, 4.0};
                cairo_set_dash(cr, dashes, 2, 0.0);
                cairo_set_line_width(cr, 1.5);
                cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 0.95);
                cairo_rectangle(cr, gb.x1, gb.y1, gb.width(), gb.height());
                cairo_stroke(cr);
                cairo_restore(cr);
            }
        }
    }

    if (is_box_selecting && !passthrough_mode) {
        double bx = std::min(box_select_start.x, box_select_end.x);
        double by = std::min(box_select_start.y, box_select_end.y);
        double bw = std::abs(box_select_end.x - box_select_start.x);
        double bh = std::abs(box_select_end.y - box_select_start.y);
        if (bw > 1.0 || bh > 1.0) {
            cairo_save(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 0.12);
            cairo_rectangle(cr, bx, by, bw, bh);
            cairo_fill_preserve(cr);
            double dashes[] = {5.0, 4.0};
            cairo_set_dash(cr, dashes, 2, 0.0);
            cairo_set_line_width(cr, 1.5);
            cairo_set_source_rgba(cr, 0.25, 0.62, 1.0, 0.85);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (current_tool == ToolType::Eraser && !passthrough_mode) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_new_sub_path(cr);
        cairo_arc(cr, mouse_x, mouse_y, eraser_radius, 0.0, 2.0 * M_PI);
        cairo_stroke(cr);
    }

    BBox tb = toolbar_bounds();
    tb.y2 += 36.0;
    if (tb.intersects(clip_rect.x, clip_rect.y, clip_rect.width, clip_rect.height)) {
        draw_toolbar_cached(cr, w);
        if (!hover_button.empty() && submenu_open.empty()) {
            draw_toolbar_tooltip(cr, w);
        }
    }

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

void ScreenDrawWindow::draw_typing_text(cairo_t* cr) {
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

void ScreenDrawWindow::draw_current_stroke(cairo_t* cr) {
    if (current_tool == ToolType::Pen && current_points.size() >= 2) {
        DrawItem item;
        item.kind = StrokeKind::Freehand;
        item.points = current_points;
        item.color = current_color;
        item.width = stroke_width;
        item.is_eraser = false;
        item.draw(cr);
    } else if (current_tool == ToolType::Eraser && current_points.size() >= 2) {
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

void ScreenDrawWindow::sync_selected_index() {
    std::sort(selected_indices.begin(), selected_indices.end());
    selected_indices.erase(std::unique(selected_indices.begin(), selected_indices.end()), selected_indices.end());
    selected_indices.erase(
        std::remove_if(selected_indices.begin(), selected_indices.end(),
                       [&](int idx) { return idx < 0 || idx >= (int)strokes.size() || !strokes[idx].is_selectable(); }),
        selected_indices.end());
    selected_index = (selected_indices.size() == 1) ? selected_indices[0] : -1;
}

bool ScreenDrawWindow::has_selection() const {
    return !selected_indices.empty();
}

bool ScreenDrawWindow::is_selected(int idx) const {
    return std::binary_search(selected_indices.begin(), selected_indices.end(), idx);
}

BBox ScreenDrawWindow::selection_raw_bounds() const {
    BBox res = {0, 0, -1, -1};
    for (int idx : selected_indices) {
        if (idx >= 0 && idx < (int)strokes.size()) {
            res = res.united(strokes[idx].raw_bounds());
        }
    }
    return res;
}

BBox ScreenDrawWindow::selection_visual_bounds() const {
    BBox res = {0, 0, -1, -1};
    for (int idx : selected_indices) {
        if (idx >= 0 && idx < (int)strokes.size()) {
            res = res.united(strokes[idx].visual_bounds(true));
        }
    }
    if (res.valid()) return res.padded(6.0);
    return res;
}

void ScreenDrawWindow::invalidate_selection() {
    if (has_selection()) {
        invalidate_bbox(selection_visual_bounds());
    }
}

void ScreenDrawWindow::clear_selection() {
    if (!selected_indices.empty()) {
        BBox b = selection_visual_bounds();
        selected_indices.clear();
        selected_index = -1;
        invalidate_bbox(b);
    } else {
        selected_index = -1;
    }
}

void ScreenDrawWindow::set_single_selection(int idx) {
    BBox old_b = selection_visual_bounds();
    bool had_sel = has_selection();
    selected_indices.clear();
    if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].is_selectable()) {
        selected_indices.push_back(idx);
    }
    sync_selected_index();
    if (had_sel) invalidate_bbox(old_b);
    if (has_selection()) invalidate_bbox(selection_visual_bounds());
}

void ScreenDrawWindow::toggle_selection_item(int idx) {
    if (idx < 0 || idx >= (int)strokes.size() || !strokes[idx].is_selectable()) return;
    BBox old_b = selection_visual_bounds();
    bool had_sel = has_selection();
    auto it = std::find(selected_indices.begin(), selected_indices.end(), idx);
    if (it != selected_indices.end()) {
        selected_indices.erase(it);
    } else {
        selected_indices.push_back(idx);
    }
    sync_selected_index();
    if (had_sel) invalidate_bbox(old_b);
    if (has_selection()) invalidate_bbox(selection_visual_bounds());
    invalidate_bbox(strokes[idx].visual_bounds(true));
}

int ScreenDrawWindow::find_topmost_selectable_at(double x, double y, bool force_interior_bbox) const {
    for (int i = (int)strokes.size() - 1; i >= 0; --i) {
        if (strokes[i].hit_test(x, y, false)) {
            return i;
        }
    }
    if (current_tool == ToolType::Select || force_interior_bbox) {
        for (int i = (int)strokes.size() - 1; i >= 0; --i) {
            if (strokes[i].hit_test(x, y, true)) {
                return i;
            }
        }
    }
    return -1;
}

int ScreenDrawWindow::hit_selected_at(double x, double y) const {
    for (int i = (int)selected_indices.size() - 1; i >= 0; --i) {
        int idx = selected_indices[i];
        if (idx >= 0 && idx < (int)strokes.size() && strokes[idx].hit_test(x, y, true)) {
            return idx;
        }
    }
    return -1;
}

int ScreenDrawWindow::find_topmost_text_at(double x, double y) const {
    for (int i = (int)strokes.size() - 1; i >= 0; --i) {
        if (strokes[i].kind == StrokeKind::Text && strokes[i].hit_test(x, y, true)) {
            return i;
        }
    }
    return -1;
}

BBox ScreenDrawWindow::typing_visual_bounds() const {
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

void ScreenDrawWindow::begin_edit_text(int idx, double click_x) {
    if (idx < 0 || idx >= (int)strokes.size() || strokes[idx].kind != StrokeKind::Text) return;
    const DrawItem& item = strokes[idx];
    BBox old_b = item.visual_bounds(true);
    clear_selection();

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
    text_color = item.color;
    text_size = item.font_size;
    text_bg_color = item.bg_color.empty() ? "transparent" : item.bg_color;
    is_manipulating = false;
    is_box_selecting = false;
    canvas_skip_selection = false;
    active_handle = HandleId::None;
    toolbar_cache_dirty = true;

    rebuild_canvas(idx);
    start_cursor_blink();
    invalidate_bbox(old_b);
    invalidate_full();
}

bool ScreenDrawWindow::begin_edit_text_at(double x, double y) {
    if (is_typing) commit_text();
    int idx = find_topmost_text_at(x, y);
    if (idx < 0) return false;
    begin_edit_text(idx, x);
    update_cursor(x, y);
    return true;
}

bool ScreenDrawWindow::on_button_press(double x, double y, Time time, unsigned int state) {
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
            if (last_toolbar_click_time != 0 && time != 0 && time - last_toolbar_click_time < 150) return true;
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
    bool is_double_click = (last_click_time != 0 && time != 0 && (time - last_click_time) < 400) &&
                           std::hypot(x - last_click_x, y - last_click_y) < 6.0;
    last_click_time = time;
    last_click_x = x;
    last_click_y = y;
    if (is_double_click && begin_edit_text_at(x, y)) {
        last_click_time = 0;
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

    bool multi_mod = (state & (ShiftMask | ControlMask)) != 0;
    pending_single_select_index = -1;

    // 6. When Shift/Ctrl is held, toggle items in the multi-selection (or start additive marquee in Select tool)
    if (multi_mod && (current_tool == ToolType::Select || has_selection())) {
        int hit_idx = find_topmost_selectable_at(x, y, true);
        if (hit_idx >= 0) {
            toggle_selection_item(hit_idx);
            if (is_selected(hit_idx)) {
                start_manipulation(HandleId::Body, x, y);
            }
            update_cursor(x, y);
            return true;
        }
        if (current_tool == ToolType::Select) {
            start_box_selection(x, y, true);
            return true;
        }
    }

    // 7. Check if user clicked a resize handle of a single selected shape
    if (!multi_mod && selected_index >= 0 && selected_index < (int)strokes.size()) {
        HandleId hid = strokes[selected_index].hit_handle(x, y);
        if (hid != HandleId::None) {
            start_manipulation(hid, x, y);
            return true;
        }
    }

    // 8. If in Select tool, handle single/multi-item selection, group move, or marquee box drag
    if (current_tool == ToolType::Select) {
        int hit_idx = find_topmost_selectable_at(x, y, true);
        if (hit_idx >= 0) {
            if (is_selected(hit_idx)) {
                if (selected_indices.size() > 1) {
                    pending_single_select_index = hit_idx;
                }
                start_manipulation(HandleId::Body, x, y);
            } else {
                set_single_selection(hit_idx);
                HandleId hid = strokes[hit_idx].hit_handle(x, y);
                start_manipulation((hid != HandleId::None) ? hid : HandleId::Body, x, y);
            }
            return true;
        }
        if (selected_indices.size() > 1 && selection_raw_bounds().padded(4.0).contains(x, y)) {
            start_manipulation(HandleId::Body, x, y);
            return true;
        }
        start_box_selection(x, y, false);
        return true;
    }

    // 9. When not in Select tool, clicking an already-selected shape(s) still moves (or edits) it
    if (has_selection()) {
        int sel_hit = hit_selected_at(x, y);
        if (sel_hit >= 0 || (selected_indices.size() > 1 && selection_raw_bounds().padded(4.0).contains(x, y))) {
            if (selected_index >= 0 && current_tool == ToolType::Text &&
                strokes[selected_index].kind == StrokeKind::Text) {
                begin_edit_text(selected_index, x);
                return true;
            }
            start_manipulation(HandleId::Body, x, y);
            return true;
        }
        clear_selection();
    }

    // 10. Text tool: click an existing text to edit it, or empty space to start new text
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

    // 11. Start drawing Pen, Eraser, or Shape
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

void ScreenDrawWindow::start_box_selection(double x, double y, bool additive) {
    is_box_selecting = true;
    box_select_start = {x, y};
    box_select_end = {x, y};
    if (additive) {
        box_select_initial = selected_indices;
    } else {
        clear_selection();
        box_select_initial.clear();
    }
    update_cursor(x, y);
}

void ScreenDrawWindow::start_manipulation(HandleId hid, double x, double y) {
    if (selected_indices.empty()) return;
    is_manipulating = true;
    active_handle = hid;
    drag_start_x = x;
    drag_start_y = y;
    drag_orig_items.clear();
    drag_orig_items.reserve(selected_indices.size());
    for (int idx : selected_indices) {
        if (idx >= 0 && idx < (int)strokes.size()) {
            drag_orig_items.push_back({idx, strokes[idx]});
        }
    }
    if (selected_index >= 0 && selected_index < (int)strokes.size()) {
        drag_orig_item = strokes[selected_index];
    }
    drag_modified = false;
    canvas_skip_selection = true;
    rebuild_canvas(-1);
    invalidate_selection();
    update_cursor(x, y);
}

bool ScreenDrawWindow::on_button_release(double ev_x, double ev_y) {
    if (is_typing_drag) {
        is_typing_drag = false;
    }

    if (is_box_selecting) {
        BBox cur_box = {std::min(box_select_start.x, box_select_end.x),
                        std::min(box_select_start.y, box_select_end.y),
                        std::max(box_select_start.x, box_select_end.x),
                        std::max(box_select_start.y, box_select_end.y)};
        is_box_selecting = false;
        box_select_initial.clear();
        invalidate_bbox(cur_box.padded(8.0));
        invalidate_selection();
        update_cursor(ev_x, ev_y);
        return true;
    }

    if (is_manipulating) {
        HandleId released_handle = active_handle;
        int clicked_single = pending_single_select_index;
        pending_single_select_index = -1;
        is_manipulating = false;
        active_handle = HandleId::None;
        canvas_skip_selection = false;
        if (drag_modified && !drag_orig_items.empty()) {
            std::vector<std::pair<int, DrawItem>> final_items;
            final_items.reserve(drag_orig_items.size());
            for (const auto& kv : drag_orig_items) {
                int idx = kv.first;
                if (idx >= 0 && idx < (int)strokes.size()) {
                    final_items.push_back({idx, strokes[idx]});
                    strokes[idx] = kv.second;
                }
            }
            push_undo_snapshot();
            for (const auto& kv : final_items) {
                strokes[kv.first] = kv.second;
            }
        } else if (!drag_modified && clicked_single >= 0) {
            set_single_selection(clicked_single);
        }
        drag_orig_items.clear();
        rebuild_canvas(-1);
        invalidate_selection();
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
            commit_stroke(item, true);
        } else {
            int hit_idx = find_topmost_selectable_at(ev_x, ev_y);
            if (hit_idx >= 0) {
                set_single_selection(hit_idx);
            }
        }
    }

    current_points.clear();
    update_cursor(ev_x, ev_y);
    return true;
}

void ScreenDrawWindow::commit_stroke(const DrawItem& item, bool select_after) {
    push_undo_snapshot();
    strokes.push_back(item);
    if (canvas_surface) {
        cairo_t* cr = cairo_create(canvas_surface);
        item.draw(cr);
        cairo_destroy(cr);
    }
    if (select_after && item.is_selectable()) {
        set_single_selection((int)strokes.size() - 1);
    } else {
        clear_selection();
    }
    invalidate_bbox(item.visual_bounds(true));
}

void ScreenDrawWindow::commit_text() {
    if (is_typing) {
        bool has_non_space = false;
        for (char c : typing_text) {
            if (c != ' ' && c != '\t' && c != '\n') {
                has_non_space = true;
                break;
            }
        }
        if (editing_index >= 0 && editing_index < (int)strokes.size()) {
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
                clear_selection();
            } else {
                bool changed = item.text != typing_text || item.color != text_color ||
                               item.font_size != text_size || item.bg_color != text_bg_color;
                if (changed) {
                    push_undo_snapshot();
                    item.text = typing_text;
                    item.color = text_color;
                    item.font_size = text_size;
                    item.bg_color = text_bg_color;
                    item.text_metrics_valid = false;
                    item.update_text_metrics();
                }
                set_single_selection(idx);
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

bool ScreenDrawWindow::on_motion(double x, double y) {
    double old_x = mouse_x;
    double old_y = mouse_y;
    mouse_x = x;
    mouse_y = y;

    if (is_typing && is_typing_drag) {
        typing_cursor_pos = cursor_index_from_x(typing_text, text_size, typing_x, x);
        start_cursor_blink();
        invalidate_full();
        return true;
    }

    if (is_box_selecting) {
        BBox prev_box = {std::min(box_select_start.x, box_select_end.x),
                         std::min(box_select_start.y, box_select_end.y),
                         std::max(box_select_start.x, box_select_end.x),
                         std::max(box_select_start.y, box_select_end.y)};
        BBox prev_sel = selection_visual_bounds();
        bool had_sel = has_selection();

        box_select_end = {x, y};
        BBox cur_box = {std::min(box_select_start.x, box_select_end.x),
                        std::min(box_select_start.y, box_select_end.y),
                        std::max(box_select_start.x, box_select_end.x),
                        std::max(box_select_start.y, box_select_end.y)};

        selected_indices = box_select_initial;
        if (cur_box.width() >= 2.0 || cur_box.height() >= 2.0) {
            for (int i = 0; i < (int)strokes.size(); ++i) {
                if (strokes[i].intersects_box(cur_box)) {
                    selected_indices.push_back(i);
                }
            }
        }
        sync_selected_index();

        BBox dirty = prev_box.united(cur_box).padded(8.0);
        if (had_sel) dirty = dirty.united(prev_sel);
        if (has_selection()) dirty = dirty.united(selection_visual_bounds());
        invalidate_bbox(dirty);
        return true;
    }

    if (is_manipulating && !selected_indices.empty()) {
        BBox before_box = selection_visual_bounds();
        double dx = x - drag_start_x;
        double dy = y - drag_start_y;
        if (std::hypot(dx, dy) > 0.5) {
            drag_modified = true;
            pending_single_select_index = -1;
        }
        if (active_handle == HandleId::Body) {
            for (const auto& kv : drag_orig_items) {
                int idx = kv.first;
                if (idx >= 0 && idx < (int)strokes.size()) {
                    strokes[idx].apply_move_from(kv.second, dx, dy);
                }
            }
        } else if (selected_index >= 0 && selected_index < (int)strokes.size()) {
            strokes[selected_index].apply_resize_from(drag_orig_item, active_handle, dx, dy);
        }
        BBox after_box = selection_visual_bounds();
        invalidate_bbox(before_box.united(after_box));
        return true;
    }

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

    std::string old_hover = hover_button;
    const ToolbarButton* hit = in_toolbar_zone(y) ? hit_toolbar(x, y) : nullptr;
    hover_button = hit ? hit->name : "";
    if (hover_button != old_hover) {
        invalidate_toolbar();
    }

    update_cursor(x, y);

    if (current_tool == ToolType::Eraser && !passthrough_mode) {
        double r = eraser_radius + 6.0;
        BBox b1 = {old_x - r, old_y - r, old_x + r, old_y + r};
        BBox b2 = {x - r, y - r, x + r, y + r};
        invalidate_bbox(b1.united(b2));
    }

    return true;
}

unsigned int ScreenDrawWindow::font_cursor_for(const std::string& name) {
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

void ScreenDrawWindow::set_cursor_by_name(const char* name) {
    if (current_cursor_name == name) return;
    current_cursor_name = name;
    if (!dpy || !window) return;
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

const char* ScreenDrawWindow::cursor_for_handle(HandleId hid) {
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

void ScreenDrawWindow::update_cursor(double x, double y) {
    if (in_toolbar_zone(y) || in_submenu_zone(x, y)) {
        set_cursor_by_name("pointer");
        return;
    }
    if (is_manipulating) {
        set_cursor_by_name(cursor_for_handle(active_handle));
        return;
    }
    if (is_box_selecting) {
        set_cursor_by_name("crosshair");
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
    } else if (selected_indices.size() > 1) {
        if (hit_selected_at(x, y) >= 0 || selection_raw_bounds().padded(4.0).contains(x, y)) {
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

void ScreenDrawWindow::start_cursor_blink() {
    blink_base_us = monotonic_us();
    blink_next_us = blink_base_us + 500000;
}

void ScreenDrawWindow::stop_cursor_blink() {
    blink_next_us = 0;
    blink_base_us = 0;
}

bool ScreenDrawWindow::on_key_press(XKeyEvent& kev) {
    char buf[128];
    KeySym keysym = NoSymbol;
    std::string text;
    if (xic) {
        Status status = 0;
        int len = Xutf8LookupString(xic, &kev, buf, sizeof(buf) - 1, &keysym, &status);
        if (status != XLookupKeySym && status != XLookupBoth) keysym = NoSymbol;
        if ((status == XLookupChars || status == XLookupBoth) && len > 0) text.assign(buf, len);
    } else {
        int len = XLookupString(&kev, buf, sizeof(buf) - 1, &keysym, nullptr);
        for (int i = 0; i < len; ++i) append_utf8(text, (unsigned char)buf[i]);
    }
    bool ctrl = (kev.state & ControlMask) != 0;
    const char* keyname = (keysym != NoSymbol) ? XKeysymToString(keysym) : nullptr;
    std::string kname = keyname ? keyname : "";
    return handle_key_action(kname, text, ctrl);
}

bool ScreenDrawWindow::handle_key_action(const std::string& kname, const std::string& text, bool ctrl) {
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
        } else if (has_selection()) {
            clear_selection();
        }
    } else if ((kname == "Delete" || kname == "BackSpace") && has_selection()) {
        push_undo_snapshot();
        for (int i = (int)selected_indices.size() - 1; i >= 0; --i) {
            int idx = selected_indices[i];
            if (idx >= 0 && idx < (int)strokes.size()) {
                strokes.erase(strokes.begin() + idx);
            }
        }
        selected_indices.clear();
        selected_index = -1;
        rebuild_canvas(-1);
        invalidate_full();
    } else if (ctrl && (kname == "a" || kname == "A")) {
        selected_indices.clear();
        for (int i = 0; i < (int)strokes.size(); ++i) {
            if (strokes[i].is_selectable()) {
                selected_indices.push_back(i);
            }
        }
        sync_selected_index();
        if (has_selection()) {
            current_tool = ToolType::Select;
            toolbar_cache_dirty = true;
        }
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

void ScreenDrawWindow::select_tool(ToolType t) {
    if (is_typing) commit_text();
    current_tool = t;
    submenu_open.clear();
    submenu_items.clear();
    toolbar_cache_dirty = true;
    if (t == ToolType::Eraser || t == ToolType::Pen) {
        clear_selection();
    }
    if (passthrough_mode) {
        exit_passthrough();
    }
    update_cursor(mouse_x, mouse_y);
    invalidate_full();
}

void ScreenDrawWindow::undo() {
    if (!undo_stack.empty()) {
        redo_stack.push_back(strokes);
        strokes = undo_stack.back();
        undo_stack.pop_back();
        selected_indices.clear();
        selected_index = -1;
        is_manipulating = false;
        is_box_selecting = false;
        canvas_skip_selection = false;
        rebuild_canvas(-1);
        invalidate_full();
    }
}

void ScreenDrawWindow::redo() {
    if (!redo_stack.empty()) {
        undo_stack.push_back(strokes);
        strokes = redo_stack.back();
        redo_stack.pop_back();
        selected_indices.clear();
        selected_index = -1;
        is_manipulating = false;
        is_box_selecting = false;
        canvas_skip_selection = false;
        rebuild_canvas(-1);
        invalidate_full();
    }
}

void ScreenDrawWindow::clear_canvas() {
    if (!strokes.empty()) {
        push_undo_snapshot();
        strokes.clear();
        selected_indices.clear();
        selected_index = -1;
        is_manipulating = false;
        is_box_selecting = false;
        canvas_skip_selection = false;
        rebuild_canvas(-1);
        invalidate_full();
    }
}

void ScreenDrawWindow::update_input_shape() {
    if (!dpy || !window || !has_shape_ext) return;

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

void ScreenDrawWindow::toggle_overlay() {
    if (passthrough_mode) {
        exit_passthrough();
    } else if (is_visible) {
        hide_overlay();
    } else {
        show_overlay();
    }
}

void ScreenDrawWindow::show_overlay() {
    is_visible = true;
    map_window();
    current_cursor_name.clear();
    update_cursor(mouse_x, mouse_y);
    invalidate_full();
    present_dirty();
    present_window(CurrentTime);
    start_focus_keepalive();
}

void ScreenDrawWindow::hide_overlay() {
    if (is_typing) commit_text();
    is_visible = false;
    has_focus = false;
    passthrough_mode = false;
    submenu_open.clear();
    submenu_items.clear();
    toolbar_cache_dirty = true;
    stop_focus_keepalive();
    update_input_shape();
    if (dpy && window) {
        XWithdrawWindow(dpy, window, screen_num);
    }
}

void ScreenDrawWindow::enter_passthrough() {
    if (is_typing) commit_text();
    passthrough_mode = true;
    clear_selection();
    is_manipulating = false;
    is_box_selecting = false;
    canvas_skip_selection = false;
    submenu_open.clear();
    submenu_items.clear();
    toolbar_cache_dirty = true;
    update_input_shape();
    invalidate_full();
}

void ScreenDrawWindow::reclaim_focus() {
    if (is_visible && !passthrough_mode) {
        present_window(CurrentTime);
    }
}

void ScreenDrawWindow::exit_passthrough() {
    passthrough_mode = false;
    toolbar_cache_dirty = true;
    update_input_shape();
    current_cursor_name.clear();
    update_cursor(mouse_x, mouse_y);
    invalidate_full();
    pending_reclaim_focus = true;
    start_focus_keepalive();
}

void ScreenDrawWindow::reassert_top() {
    if (is_visible) {
        set_keep_above();
        if (dpy && window) {
            XRaiseWindow(dpy, window);
        }
    }
}

void ScreenDrawWindow::on_focus_out() {
    if (is_visible) {
        set_keep_above();
        pending_reassert_top = true;
    }
}

void ScreenDrawWindow::start_focus_keepalive() {
    focus_keepalive_next_us = monotonic_us() + 50000;
}

void ScreenDrawWindow::stop_focus_keepalive() {
    focus_keepalive_next_us = 0;
}
