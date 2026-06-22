#!/usr/bin/env python3
"""
Screen Draw — A Gromit-MPX inspired screen annotation tool.

Draw over your entire screen with pens, erasers, and shapes.
Toggle the overlay with F9.
"""

import sys
import math
import cairo
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")

from gi.repository import Gtk, Gdk, GLib, GdkPixbuf

# Try to import gtk-layer-shell for Wayland overlay support
HAS_LAYER_SHELL = False
try:
    gi.require_version("GtkLayerShell", "0.1")
    from gi.repository import GtkLayerShell
    HAS_LAYER_SHELL = True
except (ValueError, ImportError):
    print("[warn] gtk-layer-shell not found. Falling back to X11-style overlay.")

# Try to import Keybinder for global hotkeys
HAS_KEYBINDER = False
try:
    gi.require_version("Keybinder", "3.0")
    from gi.repository import Keybinder
    HAS_KEYBINDER = True
except (ValueError, ImportError):
    print("[warn] Keybinder not found. Global hotkey (F9) may not work when overlay is hidden.")


# ─── Constants ────────────────────────────────────────────────────────────────

TOGGLE_KEY = "F9"

TOOL_PEN = "pen"
TOOL_ERASER = "eraser"
TOOL_LINE = "line"
TOOL_RECT = "rect"
TOOL_CIRCLE = "circle"
TOOL_ARROW = "arrow"

COLOR_PRESETS = [
    ("#FF3B30", "Red"),
    ("#FF9500", "Orange"),
    ("#FFCC00", "Yellow"),
    ("#34C759", "Green"),
    ("#007AFF", "Blue"),
    ("#AF52DE", "Purple"),
    ("#FF2D55", "Pink"),
    ("#FFFFFF", "White"),
    ("#000000", "Black"),
]

STROKE_PRESETS = [2, 4, 6, 8, 12, 16, 24]

TOOLBAR_HEIGHT = 52
TOOLBAR_PADDING = 6
TOOLBAR_RADIUS = 14
TOOLBAR_GAP = 6


# ─── Helpers ──────────────────────────────────────────────────────────────────

def hex_to_rgba(hex_color, alpha=1.0):
    """Convert hex color string to (r, g, b, a) tuple."""
    hex_color = hex_color.lstrip("#")
    r = int(hex_color[0:2], 16) / 255.0
    g = int(hex_color[2:4], 16) / 255.0
    b = int(hex_color[4:6], 16) / 255.0
    return (r, g, b, alpha)


def point_distance(x1, y1, x2, y2):
    return math.sqrt((x2 - x1) ** 2 + (y2 - y1) ** 2)


# ─── Stroke Data Classes ─────────────────────────────────────────────────────

class FreehandStroke:
    """A freehand pen/eraser stroke."""

    def __init__(self, points, color, width, is_eraser=False):
        self.points = points  # list of (x, y)
        self.color = color
        self.width = width
        self.is_eraser = is_eraser

    def draw(self, cr):
        if len(self.points) < 2:
            return
        if self.is_eraser:
            cr.set_operator(cairo.OPERATOR_CLEAR)
        else:
            cr.set_operator(cairo.OPERATOR_OVER)
            r, g, b, a = hex_to_rgba(self.color)
            cr.set_source_rgba(r, g, b, a)
        cr.set_line_width(self.width)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)

        cr.move_to(self.points[0][0], self.points[0][1])
        for px, py in self.points[1:]:
            cr.line_to(px, py)
        cr.stroke()


