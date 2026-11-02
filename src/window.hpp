#pragma once

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <cairo.h>

// Xlib's `None` macro collides with HandleId::None.
static constexpr unsigned long XNone = None;
#undef None

#include "geometry.hpp"
#include "draw_item.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

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
    int selected_index = -1; // >= 0 when exactly one item is selected
    std::vector<int> selected_indices;
    bool is_manipulating = false; // moving or resizing selected item(s)
    HandleId active_handle = HandleId::None;
    double drag_start_x = 0.0;
    double drag_start_y = 0.0;
    DrawItem drag_orig_item;
    std::vector<std::pair<int, DrawItem>> drag_orig_items;
    bool drag_modified = false;
    int pending_single_select_index = -1;

    // Marquee (rubber-band) multi-selection state
    bool is_box_selecting = false;
    Point box_select_start = {0, 0};
    Point box_select_end = {0, 0};
    std::vector<int> box_select_initial;

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
    int canvas_skip_index = -1; // When actively editing an item, exclude it from static canvas_surface
    bool canvas_skip_selection = false; // When actively moving/resizing selected item(s), exclude them from static canvas_surface
    cairo_surface_t* toolbar_cache_surface = nullptr;
    bool toolbar_cache_dirty = true;
    int cached_toolbar_width = -1;

    // Timers & Cursors
    int64_t focus_keepalive_next_us = 0; // 0 = keepalive timer inactive
    std::string current_cursor_name;
    bool has_shape_ext = false;
    bool quit_requested = false;

    explicit ScreenDrawWindow(Display* display);
    // Headless constructor for unit and integration testing without an X11 server
    ScreenDrawWindow(int width, int height);
    ~ScreenDrawWindow();

    void push_undo_snapshot();
    void setup_window();
    void setup_input_method();

    // X11 Window Management Helpers
    void send_wm_message(Atom type, long l0, long l1, long l2, long l3);
    void set_keep_above();
    void present_window(Time t = CurrentTime);
    void map_window();

    // Back Buffer & Presentation
    void destroy_back_buffer();
    bool create_shm_back_buffer(int w, int h);
    void ensure_back_buffer(int w, int h);
    void add_dirty_rect(int x, int y, int w, int h);
    void present_dirty();

    // Event Dispatch, Idle Work & Timers
    void handle_event(XEvent& ev);
    void run_idle();
    void run_timers();
    int next_timeout_ms() const;

    // Toolbar & Submenu Layout & Rendering (implemented in toolbar.cpp)
    void build_toolbar();
    void layout_toolbar(int width);
    BBox toolbar_bounds() const;
    std::string tool_to_name(ToolType t) const;
    void draw_toolbar_cached(cairo_t* cr, int width);
    void draw_toolbar_tooltip(cairo_t* cr, int width);
    void draw_toolbar_direct(cairo_t* cr);
    void draw_button_icon(cairo_t* cr, const std::string& name, double x, double y, double w, double h, bool active);
    const ToolbarButton* find_button(const std::string& name) const;
    void draw_pen_submenu(cairo_t* cr, int width);
    void draw_eraser_submenu(cairo_t* cr, int width);
    void draw_text_submenu(cairo_t* cr, int width);
    const ToolbarButton* hit_toolbar(double x, double y) const;
    const SubmenuItem* hit_submenu(double x, double y) const;
    bool in_toolbar_zone(double y) const;
    bool in_submenu_zone(double x, double y) const;
    void handle_toolbar_click(const std::string& name);
    void handle_submenu_click(const SubmenuItem& item);

    // Canvas & Dirty Region Management
    void ensure_canvas(int w, int h);
    void rebuild_canvas(int skip_idx = -1);
    void invalidate_bbox(const BBox& b);
    void invalidate_toolbar();
    void invalidate_full();

    // Main Rendering
    void on_draw(cairo_t* cr, int w, int h, const cairo_rectangle_int_t& clip_rect);
    void draw_typing_text(cairo_t* cr);
    void draw_current_stroke(cairo_t* cr);

    // Selection & Hit Testing Helpers
    void sync_selected_index();
    bool has_selection() const;
    bool is_selected(int idx) const;
    BBox selection_raw_bounds() const;
    BBox selection_visual_bounds() const;
    void invalidate_selection();
    void clear_selection();
    void set_single_selection(int idx);
    void toggle_selection_item(int idx);
    int find_topmost_selectable_at(double x, double y, bool force_interior_bbox = false) const;
    int hit_selected_at(double x, double y) const;
    int find_topmost_text_at(double x, double y) const;
    BBox typing_visual_bounds() const;

    // Text Re-Editing
    void begin_edit_text(int idx, double click_x = -1.0);
    bool begin_edit_text_at(double x, double y);

    // Event Handlers
    bool on_button_press(double x, double y, Time time, unsigned int state = 0);
    void start_box_selection(double x, double y, bool additive);
    void start_manipulation(HandleId hid, double x, double y);
    bool on_button_release(double ev_x, double ev_y);
    void commit_stroke(const DrawItem& item, bool select_after);
    void commit_text();
    bool on_motion(double x, double y);

    // Cursor Management
    static unsigned int font_cursor_for(const std::string& name);
    void set_cursor_by_name(const char* name);
    static const char* cursor_for_handle(HandleId hid);
    void update_cursor(double x, double y);

    // Text Cursor Blink Timer
    void start_cursor_blink();
    void stop_cursor_blink();

    // Keyboard Handling & Document Actions
    bool on_key_press(XKeyEvent& kev);
    bool handle_key_action(const std::string& kname, const std::string& text, bool ctrl);
    void select_tool(ToolType t);
    void undo();
    void redo();
    void clear_canvas();

    // Passthrough & Overlay Visibility
    void update_input_shape();
    void toggle_overlay();
    void show_overlay();
    void hide_overlay();
    void enter_passthrough();
    void reclaim_focus();
    void exit_passthrough();
    void reassert_top();
    void on_focus_out();
    void start_focus_keepalive();
    void stop_focus_keepalive();
};
