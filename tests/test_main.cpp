#include "../src/geometry.hpp"
#include "../src/draw_item.hpp"
#include "../src/window.hpp"
#include "../src/ipc.hpp"

#include <unistd.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_tests_run = 0;
static int g_checks_passed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            std::abort();                                                           \
        }                                                                           \
        ++g_checks_passed;                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                       \
    do {                                                                            \
        double _va = (a);                                                           \
        double _vb = (b);                                                           \
        if (std::abs(_va - _vb) > (eps)) {                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s (%f) != %s (%f)\n",              \
                         __FILE__, __LINE__, #a, _va, #b, _vb);                     \
            std::abort();                                                           \
        }                                                                           \
        ++g_checks_passed;                                                          \
    } while (0)

#define RUN_TEST(fn)                                                                \
    do {                                                                            \
        ++g_tests_run;                                                              \
        fn();                                                                       \
        std::printf("  [PASS] %s\n", #fn);                                          \
    } while (0)

// ─── 1. Geometry & Color Tests ───────────────────────────────────────────────

static void test_hex_to_rgba() {
    RGBA c1 = hex_to_rgba("#FF3B30", 0.8);
    CHECK_NEAR(c1.r, 1.0, 1e-3);
    CHECK_NEAR(c1.g, 0x3B / 255.0, 1e-3);
    CHECK_NEAR(c1.b, 0x30 / 255.0, 1e-3);
    CHECK_NEAR(c1.a, 0.8, 1e-6);

    RGBA c2 = hex_to_rgba("007AFF");
    CHECK_NEAR(c2.r, 0.0, 1e-3);
    CHECK_NEAR(c2.g, 0x7A / 255.0, 1e-3);
    CHECK_NEAR(c2.b, 1.0, 1e-3);
    CHECK_NEAR(c2.a, 1.0, 1e-6);

    RGBA fallback = hex_to_rgba("");
    CHECK_NEAR(fallback.r, 1.0, 1e-6);
    CHECK_NEAR(fallback.g, 0.0, 1e-6);
}

static void test_bbox_operations() {
    BBox b1{10, 20, 50, 80};
    CHECK(b1.valid());
    CHECK_NEAR(b1.width(), 40.0, 1e-6);
    CHECK_NEAR(b1.height(), 60.0, 1e-6);
    CHECK(b1.contains(10, 20));
    CHECK(b1.contains(30, 50));
    CHECK(!b1.contains(9, 20));

    BBox bp = b1.padded(5.0);
    CHECK_NEAR(bp.x1, 5.0, 1e-6);
    CHECK_NEAR(bp.y1, 15.0, 1e-6);
    CHECK_NEAR(bp.x2, 55.0, 1e-6);
    CHECK_NEAR(bp.y2, 85.0, 1e-6);

    BBox b2{40, 10, 100, 60};
    BBox u = b1.united(b2);
    CHECK_NEAR(u.x1, 10.0, 1e-6);
    CHECK_NEAR(u.y1, 10.0, 1e-6);
    CHECK_NEAR(u.x2, 100.0, 1e-6);
    CHECK_NEAR(u.y2, 80.0, 1e-6);

    BBox invalid{0, 0, -1, -1};
    CHECK(!invalid.valid());
    BBox u_inv = invalid.united(b1);
    CHECK_NEAR(u_inv.x1, 10.0, 1e-6);
    CHECK_NEAR(u_inv.x2, 50.0, 1e-6);

    CHECK(b1.intersects(45, 75, 20, 20));
    CHECK(!b1.intersects(60, 90, 20, 20));
}

static void test_segment_math() {
    CHECK_NEAR(point_to_segment_dist(5, 3, 0, 0, 10, 0), 3.0, 1e-6);
    CHECK_NEAR(point_to_segment_dist(-3, 4, 0, 0, 10, 0), 5.0, 1e-6);
    CHECK_NEAR(point_to_segment_dist(5, 5, 2, 2, 2, 2), std::hypot(3.0, 3.0), 1e-6);

    BBox box{10, 10, 30, 30};
    // Segment completely inside box
    CHECK(segment_intersects_box(15, 15, 25, 25, box));
    // Segment crossing box diagonally
    CHECK(segment_intersects_box(0, 0, 40, 40, box));
    // Segment whose AABB overlaps box, but passes above-right of the box
    CHECK(!segment_intersects_box(0, 35, 35, 0, {0, 0, 10, 10}));
    // Disjoint AABB
    CHECK(!segment_intersects_box(0, 0, 5, 5, box));
}

static void test_utf8_and_cursor_index() {
    std::string s;
    append_utf8(s, 'A');
    append_utf8(s, 0x00E9); // é (2 bytes)
    append_utf8(s, 0x20AC); // € (3 bytes)
    append_utf8(s, 0x1F600); // 😀 (4 bytes)
    CHECK(s.size() == 1 + 2 + 3 + 4);

    std::string word = "Hello World";
    CHECK(cursor_index_from_x(word, 24.0, 100.0, 50.0) == 0);
    CHECK(cursor_index_from_x(word, 24.0, 100.0, 5000.0) == word.size());
    size_t mid = cursor_index_from_x(word, 24.0, 100.0, 150.0);
    CHECK(mid > 0 && mid < word.size());
}

// ─── 2. DrawItem Tests ───────────────────────────────────────────────────────

static void test_draw_item_shapes_and_handles() {
    DrawItem rect;
    rect.kind = StrokeKind::Shape;
    rect.shape_type = ToolType::Rect;
    rect.x1 = 100;
    rect.y1 = 100;
    rect.x2 = 200;
    rect.y2 = 180;
    rect.width = 4.0;

    // Edge hit vs hollow interior hit
    CHECK(rect.hit_test(100, 140, false));
    CHECK(!rect.hit_test(150, 140, false));
    CHECK(rect.hit_test(150, 140, true));

    // Handles: 8 handles for Rect
    auto handles = rect.get_handles();
    CHECK(handles.size() == 8);
    CHECK(rect.hit_handle(100, 100) == HandleId::NW);
    CHECK(rect.hit_handle(200, 180) == HandleId::SE);
    CHECK(rect.hit_handle(150, 100) == HandleId::N);

    // Move transform
    DrawItem orig = rect;
    rect.apply_move_from(orig, 25.0, -15.0);
    CHECK_NEAR(rect.x1, 125.0, 1e-6);
    CHECK_NEAR(rect.y1, 85.0, 1e-6);
    CHECK_NEAR(rect.x2, 225.0, 1e-6);
    CHECK_NEAR(rect.y2, 165.0, 1e-6);

    // Resize transform
    rect.apply_resize_from(orig, HandleId::SE, 30.0, 20.0);
    CHECK_NEAR(rect.x1, 100.0, 1e-6);
    CHECK_NEAR(rect.y1, 100.0, 1e-6);
    CHECK_NEAR(rect.x2, 230.0, 1e-6);
    CHECK_NEAR(rect.y2, 200.0, 1e-6);

    // Line / Arrow has 2 endpoint handles
    DrawItem arrow;
    arrow.kind = StrokeKind::Shape;
    arrow.shape_type = ToolType::Arrow;
    arrow.x1 = 50;
    arrow.y1 = 50;
    arrow.x2 = 150;
    arrow.y2 = 50;
    CHECK(arrow.get_handles().size() == 2);
    CHECK(arrow.hit_handle(50, 50) == HandleId::LineStart);
    CHECK(arrow.hit_handle(150, 50) == HandleId::LineEnd);
    CHECK(arrow.intersects_box({90, 45, 110, 55}));
    CHECK(!arrow.intersects_box({90, 80, 110, 100}));
}

static void test_draw_item_freehand_and_text() {
    DrawItem pen;
    pen.kind = StrokeKind::Freehand;
    pen.points = {{10, 10}, {30, 30}, {60, 20}};
    pen.width = 5.0;
    CHECK(pen.is_selectable());
    CHECK(pen.hit_test(20, 20, false));
    CHECK(pen.intersects_box({15, 15, 25, 25}));

    DrawItem orig_pen = pen;
    pen.apply_move_from(orig_pen, 10.0, 5.0);
    CHECK_NEAR(pen.points[0].x, 20.0, 1e-6);
    CHECK_NEAR(pen.points[0].y, 15.0, 1e-6);

    DrawItem eraser = pen;
    eraser.is_eraser = true;
    CHECK(!eraser.is_selectable());
    CHECK(!eraser.hit_test(20, 20, true));
    CHECK(!eraser.intersects_box({0, 0, 100, 100}));

    DrawItem txt;
    txt.kind = StrokeKind::Text;
    txt.text = "Annotation";
    txt.text_x = 200;
    txt.text_y = 200;
    txt.font_size = 24.0;
    txt.update_text_metrics();
    BBox tb = txt.raw_bounds();
    CHECK(tb.valid());
    CHECK(txt.get_handles().size() == 4);
    CHECK(txt.hit_test((tb.x1 + tb.x2) * 0.5, (tb.y1 + tb.y2) * 0.5, false));
}

// ─── 3. ScreenDrawWindow Multi-Select & Move Tests ───────────────────────────

static void draw_rect_on(ScreenDrawWindow& win, double x1, double y1, double x2, double y2) {
    win.select_tool(ToolType::Rect);
    win.on_button_press(x1, y1, 100);
    win.on_motion(x2, y2);
    win.on_button_release(x2, y2);
}

static void test_window_marquee_select_and_group_move() {
    ScreenDrawWindow win(1920, 1080);

    // Draw 3 rectangles at distinct positions
    draw_rect_on(win, 100, 100, 160, 160);
    draw_rect_on(win, 200, 100, 260, 160);
    draw_rect_on(win, 500, 500, 580, 580);
    CHECK(win.strokes.size() == 3);

    // Switch to Select tool and marquee-select the first two rectangles
    win.select_tool(ToolType::Select);
    win.on_button_press(80, 80, 200, 0);
    win.on_motion(280, 180);
    CHECK(win.is_box_selecting);
    CHECK(win.selected_indices.size() == 2);
    CHECK(win.is_selected(0));
    CHECK(win.is_selected(1));
    CHECK(!win.is_selected(2));
    CHECK(win.selected_index == -1); // -1 when multiple items are selected
    win.on_button_release(280, 180);
    CHECK(!win.is_box_selecting);
    CHECK(win.selected_indices.size() == 2);

    // Drag the first selected rectangle by (+40, +50) -> both selected rectangles should move together
    win.on_button_press(130, 130, 300, 0);
    CHECK(win.is_manipulating);
    win.on_motion(170, 180);
    win.on_button_release(170, 180);
    CHECK(!win.is_manipulating);

    // Verify both item 0 and item 1 moved by (+40, +50), while item 2 stayed untouched
    CHECK_NEAR(win.strokes[0].x1, 140.0, 1e-6);
    CHECK_NEAR(win.strokes[0].y1, 150.0, 1e-6);
    CHECK_NEAR(win.strokes[0].x2, 200.0, 1e-6);
    CHECK_NEAR(win.strokes[0].y2, 210.0, 1e-6);

    CHECK_NEAR(win.strokes[1].x1, 240.0, 1e-6);
    CHECK_NEAR(win.strokes[1].y1, 150.0, 1e-6);
    CHECK_NEAR(win.strokes[1].x2, 300.0, 1e-6);
    CHECK_NEAR(win.strokes[1].y2, 210.0, 1e-6);

    CHECK_NEAR(win.strokes[2].x1, 500.0, 1e-6);
    CHECK_NEAR(win.strokes[2].y1, 500.0, 1e-6);

    // Single-step Undo restores both moved items at once
    win.handle_key_action("z", "z", true);
    CHECK_NEAR(win.strokes[0].x1, 100.0, 1e-6);
    CHECK_NEAR(win.strokes[0].y1, 100.0, 1e-6);
    CHECK_NEAR(win.strokes[1].x1, 200.0, 1e-6);
    CHECK_NEAR(win.strokes[1].y1, 100.0, 1e-6);

    // Single-step Redo re-applies the group move
    win.handle_key_action("y", "y", true);
    CHECK_NEAR(win.strokes[0].x1, 140.0, 1e-6);
    CHECK_NEAR(win.strokes[1].x1, 240.0, 1e-6);
}

static void test_window_shift_click_ctrl_a_and_batch_delete() {
    ScreenDrawWindow win(1920, 1080);

    draw_rect_on(win, 100, 100, 150, 150);
    draw_rect_on(win, 250, 100, 300, 150);
    draw_rect_on(win, 400, 100, 450, 150);
    CHECK(win.strokes.size() == 3);

    win.select_tool(ToolType::Select);
    // Select item 0
    win.on_button_press(125, 125, 100, 0);
    win.on_button_release(125, 125);
    CHECK(win.selected_indices.size() == 1);
    CHECK(win.selected_index == 0);

    // Shift+Click item 2 to add to selection
    win.on_button_press(425, 125, 200, ShiftMask);
    win.on_button_release(425, 125);
    CHECK(win.selected_indices.size() == 2);
    CHECK(win.is_selected(0));
    CHECK(win.is_selected(2));

    // Batch color change on selected items
    SubmenuItem color_item{"color", "#007AFF", 0, 0, 0, 26, 26};
    win.handle_submenu_click(color_item);
    CHECK(win.strokes[0].color == "#007AFF");
    CHECK(win.strokes[1].color != "#007AFF");
    CHECK(win.strokes[2].color == "#007AFF");

    // Shift+Click item 0 again to remove it from selection
    win.on_button_press(125, 125, 300, ShiftMask);
    win.on_button_release(125, 125);
    CHECK(win.selected_indices.size() == 1);
    CHECK(win.selected_index == 2);

    // Ctrl+A selects all 3 items
    win.handle_key_action("a", "a", true);
    CHECK(win.selected_indices.size() == 3);

    // Plain click (no drag) on item 1 narrows selection to item 1
    win.on_button_press(275, 125, 400, 0);
    win.on_button_release(275, 125);
    CHECK(win.selected_indices.size() == 1);
    CHECK(win.selected_index == 1);

    // Select all and delete with Delete key
    win.handle_key_action("a", "a", true);
    CHECK(win.selected_indices.size() == 3);
    win.handle_key_action("Delete", "", false);
    CHECK(win.strokes.empty());
    CHECK(!win.has_selection());

    // Undo restores all 3 deleted items
    win.handle_key_action("z", "z", true);
    CHECK(win.strokes.size() == 3);
}

static void test_window_text_editing_and_navigation() {
    ScreenDrawWindow win(1920, 1080);

    win.select_tool(ToolType::Text);
    win.on_button_press(300, 300, 100, 0);
    CHECK(win.is_typing);

    win.handle_key_action("H", "H", false);
    win.handle_key_action("e", "e", false);
    win.handle_key_action("l", "l", false);
    win.handle_key_action("l", "l", false);
    win.handle_key_action("o", "o", false);
    win.handle_key_action("space", " ", false);
    win.handle_key_action("W", "W", false);
    win.handle_key_action("o", "o", false);
    win.handle_key_action("r", "r", false);
    win.handle_key_action("l", "l", false);
    win.handle_key_action("d", "d", false);
    CHECK(win.typing_text == "Hello World");
    CHECK(win.typing_cursor_pos == 11);

    // Ctrl+BackSpace deletes "World"
    win.handle_key_action("BackSpace", "", true);
    CHECK(win.typing_text == "Hello ");

    // Home moves cursor to 0, Ctrl+Delete deletes "Hello"
    win.handle_key_action("Home", "", false);
    CHECK(win.typing_cursor_pos == 0);
    win.handle_key_action("Delete", "", true);
    CHECK(win.typing_text == " ");

    // Type new text and commit via Return
    win.handle_key_action("Delete", "", false);
    CHECK(win.typing_text.empty());
    for (char c : std::string("Screen Draw")) {
        std::string s(1, c);
        win.handle_key_action(s, s, false);
    }
    win.handle_key_action("Return", "\r", false);
    CHECK(!win.is_typing);
    CHECK(win.strokes.size() == 1);
    CHECK(win.strokes[0].kind == StrokeKind::Text);
    CHECK(win.strokes[0].text == "Screen Draw");
    CHECK(win.selected_index == 0);

    // Re-open selected text with F2, move cursor Left 4 times, insert "2.0 "
    win.handle_key_action("F2", "", false);
    CHECK(win.is_typing);
    CHECK(win.editing_index == 0);
    for (int i = 0; i < 4; ++i) win.handle_key_action("Left", "", false);
    for (char c : std::string("2.0 ")) {
        std::string s(1, c);
        win.handle_key_action(s, s, false);
    }
    win.handle_key_action("Return", "\r", false);
    CHECK(win.strokes[0].text == "Screen 2.0 Draw");
}

static void test_window_rendering_and_toolbar() {
    ScreenDrawWindow win(1920, 1080);
    draw_rect_on(win, 100, 100, 200, 200);
    draw_rect_on(win, 250, 100, 350, 200);
    win.handle_key_action("a", "a", true);

    // Render full frame to an offscreen surface to exercise on_draw with multi-selection & submenus
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 1080);
    cairo_t* cr = cairo_create(surf);
    cairo_rectangle_int_t clip = {0, 0, 1920, 1080};

    win.submenu_open = "pen_options";
    win.on_draw(cr, 1920, 1080, clip);
    CHECK(!win.submenu_items.empty());

    win.submenu_open = "eraser_options";
    win.on_draw(cr, 1920, 1080, clip);
    CHECK(!win.submenu_items.empty());

    win.submenu_open = "text_options";
    win.on_draw(cr, 1920, 1080, clip);
    CHECK(!win.submenu_items.empty());

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

// ─── 4. IPC Socket Tests ─────────────────────────────────────────────────────

static void test_ipc_socket_commands() {
    CHECK(shell_quote("simple") == "'simple'");
    CHECK(shell_quote("a'b") == "'a'\\''b'");

    std::string sock = "/tmp/screen-draw-test-" + std::to_string((unsigned long)getpid()) + ".sock";
    int listen_fd = create_ipc_listener(sock);
    CHECK(listen_fd >= 0);

    ScreenDrawWindow win(800, 600);
    CHECK(!win.is_visible);

    CHECK(send_ipc_command("toggle", sock));
    handle_ipc_client(listen_fd, win);
    CHECK(win.is_visible);

    CHECK(send_ipc_command("hide", sock));
    handle_ipc_client(listen_fd, win);
    CHECK(!win.is_visible);

    CHECK(send_ipc_command("show", sock));
    handle_ipc_client(listen_fd, win);
    CHECK(win.is_visible);

    CHECK(send_ipc_command("quit", sock));
    handle_ipc_client(listen_fd, win);
    CHECK(win.quit_requested);

    close(listen_fd);
    unlink(sock.c_str());
}

int main() {
    std::printf("Running Screen Draw test suite...\n");
    RUN_TEST(test_hex_to_rgba);
    RUN_TEST(test_bbox_operations);
    RUN_TEST(test_segment_math);
    RUN_TEST(test_utf8_and_cursor_index);
    RUN_TEST(test_draw_item_shapes_and_handles);
    RUN_TEST(test_draw_item_freehand_and_text);
    RUN_TEST(test_window_marquee_select_and_group_move);
    RUN_TEST(test_window_shift_click_ctrl_a_and_batch_delete);
    RUN_TEST(test_window_text_editing_and_navigation);
    RUN_TEST(test_window_rendering_and_toolbar);
    RUN_TEST(test_ipc_socket_commands);
    std::printf("All %d test suites (%d assertions) passed!\n", g_tests_run, g_checks_passed);
    return 0;
}