class ShapeStroke:
    """A geometric shape stroke (line, rect, circle, arrow)."""

    def __init__(self, shape_type, x1, y1, x2, y2, color, width):
        self.shape_type = shape_type
        self.x1, self.y1 = x1, y1
        self.x2, self.y2 = x2, y2
        self.color = color
        self.width = width

    def draw(self, cr):
        r, g, b, a = hex_to_rgba(self.color)
        cr.set_source_rgba(r, g, b, a)
        cr.set_line_width(self.width)
        cr.set_line_cap(cairo.LINE_CAP_ROUND)
        cr.set_line_join(cairo.LINE_JOIN_ROUND)
        cr.set_operator(cairo.OPERATOR_OVER)

        if self.shape_type == TOOL_LINE:
            cr.move_to(self.x1, self.y1)
            cr.line_to(self.x2, self.y2)
            cr.stroke()

        elif self.shape_type == TOOL_RECT:
            x = min(self.x1, self.x2)
            y = min(self.y1, self.y2)
            w = abs(self.x2 - self.x1)
            h = abs(self.y2 - self.y1)
            cr.rectangle(x, y, w, h)
            cr.stroke()

        elif self.shape_type == TOOL_CIRCLE:
            cx = (self.x1 + self.x2) / 2
            cy = (self.y1 + self.y2) / 2
            rx = abs(self.x2 - self.x1) / 2
            ry = abs(self.y2 - self.y1) / 2
            cr.save()
            cr.translate(cx, cy)
            if rx > 0 and ry > 0:
                cr.scale(1.0, ry / rx)
                cr.arc(0, 0, rx, 0, 2 * math.pi)
            cr.restore()
            cr.stroke()

        elif self.shape_type == TOOL_ARROW:
            cr.move_to(self.x1, self.y1)
            cr.line_to(self.x2, self.y2)
            cr.stroke()
            # Arrowhead
            angle = math.atan2(self.y2 - self.y1, self.x2 - self.x1)
            head_len = max(self.width * 3, 18)
            head_angle = math.pi / 6
            lx = self.x2 - head_len * math.cos(angle - head_angle)
            ly = self.y2 - head_len * math.sin(angle - head_angle)
            rx = self.x2 - head_len * math.cos(angle + head_angle)
            ry = self.y2 - head_len * math.sin(angle + head_angle)
            cr.move_to(self.x2, self.y2)
            cr.line_to(lx, ly)
            cr.move_to(self.x2, self.y2)
            cr.line_to(rx, ry)
            cr.stroke()


# ─── Toolbar Button Definitions ──────────────────────────────────────────────

class ToolButton:
    """Represents a clickable button in the toolbar."""

    def __init__(self, name, icon_draw_func, x=0, y=0, w=40, h=40,
                 is_active=False, tooltip="", has_submenu=False):
        self.name = name
        self.icon_draw_func = icon_draw_func
        self.x = x
        self.y = y
        self.w = w
        self.h = h
        self.is_active = is_active
        self.tooltip = tooltip
        self.has_submenu = has_submenu
        self.hover = False


# ─── Main Application Window ─────────────────────────────────────────────────

class ScreenDrawWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="Screen Draw")

        # ── Drawing state ──
        self.strokes = []       # committed strokes
        self.redo_stack = []    # undone strokes
        self.current_points = []
        self.is_drawing = False
        self.shape_start = None
        self.shape_end = None

        # ── Tool state ──
        self.current_tool = TOOL_PEN
        self.current_color = "#FF3B30"
        self.stroke_width = 4
        self.is_visible = False

        # ── Submenu state ──
        self.submenu_open = None   # "pen_options" | "shapes" | None
        self.submenu_buttons = []  # dynamic sub-buttons

        # ── Canvas surface (persistent drawing buffer) ──
        self.canvas_surface = None

        # ── Setup the window ──
        self._setup_window()
        self._build_toolbar_buttons()
        self._connect_events()

        # ── Register global hotkey ──
        if HAS_KEYBINDER:
            Keybinder.init()
            Keybinder.bind(TOGGLE_KEY, self._on_global_toggle, None)

    # ── Window Setup ──────────────────────────────────────────────────────

    def _setup_window(self):
        screen = Gdk.Screen.get_default()
        visual = screen.get_rgba_visual()
        if visual:
            self.set_visual(visual)

        self.set_app_paintable(True)
        self.set_decorated(False)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_keep_above(True)
        self.set_accept_focus(True)

        if HAS_LAYER_SHELL:
            GtkLayerShell.init_for_window(self)
            GtkLayerShell.set_layer(self, GtkLayerShell.Layer.OVERLAY)
            GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.TOP, True)
            GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.BOTTOM, True)
            GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.LEFT, True)
            GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.RIGHT, True)
            GtkLayerShell.set_exclusive_zone(self, -1)  # overlay mode
            GtkLayerShell.set_keyboard_mode(
                self, GtkLayerShell.KeyboardMode.ON_DEMAND
            )
        else:
            # X11 fallback
            monitor = screen.get_display().get_primary_monitor()
            if monitor is None:
                monitor = screen.get_display().get_monitor(0)
            geom = monitor.get_geometry()
            self.move(geom.x, geom.y)
            self.set_default_size(geom.width, geom.height)
            self.fullscreen()
            self.set_type_hint(Gdk.WindowTypeHint.DOCK)

        # Enable input events
        self.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK
            | Gdk.EventMask.BUTTON_RELEASE_MASK
            | Gdk.EventMask.POINTER_MOTION_MASK
            | Gdk.EventMask.KEY_PRESS_MASK
            | Gdk.EventMask.POINTER_MOTION_HINT_MASK
        )

    # ── Event Connections ─────────────────────────────────────────────────

    def _connect_events(self):
        self.connect("draw", self._on_draw)
        self.connect("button-press-event", self._on_button_press)
        self.connect("button-release-event", self._on_button_release)
        self.connect("motion-notify-event", self._on_motion)
        self.connect("key-press-event", self._on_key_press)
        self.connect("destroy", self._on_destroy)
        self.connect("configure-event", self._on_configure)

    # ── Toolbar Button Definitions ────────────────────────────────────────

    def _build_toolbar_buttons(self):
        self.toolbar_buttons = []

        # Pen
        btn_pen = ToolButton(
            "pen", self._draw_pen_icon, tooltip="Pen (P)",
            is_active=True, has_submenu=True
        )
        self.toolbar_buttons.append(btn_pen)

        # Eraser
        btn_eraser = ToolButton(
            "eraser", self._draw_eraser_icon, tooltip="Eraser (E)"
        )
        self.toolbar_buttons.append(btn_eraser)

        # Separator
        btn_sep = ToolButton("sep", None, w=2, tooltip="")
        self.toolbar_buttons.append(btn_sep)

        # Line
        btn_line = ToolButton(
            "line", self._draw_line_icon, tooltip="Line (L)"
        )
        self.toolbar_buttons.append(btn_line)

        # Rectangle
        btn_rect = ToolButton(
            "rect", self._draw_rect_icon, tooltip="Rectangle (R)"
        )
        self.toolbar_buttons.append(btn_rect)

        # Circle
        btn_circle = ToolButton(
            "circle", self._draw_circle_icon, tooltip="Circle (C)"
        )
        self.toolbar_buttons.append(btn_circle)

        # Arrow
        btn_arrow = ToolButton(
            "arrow", self._draw_arrow_icon, tooltip="Arrow (A)"
        )
        self.toolbar_buttons.append(btn_arrow)

        # Separator
        btn_sep2 = ToolButton("sep2", None, w=2, tooltip="")
        self.toolbar_buttons.append(btn_sep2)

        # Undo
        btn_undo = ToolButton(
            "undo", self._draw_undo_icon, tooltip="Undo (Ctrl+Z)"
        )
        self.toolbar_buttons.append(btn_undo)

        # Redo
        btn_redo = ToolButton(
            "redo", self._draw_redo_icon, tooltip="Redo (Ctrl+Y)"
        )
        self.toolbar_buttons.append(btn_redo)

        # Clear
        btn_clear = ToolButton(
            "clear", self._draw_clear_icon, tooltip="Clear All (Ctrl+C)"
        )
        self.toolbar_buttons.append(btn_clear)

        # Close (hide)
        btn_close = ToolButton(
            "close", self._draw_close_icon, tooltip="Hide (Esc)"
        )
        self.toolbar_buttons.append(btn_close)

    def _layout_toolbar_buttons(self, width):
        """Position toolbar buttons centered at the top of the screen."""
        btn_size = TOOLBAR_HEIGHT - TOOLBAR_PADDING * 2
        total_w = 0
        for btn in self.toolbar_buttons:
            if btn.name.startswith("sep"):
                total_w += 2 + TOOLBAR_GAP
            else:
                total_w += btn_size + TOOLBAR_GAP

        start_x = (width - total_w) / 2
        cur_x = start_x
        for btn in self.toolbar_buttons:
            if btn.name.startswith("sep"):
                btn.x = cur_x
                btn.y = TOOLBAR_PADDING + 4
                btn.w = 2
                btn.h = btn_size - 8
                cur_x += 2 + TOOLBAR_GAP
            else:
                btn.x = cur_x
                btn.y = TOOLBAR_PADDING
                btn.w = btn_size
                btn.h = btn_size
                cur_x += btn_size + TOOLBAR_GAP

    # ── Canvas Surface Management ─────────────────────────────────────────

    def _ensure_canvas(self, width, height):
        """Create or resize the persistent canvas surface."""
        if (self.canvas_surface is None
                or self.canvas_surface.get_width() != width
                or self.canvas_surface.get_height() != height):
            new_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
            if self.canvas_surface is not None:
                cr = cairo.Context(new_surface)
                cr.set_source_surface(self.canvas_surface, 0, 0)
                cr.paint()
            self.canvas_surface = new_surface

    def _redraw_canvas(self):
        """Rebuild the canvas from strokes."""
        if self.canvas_surface is None:
            return
        w = self.canvas_surface.get_width()
        h = self.canvas_surface.get_height()
        self.canvas_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
        cr = cairo.Context(self.canvas_surface)
        for stroke in self.strokes:
            stroke.draw(cr)

    def _on_configure(self, widget, event):
        self._ensure_canvas(event.width, event.height)

    # ── Draw Handler ──────────────────────────────────────────────────────

    def _on_draw(self, widget, cr):
        alloc = self.get_allocation()
        width, height = alloc.width, alloc.height

        self._ensure_canvas(width, height)
        self._layout_toolbar_buttons(width)

        # Clear the window (fully transparent)
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_operator(cairo.OPERATOR_OVER)

        # Semi-transparent overlay tint so user knows drawing mode is on
        cr.set_source_rgba(0, 0, 0, 0.05)
        cr.paint()

        # Paint the committed strokes
        cr.set_source_surface(self.canvas_surface, 0, 0)
        cr.paint()

        # Paint the in-progress stroke / shape
        if self.is_drawing:
            if self.current_tool == TOOL_PEN and len(self.current_points) >= 2:
                temp = FreehandStroke(
                    self.current_points, self.current_color, self.stroke_width
                )
                temp.draw(cr)
            elif self.current_tool == TOOL_ERASER and len(self.current_points) >= 2:
                temp = FreehandStroke(
                    self.current_points, "#000", self.stroke_width * 3, is_eraser=True
                )
                temp.draw(cr)
            elif (self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW)
                  and self.shape_start and self.shape_end):
                temp = ShapeStroke(
                    self.current_tool,
                    *self.shape_start, *self.shape_end,
                    self.current_color, self.stroke_width
                )
                temp.draw(cr)

        # ── Draw Toolbar ──
        self._draw_toolbar(cr, width)

        # ── Draw Submenu ──
        if self.submenu_open == "pen_options":
            self._draw_pen_options_submenu(cr, width)

    # ── Toolbar Drawing ───────────────────────────────────────────────────

    def _draw_toolbar(self, cr, width):
        """Draw the floating toolbar at the top-center."""
        # Background pill
        total_w = 0
        for btn in self.toolbar_buttons:
            total_w += btn.w + TOOLBAR_GAP
        bar_x = (width - total_w) / 2 - 12
        bar_w = total_w + 24
        bar_y = 0
        bar_h = TOOLBAR_HEIGHT

        # Rounded rect background
        r = TOOLBAR_RADIUS
        cr.new_sub_path()
        cr.arc(bar_x + bar_w - r, bar_y + r, r, -math.pi / 2, 0)
        cr.arc(bar_x + bar_w - r, bar_y + bar_h - r, r, 0, math.pi / 2)
        cr.arc(bar_x + r, bar_y + bar_h - r, r, math.pi / 2, math.pi)
        cr.arc(bar_x + r, bar_y + r, r, math.pi, 3 * math.pi / 2)
        cr.close_path()
        cr.set_source_rgba(0.12, 0.12, 0.14, 0.92)
        cr.fill()

        # Subtle border
        cr.new_sub_path()
        cr.arc(bar_x + bar_w - r, bar_y + r, r, -math.pi / 2, 0)
        cr.arc(bar_x + bar_w - r, bar_y + bar_h - r, r, 0, math.pi / 2)
        cr.arc(bar_x + r, bar_y + bar_h - r, r, math.pi / 2, math.pi)
        cr.arc(bar_x + r, bar_y + r, r, math.pi, 3 * math.pi / 2)
        cr.close_path()
        cr.set_source_rgba(1, 1, 1, 0.1)
        cr.set_line_width(1)
        cr.stroke()

        # Buttons
        for btn in self.toolbar_buttons:
            if btn.name.startswith("sep"):
                cr.set_source_rgba(1, 1, 1, 0.15)
                cr.rectangle(btn.x, btn.y, btn.w, btn.h)
                cr.fill()
                continue

            # Button background
            is_tool_btn = btn.name in (
                TOOL_PEN, TOOL_ERASER, TOOL_LINE, TOOL_RECT,
                TOOL_CIRCLE, TOOL_ARROW
            )
            is_active = is_tool_btn and self.current_tool == btn.name
            if is_active:
                # Active glow
                self._rounded_rect(cr, btn.x, btn.y, btn.w, btn.h, 8)
                cr.set_source_rgba(1, 1, 1, 0.18)
                cr.fill()
            elif btn.hover:
                self._rounded_rect(cr, btn.x, btn.y, btn.w, btn.h, 8)
                cr.set_source_rgba(1, 1, 1, 0.08)
                cr.fill()

            # Draw icon
            if btn.icon_draw_func:
                cr.save()
                btn.icon_draw_func(cr, btn.x, btn.y, btn.w, btn.h, is_active)
                cr.restore()

            # Active dot indicator
            if is_active:
                cr.arc(btn.x + btn.w / 2, btn.y + btn.h - 1, 2, 0, 2 * math.pi)
                r, g, b, _ = hex_to_rgba(self.current_color)
                cr.set_source_rgba(r, g, b, 1)
                cr.fill()

    def _rounded_rect(self, cr, x, y, w, h, r):
        cr.new_sub_path()
        cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
        cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
        cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
        cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
        cr.close_path()

    # ── Pen Options Submenu ───────────────────────────────────────────────

    def _draw_pen_options_submenu(self, cr, width):
        """Draw a submenu below the pen button for color & stroke selection."""
        pen_btn = None
        for btn in self.toolbar_buttons:
            if btn.name == "pen":
                pen_btn = btn
                break
        if not pen_btn:
            return

        # Submenu dimensions
        sm_w = 280
        sm_h = 120
        sm_x = pen_btn.x + pen_btn.w / 2 - sm_w / 2
        sm_y = TOOLBAR_HEIGHT + 8

        # Clamp to screen
        if sm_x < 8:
            sm_x = 8

        # Background
        r = 12
        self._rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, r)
        cr.set_source_rgba(0.14, 0.14, 0.16, 0.94)
        cr.fill()
        self._rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, r)
        cr.set_source_rgba(1, 1, 1, 0.08)
        cr.set_line_width(1)
        cr.stroke()

        # ── Color swatches ──
        self.submenu_buttons = []
        swatch_size = 22
        swatch_gap = 6
        row_y = sm_y + 14
        start_x = sm_x + 14

        cr.set_source_rgba(1, 1, 1, 0.5)
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        cr.set_font_size(11)
        cr.move_to(start_x, row_y)
        cr.show_text("COLOR")
        row_y += 10

        for i, (hex_c, name) in enumerate(COLOR_PRESETS):
            sx = start_x + i * (swatch_size + swatch_gap)
            sy = row_y

            # Draw swatch
            rc, gc, bc, _ = hex_to_rgba(hex_c)
            cr.arc(sx + swatch_size / 2, sy + swatch_size / 2, swatch_size / 2, 0, 2 * math.pi)
            cr.set_source_rgba(rc, gc, bc, 1)
            cr.fill()

            # Selection ring
            if hex_c == self.current_color:
                cr.arc(sx + swatch_size / 2, sy + swatch_size / 2,
                       swatch_size / 2 + 3, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.8)
                cr.set_line_width(2)
                cr.stroke()

            self.submenu_buttons.append({
                "type": "color",
                "value": hex_c,
                "x": sx, "y": sy, "w": swatch_size, "h": swatch_size,
            })

        # ── Stroke width ──
        row_y += swatch_size + 16
        cr.set_source_rgba(1, 1, 1, 0.5)
        cr.set_font_size(11)
        cr.move_to(start_x, row_y)
        cr.show_text("STROKE")
        row_y += 10

        for i, sw in enumerate(STROKE_PRESETS):
            sx = start_x + i * 36
            sy = row_y
            dot_r = max(sw / 2, 2)
            # Background pill
            if self.stroke_width == sw:
                self._rounded_rect(cr, sx - 2, sy - 2, 30, 22, 6)
                cr.set_source_rgba(1, 1, 1, 0.15)
                cr.fill()
            # Dot
            cr.arc(sx + 13, sy + 9, dot_r, 0, 2 * math.pi)
            cr.set_source_rgba(1, 1, 1, 0.85)
            cr.fill()

            self.submenu_buttons.append({
                "type": "stroke",
                "value": sw,
                "x": sx - 2, "y": sy - 2, "w": 30, "h": 22,
            })

    # ── Icon Drawing Functions ────────────────────────────────────────────

    def _draw_pen_icon(self, cr, x, y, w, h, active):
        """Draw a pen/pencil icon."""
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2
        # Pen body
        cr.move_to(cx - 8, cy + 10)
        cr.line_to(cx + 6, cy - 6)
        cr.line_to(cx + 10, cy - 2)
        cr.line_to(cx - 4, cy + 14)
        cr.close_path()
        cr.stroke()
        # Tip
        cr.move_to(cx - 8, cy + 10)
        cr.line_to(cx - 10, cy + 14)
        cr.line_to(cx - 4, cy + 14)
        cr.stroke()
        # Current color indicator dot
        rc, gc, bc, _ = hex_to_rgba(self.current_color)
        cr.arc(cx + 10, cy + 10, 4, 0, 2 * math.pi)
        cr.set_source_rgba(rc, gc, bc, 1)
        cr.fill()

    def _draw_eraser_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2
        # Eraser block
        cr.rectangle(cx - 10, cy - 5, 20, 14)
        cr.stroke()
        # Eraser stripe
        cr.move_to(cx - 10, cy + 3)
        cr.line_to(cx + 10, cy + 3)
        cr.stroke()

    def _draw_line_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        cr.move_to(x + 10, y + h - 10)
        cr.line_to(x + w - 10, y + 10)
        cr.stroke()

    def _draw_rect_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        cr.rectangle(x + 8, y + 10, w - 16, h - 20)
        cr.stroke()

    def _draw_circle_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        cr.arc(x + w / 2, y + h / 2, min(w, h) / 2 - 8, 0, 2 * math.pi)
        cr.stroke()

    def _draw_arrow_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.9 if active else 0.65)
        cr.set_line_width(2)
        sx, sy = x + 10, y + h - 10
        ex, ey = x + w - 10, y + 10
        cr.move_to(sx, sy)
        cr.line_to(ex, ey)
        cr.stroke()
        # Arrowhead
        cr.move_to(ex, ey)
        cr.line_to(ex - 8, ey + 2)
        cr.move_to(ex, ey)
        cr.line_to(ex - 2, ey + 8)
        cr.stroke()

    def _draw_undo_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.65)
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2
        cr.arc(cx, cy, 8, math.pi * 0.8, math.pi * 2.2)
        cr.stroke()
        # Arrow tip
        px = cx + 8 * math.cos(math.pi * 0.8)
        py = cy + 8 * math.sin(math.pi * 0.8)
        cr.move_to(px, py)
        cr.line_to(px - 5, py - 2)
        cr.move_to(px, py)
        cr.line_to(px + 1, py - 6)
        cr.stroke()

    def _draw_redo_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.65)
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2
        cr.arc_negative(cx, cy, 8, math.pi * 0.2, -math.pi * 1.2)
        cr.stroke()
        px = cx + 8 * math.cos(math.pi * 0.2)
        py = cy + 8 * math.sin(math.pi * 0.2)
        cr.move_to(px, py)
        cr.line_to(px + 5, py - 2)
        cr.move_to(px, py)
        cr.line_to(px - 1, py - 6)
        cr.stroke()

    def _draw_clear_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 0.3, 0.3, 0.75)
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2
        # Trash can
        cr.rectangle(cx - 7, cy - 3, 14, 14)
        cr.stroke()
        cr.move_to(cx - 10, cy - 3)
        cr.line_to(cx + 10, cy - 3)
        cr.stroke()
        cr.move_to(cx - 3, cy - 3)
        cr.line_to(cx - 2, cy - 7)
        cr.line_to(cx + 2, cy - 7)
        cr.line_to(cx + 3, cy - 3)
        cr.stroke()

    def _draw_close_icon(self, cr, x, y, w, h, active):
        cr.set_source_rgba(1, 1, 1, 0.65)
        cr.set_line_width(2.5)
        cx, cy = x + w / 2, y + h / 2
        d = 7
        cr.move_to(cx - d, cy - d)
        cr.line_to(cx + d, cy + d)
        cr.move_to(cx + d, cy - d)
        cr.line_to(cx - d, cy + d)
        cr.stroke()

    # ── Hit Testing ───────────────────────────────────────────────────────

    def _toolbar_hit_test(self, x, y):
        """Return the ToolButton under (x, y), or None."""
        for btn in self.toolbar_buttons:
            if btn.name.startswith("sep"):
                continue
            if (btn.x <= x <= btn.x + btn.w
                    and btn.y <= y <= btn.y + btn.h):
                return btn
        return None

    def _submenu_hit_test(self, x, y):
        """Return the submenu item dict under (x, y), or None."""
        for item in self.submenu_buttons:
            if (item["x"] <= x <= item["x"] + item["w"]
                    and item["y"] <= y <= item["y"] + item["h"]):
                return item
        return None

    def _is_in_toolbar_area(self, y):
        return y <= TOOLBAR_HEIGHT

    def _is_in_submenu_area(self, x, y):
        if self.submenu_open is None:
            return False
        # Approximate submenu area
        return TOOLBAR_HEIGHT < y < TOOLBAR_HEIGHT + 140

    # ── Input Handlers ────────────────────────────────────────────────────

    def _on_button_press(self, widget, event):
        if event.button != 1:
            return

        x, y = event.x, event.y

        # Check toolbar
        if self._is_in_toolbar_area(y):
            btn = self._toolbar_hit_test(x, y)
            if btn:
                self._handle_toolbar_click(btn)
            return

        # Check submenu
        if self._is_in_submenu_area(x, y):
            item = self._submenu_hit_test(x, y)
            if item:
                self._handle_submenu_click(item)
            return

        # Close submenu if open and clicked outside
        if self.submenu_open:
            self.submenu_open = None
            self.submenu_buttons = []
            self.queue_draw()
            return

        # Start drawing
        self.is_drawing = True
        if self.current_tool in (TOOL_PEN, TOOL_ERASER):
            self.current_points = [(x, y)]
        elif self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW):
            self.shape_start = (x, y)
            self.shape_end = (x, y)

    def _on_button_release(self, widget, event):
        if event.button != 1 or not self.is_drawing:
            return

        x, y = event.x, event.y
        self.is_drawing = False

        if self.current_tool == TOOL_PEN and len(self.current_points) >= 2:
            stroke = FreehandStroke(
                list(self.current_points), self.current_color, self.stroke_width
            )
            self.strokes.append(stroke)
            self.redo_stack.clear()
            # Commit to canvas
            cr = cairo.Context(self.canvas_surface)
            stroke.draw(cr)

        elif self.current_tool == TOOL_ERASER and len(self.current_points) >= 2:
            stroke = FreehandStroke(
                list(self.current_points), "#000", self.stroke_width * 3, is_eraser=True
            )
            self.strokes.append(stroke)
            self.redo_stack.clear()
            cr = cairo.Context(self.canvas_surface)
            stroke.draw(cr)

        elif (self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW)
              and self.shape_start and self.shape_end):
            stroke = ShapeStroke(
                self.current_tool,
                *self.shape_start, *self.shape_end,
                self.current_color, self.stroke_width
            )
            self.strokes.append(stroke)
            self.redo_stack.clear()
            cr = cairo.Context(self.canvas_surface)
            stroke.draw(cr)

        self.current_points = []
        self.shape_start = None
        self.shape_end = None
        self.queue_draw()

    def _on_motion(self, widget, event):
        x, y = event.x, event.y

        # Update hover states for toolbar
        needs_redraw = False
        for btn in self.toolbar_buttons:
            if btn.name.startswith("sep"):
                continue
            was_hover = btn.hover
            btn.hover = (btn.x <= x <= btn.x + btn.w
                         and btn.y <= y <= btn.y + btn.h)
            if btn.hover != was_hover:
                needs_redraw = True

        if self.is_drawing:
            if self.current_tool in (TOOL_PEN, TOOL_ERASER):
                self.current_points.append((x, y))
            elif self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW):
                self.shape_end = (x, y)
            needs_redraw = True

        if needs_redraw:
            self.queue_draw()

    def _on_key_press(self, widget, event):
        keyval = event.keyval
        state = event.state
        ctrl = state & Gdk.ModifierType.CONTROL_MASK

        keyname = Gdk.keyval_name(keyval)

        if keyname == TOGGLE_KEY:
            self._toggle_overlay()
        elif keyname == "Escape":
            self._hide_overlay()
        elif ctrl and keyname in ("z", "Z"):
            self._undo()
        elif ctrl and keyname in ("y", "Y"):
            self._redo()
        elif ctrl and keyname in ("c", "C"):
            self._clear_canvas()
        elif keyname in ("p", "P"):
            self._select_tool(TOOL_PEN)
        elif keyname in ("e", "E"):
            self._select_tool(TOOL_ERASER)
        elif keyname in ("l", "L"):
            self._select_tool(TOOL_LINE)
        elif keyname in ("r", "R"):
            self._select_tool(TOOL_RECT)
        elif keyname in ("c", "C") and not ctrl:
            self._select_tool(TOOL_CIRCLE)
        elif keyname in ("a", "A"):
            self._select_tool(TOOL_ARROW)

    # ── Toolbar Actions ───────────────────────────────────────────────────

    def _handle_toolbar_click(self, btn):
        if btn.name == "pen":
            self._select_tool(TOOL_PEN)
            # Toggle pen options submenu
            if self.submenu_open == "pen_options":
                self.submenu_open = None
                self.submenu_buttons = []
            else:
                self.submenu_open = "pen_options"
        elif btn.name == "eraser":
            self._select_tool(TOOL_ERASER)
            self.submenu_open = None
            self.submenu_buttons = []
        elif btn.name in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW):
            self._select_tool(btn.name)
            self.submenu_open = None
            self.submenu_buttons = []
        elif btn.name == "undo":
            self._undo()
        elif btn.name == "redo":
            self._redo()
        elif btn.name == "clear":
            self._clear_canvas()
        elif btn.name == "close":
            self._hide_overlay()
        self.queue_draw()

    def _handle_submenu_click(self, item):
        if item["type"] == "color":
            self.current_color = item["value"]
        elif item["type"] == "stroke":
            self.stroke_width = item["value"]
        self.queue_draw()

    def _select_tool(self, tool):
        self.current_tool = tool
        self.queue_draw()

    # ── Canvas Actions ────────────────────────────────────────────────────

    def _undo(self):
        if self.strokes:
            stroke = self.strokes.pop()
            self.redo_stack.append(stroke)
            self._redraw_canvas()
            self.queue_draw()

    def _redo(self):
        if self.redo_stack:
            stroke = self.redo_stack.pop()
            self.strokes.append(stroke)
            cr = cairo.Context(self.canvas_surface)
            stroke.draw(cr)
            self.queue_draw()

    def _clear_canvas(self):
        self.strokes.clear()
        self.redo_stack.clear()
        self._redraw_canvas()
        self.queue_draw()

    # ── Toggle / Visibility ───────────────────────────────────────────────

    def _toggle_overlay(self):
        if self.is_visible:
            self._hide_overlay()
        else:
            self._show_overlay()

    def _show_overlay(self):
        self.is_visible = True
        self.show_all()
        self.present()
        # Set cursor to crosshair
        window = self.get_window()
        if window:
            cursor = Gdk.Cursor.new_from_name(self.get_display(), "crosshair")
            window.set_cursor(cursor)

    def _hide_overlay(self):
        self.is_visible = False
        self.submenu_open = None
        self.submenu_buttons = []
        self.hide()

    def _on_global_toggle(self, keystr, user_data):
        self._toggle_overlay()

    def _on_destroy(self, widget):
        if HAS_KEYBINDER:
            Keybinder.unbind(TOGGLE_KEY)
        Gtk.main_quit()


# ─── Entry Point ──────────────────────────────────────────────────────────────

def main():
    print("Screen Draw — Press F9 to toggle the overlay")
    print("Press Ctrl+C in terminal to quit")

    win = ScreenDrawWindow()

    # Start hidden — press F9 to show
    # Show briefly to initialize, then hide
    win.show_all()
    GLib.idle_add(lambda: (win._hide_overlay(), False))

    try:
        Gtk.main()
    except KeyboardInterrupt:
        print("\nExiting...")
        sys.exit(0)


if __name__ == "__main__":
    main()
