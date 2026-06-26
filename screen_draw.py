#!/usr/bin/env python3
"""
Screen Draw — A screen annotation tool for GNOME/Wayland.

Draw over your entire screen with pens, erasers, and shapes.
Toggle the overlay with F9.
"""

import os

# Force X11 backend (XWayland) because native GNOME Wayland strictly 
# prevents normal windows from being global always-on-top overlays.
os.environ["GDK_BACKEND"] = "x11"

import sys
import math
import time
import signal
import subprocess
import warnings
import cairo
import gi

# Suppress harmless Gio deprecation warnings (register_object in newer GLib)
warnings.filterwarnings("ignore", message=".*is deprecated", category=DeprecationWarning)

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")

from gi.repository import Gtk, Gdk, GLib, Gio


# ─── D-Bus Service for Global Hotkey ──────────────────────────────────────────

DBUS_NAME = "com.tools.ScreenDraw"
DBUS_PATH = "/com/tools/ScreenDraw"
DBUS_IFACE = "com.tools.ScreenDraw"

DBUS_XML = """
<node>
  <interface name="com.tools.ScreenDraw">
    <method name="Toggle"/>
    <method name="Show"/>
    <method name="Hide"/>
    <method name="Quit"/>
  </interface>
</node>
"""

# GNOME Custom Keybinding constants
GS_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys"
GS_KEY = "custom-keybindings"
GS_CUSTOM_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding"
KEYBINDING_SLOT = "screen-draw"
KEYBINDING_PATH_PREFIX = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/"


# ─── Constants ────────────────────────────────────────────────────────────────

TOGGLE_KEY = "F9"

TOOL_PEN = "pen"
TOOL_ERASER = "eraser"
TOOL_LINE = "line"
TOOL_RECT = "rect"
TOOL_CIRCLE = "circle"
TOOL_ARROW = "arrow"
TOOL_TEXT = "text"

COLOR_PRESETS = [
    ("#FF3B30", "Red"),
    ("#FF9500", "Orange"),
    ("#FFCC00", "Yellow"),
    ("#34C759", "Green"),
    ("#007AFF", "Blue"),
    ("#5856D6", "Indigo"),
    ("#AF52DE", "Purple"),
    ("#FF2D55", "Pink"),
    ("#FFFFFF", "White"),
    ("#8E8E93", "Gray"),
    ("#000000", "Black"),
]

STROKE_PRESETS = [2, 3, 5, 8, 12, 18, 26]
ERASER_RADIUS_PRESETS = [6, 10, 16, 24, 36, 50]
TEXT_SIZE_PRESETS = [16, 24, 32, 48, 64, 96]

TOOLBAR_HEIGHT = 50
TOOLBAR_PADDING = 5
TOOLBAR_RADIUS = 16
TOOLBAR_BTN_SIZE = 40
TOOLBAR_GAP = 4

# Submenu dimensions
SM_COLOR_SWATCH = 26
SM_COLOR_GAP = 8
SM_STROKE_PILL_W = 34
SM_STROKE_PILL_H = 26


# ─── Helpers ──────────────────────────────────────────────────────────────────

def hex_to_rgba(hex_color, alpha=1.0):
    """Convert hex color string to (r, g, b, a) tuple."""
    hex_color = hex_color.lstrip("#")
    r = int(hex_color[0:2], 16) / 255.0
    g = int(hex_color[2:4], 16) / 255.0
    b = int(hex_color[4:6], 16) / 255.0
    return (r, g, b, alpha)


def rounded_rect(cr, x, y, w, h, r):
    """Draw a rounded rectangle path."""
    cr.new_sub_path()
    cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
    cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
    cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
    cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()


def lerp_color(c1, c2, t):
    """Linear interpolation between two RGBA tuples."""
    return tuple(a + (b - a) * t for a, b in zip(c1, c2))


# ─── Stroke Data ─────────────────────────────────────────────────────────────

class FreehandStroke:
    """A freehand pen or eraser stroke with Catmull-Rom smoothing."""

    def __init__(self, points, color, width, is_eraser=False):
        self.points = points
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

        pts = self.points
        cr.move_to(pts[0][0], pts[0][1])

        if len(pts) == 2:
            cr.line_to(pts[1][0], pts[1][1])
        else:
            # Quadratic Bézier smoothing
            for i in range(1, len(pts) - 1):
                xc = (pts[i][0] + pts[i + 1][0]) / 2
                yc = (pts[i][1] + pts[i + 1][1]) / 2
                cr.curve_to(
                    pts[i][0], pts[i][1],
                    pts[i][0], pts[i][1],
                    xc, yc,
                )
            # Last point
            cr.line_to(pts[-1][0], pts[-1][1])

        cr.stroke()


class ShapeStroke:
    """A geometric shape stroke."""

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
            if w > 0 and h > 0:
                cr.rectangle(x, y, w, h)
                cr.stroke()

        elif self.shape_type == TOOL_CIRCLE:
            cx = (self.x1 + self.x2) / 2
            cy = (self.y1 + self.y2) / 2
            rx = abs(self.x2 - self.x1) / 2
            ry = abs(self.y2 - self.y1) / 2
            if rx > 0 and ry > 0:
                cr.save()
                cr.translate(cx, cy)
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
            head_len = max(self.width * 3.5, 20)
            spread = math.pi / 6
            for sign in (-1, 1):
                lx = self.x2 - head_len * math.cos(angle + sign * spread)
                ly = self.y2 - head_len * math.sin(angle + sign * spread)
                cr.move_to(self.x2, self.y2)
                cr.line_to(lx, ly)
                cr.stroke()


class TextStroke:
    """A text stroke."""

    def __init__(self, text, x, y, color, font_size, bg_color):
        self.text = text
        self.x, self.y = x, y
        self.color = color
        self.font_size = font_size
        self.bg_color = bg_color

    def draw(self, cr):
        cr.set_operator(cairo.OPERATOR_OVER)
        cr.select_font_face("Inter", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        cr.set_font_size(self.font_size)
        
        if self.bg_color and self.bg_color != "transparent":
            cr.set_source_rgba(*hex_to_rgba(self.bg_color))
            extents = cr.text_extents(self.text)
            pad = 8
            cr.rectangle(self.x + extents.x_bearing - pad, 
                         self.y + extents.y_bearing - pad,
                         extents.width + pad * 2, 
                         extents.height + pad * 2)
            cr.fill()

        r, g, b, a = hex_to_rgba(self.color)
        cr.set_source_rgba(r, g, b, a)
        cr.move_to(self.x, self.y)
        cr.show_text(self.text)


# ─── Main Application ────────────────────────────────────────────────────────

class ScreenDrawApp(Gtk.Application):
    def __init__(self):
        super().__init__(
            application_id="com.tools.screendraw",
            flags=Gio.ApplicationFlags.FLAGS_NONE,
        )
        self._dbus_service = None
        self._window = None

    def do_activate(self):
        if self._window is None:
            self.hold()  # Prevent auto-quit (no ApplicationWindow)
            self._window = ScreenDrawWindow()
            self._window.connect("destroy", lambda _: self.release())
            self._window.show_all()
            GLib.idle_add(self._window._hide_overlay)
            # Set up D-Bus service
            self._dbus_service = ScreenDrawDBusService(self._window)
            print("[\u2713] D-Bus service registered")
        else:
            # Already running, toggle overlay
            self._window._toggle_overlay()


class ScreenDrawWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="Screen Draw")

        # ── Drawing state ──
        self.strokes = []
        self.redo_stack = []
        self.current_points = []
        self.is_drawing = False
        self.shape_start = None
        self.shape_end = None

        # ── Tool state ──
        self.current_tool = TOOL_PEN
        self.current_color = "#FF3B30"
        self.stroke_width = 5
        self.eraser_radius = 16
        self.is_visible = False

        # ── Text state ──
        self.is_typing = False
        self.typing_x = 0
        self.typing_y = 0
        self.typing_text = ""
        self._blink_id = None
        self.text_size = 24
        self.text_bg_color = "transparent"

        # ── UI state ──
        self.submenu_open = None   # "pen_options" | "eraser_options" | None
        self.submenu_items = []
        self.toolbar_buttons = []
        self.hover_button = None
        self.mouse_x = 0
        self.mouse_y = 0
        self._last_toolbar_click_time = 0  # debounce duplicate GTK events
        self._passthrough_mode = False  # click-through interact mode

        # ── Canvas surface ──
        self.canvas_surface = None

        # ── Focus-keepalive timer ──
        self._focus_keepalive_id = None

        # ── Event system state ──
        self._draw_scheduled = False       # whether an idle-draw is pending
        self._cached_toolbar_width = -1    # last width used for toolbar layout
        self._cursor_zone = None           # "toolbar"|"eraser"|"canvas" — avoid redundant cursor sets
        self._cursors = {}                 # cached Gdk.Cursor objects
        self._min_point_dist_sq = 4.0      # min squared distance between freehand points (2px)
        self._last_motion_time = 0.0       # for motion coalescing
        self._motion_coalesce_id = None    # pending motion idle handler
        self._pending_motion_xy = None     # latest unprocessed motion (x, y)

        # ── Setup ──
        self._setup_window()
        self._build_toolbar()
        self._connect_events()

    def _setup_window(self):
        screen = Gdk.Screen.get_default()
        visual = screen.get_rgba_visual()
        if visual:
            self.set_visual(visual)

        self.set_app_paintable(True)
        self.set_decorated(False)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_type_hint(Gdk.WindowTypeHint.UTILITY)
        self.set_keep_above(True)
        self.set_accept_focus(True)
        self.set_title("Screen Draw")

        # Get monitor geometry
        display = Gdk.Display.get_default()
        monitor = display.get_primary_monitor()
        if monitor is None:
            monitor = display.get_monitor(0)
        geom = monitor.get_geometry()
        self.mon_width = geom.width
        self.mon_height = geom.height

        self.set_default_size(self.mon_width, self.mon_height)
        self.move(geom.x, geom.y)
        # NOTE: Do NOT call fullscreen() — on GNOME/Mutter Wayland, fullscreen
        # windows are "unredirected" (bypass the compositor), which makes RGBA
        # transparency impossible. Instead we size the window to fill the screen.
        self.resize(self.mon_width, self.mon_height)

        # Input events
        self.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK
            | Gdk.EventMask.BUTTON_RELEASE_MASK
            | Gdk.EventMask.POINTER_MOTION_MASK
            | Gdk.EventMask.KEY_PRESS_MASK
            | Gdk.EventMask.LEAVE_NOTIFY_MASK
        )



    def _connect_events(self):
        self.connect("draw", self._on_draw)
        self.connect("button-press-event", self._on_button_press)
        self.connect("button-release-event", self._on_button_release)
        self.connect("motion-notify-event", self._on_motion)
        self.connect("key-press-event", self._on_key_press)
        self.connect("destroy", self._on_destroy)
        self.connect("configure-event", self._on_configure)
        self.connect("focus-out-event", self._on_focus_out)

    # ── Toolbar Definition ────────────────────────────────────────────────

    def _build_toolbar(self):
        """Define toolbar buttons with their icons and metadata."""
        self.toolbar_buttons = []
        defs = [
            ("pen",    "Pen (P)",       True),
            ("eraser", "Eraser (E)",    False),
            ("__sep",  "",              False),
            ("text",   "Text (T)",      False),
            ("line",   "Line (L)",      False),
            ("rect",   "Rectangle (R)", False),
            ("circle", "Circle (O)",    False),
            ("arrow",  "Arrow (A)",     False),
            ("__sep2", "",              False),
            ("undo",   "Undo (Ctrl+Z)", False),
            ("redo",   "Redo (Ctrl+Y)", False),
            ("clear",  "Clear (Ctrl+X)",False),
            ("__sep3", "",              False),
            ("cursor", "Interact (C)",  False),
            ("close",  "Close (F9)",    False),
        ]
        for name, tip, is_active in defs:
            self.toolbar_buttons.append({
                "name": name,
                "tip": tip,
                "active": is_active,
                "x": 0, "y": 0,
                "w": TOOLBAR_BTN_SIZE if not name.startswith("__sep") else 2,
                "h": TOOLBAR_BTN_SIZE,
            })

    def _layout_toolbar(self, width):
        """Position toolbar buttons centered horizontally (cached)."""
        if width == self._cached_toolbar_width:
            return  # layout unchanged, skip recalculation
        self._cached_toolbar_width = width

        total_w = 0
        for b in self.toolbar_buttons:
            if b["name"].startswith("__sep"):
                total_w += 12
            else:
                total_w += TOOLBAR_BTN_SIZE + TOOLBAR_GAP
        start_x = (width - total_w) / 2
        cx = start_x
        for b in self.toolbar_buttons:
            if b["name"].startswith("__sep"):
                b["x"] = cx + 4
                b["y"] = TOOLBAR_PADDING + 8
                b["w"] = 2
                b["h"] = TOOLBAR_BTN_SIZE - 16
                cx += 12
            else:
                b["x"] = cx
                b["y"] = TOOLBAR_PADDING
                b["w"] = TOOLBAR_BTN_SIZE
                b["h"] = TOOLBAR_BTN_SIZE
                cx += TOOLBAR_BTN_SIZE + TOOLBAR_GAP

    # ── Canvas Management ─────────────────────────────────────────────────

    def _ensure_canvas(self, w, h):
        if (self.canvas_surface is None
                or self.canvas_surface.get_width() != w
                or self.canvas_surface.get_height() != h):
            new_s = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
            if self.canvas_surface:
                cr = cairo.Context(new_s)
                cr.set_source_surface(self.canvas_surface, 0, 0)
                cr.paint()
            self.canvas_surface = new_s

    def _rebuild_canvas(self):
        if self.canvas_surface is None:
            return
        w = self.canvas_surface.get_width()
        h = self.canvas_surface.get_height()
        self.canvas_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
        cr = cairo.Context(self.canvas_surface)
        for s in self.strokes:
            s.draw(cr)

    def _on_configure(self, widget, event):
        self._ensure_canvas(event.width, event.height)

    # ── Main Draw ─────────────────────────────────────────────────────────

    def _on_draw(self, widget, cr):
        alloc = self.get_allocation()
        w, h = alloc.width, alloc.height
        self._ensure_canvas(w, h)
        self._layout_toolbar(w)

        # Clear to transparent
        cr.set_operator(cairo.OPERATOR_SOURCE)
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_operator(cairo.OPERATOR_OVER)

        # Very light tint so user knows overlay is active (skip in passthrough mode)
        if not self._passthrough_mode:
            cr.set_source_rgba(0, 0, 0, 0.03)
            cr.paint()

        # Paint committed strokes
        cr.set_source_surface(self.canvas_surface, 0, 0)
        cr.paint()

        # Paint in-progress stroke
        if self.is_drawing:
            self._draw_current_stroke(cr)

        if self.is_typing:
            self._draw_typing_text(cr)

        # Eraser cursor circle
        if self.current_tool == TOOL_ERASER and not self.is_drawing and not self._passthrough_mode:
            cr.set_source_rgba(1, 1, 1, 0.5)
            cr.set_line_width(1.5)
            cr.arc(self.mouse_x, self.mouse_y,
                   self.eraser_radius, 0, 2 * math.pi)
            cr.stroke()

        # Toolbar
        self._draw_toolbar(cr, w)

        # Submenu
        if self.submenu_open == "pen_options":
            self._draw_pen_submenu(cr, w)
        elif self.submenu_open == "eraser_options":
            self._draw_eraser_submenu(cr, w)
        elif self.submenu_open == "text_options":
            self._draw_text_submenu(cr, w)

    def _draw_typing_text(self, cr):
        font_size = self.text_size
        cr.select_font_face("Inter", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        cr.set_font_size(font_size)
        
        extents = cr.text_extents(self.typing_text) if self.typing_text else cr.text_extents("")
        
        if self.text_bg_color and self.text_bg_color != "transparent" and self.typing_text:
            cr.set_source_rgba(*hex_to_rgba(self.text_bg_color))
            pad = 8
            cr.rectangle(self.typing_x + extents.x_bearing - pad, 
                         self.typing_y + extents.y_bearing - pad,
                         extents.width + pad * 2, 
                         extents.height + pad * 2)
            cr.fill()

        r, g, b, a = hex_to_rgba(self.current_color)
        cr.set_source_rgba(r, g, b, a)
        
        cr.move_to(self.typing_x, self.typing_y)
        if self.typing_text:
            cr.show_text(self.typing_text)
            
        cursor_x = self.typing_x + extents.x_advance
        cursor_y = self.typing_y
        
        if int(time.time() * 2) % 2 == 0:
            cr.move_to(cursor_x, cursor_y)
            cr.line_to(cursor_x, cursor_y - font_size * 0.8)
            cr.set_line_width(2)
            cr.stroke()

    def _draw_current_stroke(self, cr):
        """Render the stroke currently being drawn."""
        if self.current_tool == TOOL_PEN and len(self.current_points) >= 2:
            FreehandStroke(
                self.current_points, self.current_color, self.stroke_width
            ).draw(cr)

        elif self.current_tool == TOOL_ERASER and len(self.current_points) >= 2:
            FreehandStroke(
                self.current_points, "#000", self.eraser_radius * 2,
                is_eraser=True
            ).draw(cr)

        elif (self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW)
              and self.shape_start and self.shape_end):
            ShapeStroke(
                self.current_tool,
                *self.shape_start, *self.shape_end,
                self.current_color, self.stroke_width
            ).draw(cr)

    # ── Toolbar Rendering ─────────────────────────────────────────────────

    def _draw_toolbar(self, cr, width):
        # Compute bar dimensions
        first = None
        last = None
        for b in self.toolbar_buttons:
            if not b["name"].startswith("__sep"):
                if first is None:
                    first = b
                last = b

        if not first or not last:
            return

        bar_x = first["x"] - 10
        bar_w = (last["x"] + last["w"]) - first["x"] + 20
        bar_y = 0
        bar_h = TOOLBAR_HEIGHT

        # Shadow
        cr.save()
        for i in range(6):
            rounded_rect(cr, bar_x - i, bar_y - i, bar_w + 2 * i, bar_h + 2 * i, TOOLBAR_RADIUS + i)
            cr.set_source_rgba(0, 0, 0, 0.03)
            cr.fill()
        cr.restore()

        # Background
        rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, TOOLBAR_RADIUS)
        # Gradient
        grad = cairo.LinearGradient(bar_x, bar_y, bar_x, bar_y + bar_h)
        grad.add_color_stop_rgba(0, 0.16, 0.16, 0.18, 0.95)
        grad.add_color_stop_rgba(1, 0.10, 0.10, 0.12, 0.95)
        cr.set_source(grad)
        cr.fill()

        # Border
        rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, TOOLBAR_RADIUS)
        cr.set_source_rgba(1, 1, 1, 0.08)
        cr.set_line_width(1)
        cr.stroke()

        # Buttons
        tool_names = {TOOL_PEN, TOOL_ERASER, TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW, TOOL_TEXT}
        for b in self.toolbar_buttons:
            name = b["name"]
            if name.startswith("__sep"):
                # Separator
                cr.set_source_rgba(1, 1, 1, 0.1)
                cr.rectangle(b["x"], b["y"], b["w"], b["h"])
                cr.fill()
                continue

            bx, by, bw, bh = b["x"], b["y"], b["w"], b["h"]
            if name == "cursor":
                is_active = self._passthrough_mode
            else:
                is_active = name in tool_names and self.current_tool == name
            is_hover = (self.hover_button == name)

            # Button background
            if is_active:
                rounded_rect(cr, bx + 2, by + 2, bw - 4, bh - 4, 8)
                r, g, bc, _ = hex_to_rgba(self.current_color, 0.25)
                cr.set_source_rgba(r, g, bc, 0.25)
                cr.fill()
            elif is_hover:
                rounded_rect(cr, bx + 2, by + 2, bw - 4, bh - 4, 8)
                cr.set_source_rgba(1, 1, 1, 0.08)
                cr.fill()

            # Icon
            self._draw_button_icon(cr, name, bx, by, bw, bh, is_active)

            # Active indicator dot
            if is_active:
                cr.arc(bx + bw / 2, by + bh - 2, 2.5, 0, 2 * math.pi)
                rc, gc, bcc, _ = hex_to_rgba(self.current_color)
                cr.set_source_rgba(rc, gc, bcc, 1)
                cr.fill()

    def _draw_button_icon(self, cr, name, x, y, w, h, active):
        """Draw the icon for a specific toolbar button."""
        alpha = 0.95 if active else 0.65
        cr.set_line_width(2)
        cx, cy = x + w / 2, y + h / 2

        if name == "text":
            cr.set_source_rgba(1, 1, 1, alpha)
            cr.select_font_face("Inter", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
            cr.set_font_size(20)
            ext = cr.text_extents("T")
            cr.move_to(cx - ext.width / 2 - ext.x_bearing, cy - ext.height / 2 - ext.y_bearing)
            cr.show_text("T")

        elif name == "pen":
            cr.set_source_rgba(1, 1, 1, alpha)
            # Pen body (angled rectangle)
            cr.save()
            cr.translate(cx, cy)
            cr.rotate(-math.pi / 4)
            cr.rectangle(-3, -12, 6, 18)
            cr.stroke()
            # Tip
            cr.move_to(-3, 6)
            cr.line_to(0, 11)
            cr.line_to(3, 6)
            cr.stroke()
            cr.restore()
            # Color dot
            rc, gc, bc, _ = hex_to_rgba(self.current_color)
            cr.arc(x + w - 8, y + h - 8, 5, 0, 2 * math.pi)
            cr.set_source_rgba(rc, gc, bc, 1)
            cr.fill()
            cr.set_source_rgba(1, 1, 1, 0.5)
            cr.arc(x + w - 8, y + h - 8, 5, 0, 2 * math.pi)
            cr.set_line_width(1)
            cr.stroke()

        elif name == "eraser":
            cr.set_source_rgba(1, 1, 1, alpha)
            # Eraser rectangle
            cr.save()
            cr.translate(cx, cy)
            cr.rotate(-math.pi / 6)
            rounded_rect(cr, -12, -6, 24, 12, 3)
            cr.stroke()
            # Divider line
            cr.move_to(-4, -6)
            cr.line_to(-4, 6)
            cr.stroke()
            cr.restore()

        elif name == "line":
            cr.set_source_rgba(1, 1, 1, alpha)
            cr.move_to(x + 10, y + h - 10)
            cr.line_to(x + w - 10, y + 10)
            cr.stroke()

        elif name == "rect":
            cr.set_source_rgba(1, 1, 1, alpha)
            rounded_rect(cr, x + 9, y + 11, w - 18, h - 22, 2)
            cr.stroke()

        elif name == "circle":
            cr.set_source_rgba(1, 1, 1, alpha)
            cr.arc(cx, cy, min(w, h) / 2 - 9, 0, 2 * math.pi)
            cr.stroke()

        elif name == "arrow":
            cr.set_source_rgba(1, 1, 1, alpha)
            sx, sy = x + 10, y + h - 10
            ex, ey = x + w - 10, y + 10
            cr.move_to(sx, sy)
            cr.line_to(ex, ey)
            cr.stroke()
            # Arrowhead
            angle = math.atan2(ey - sy, ex - sx)
            hl = 10
            for sign in (-1, 1):
                cr.move_to(ex, ey)
                cr.line_to(
                    ex - hl * math.cos(angle + sign * math.pi / 5),
                    ey - hl * math.sin(angle + sign * math.pi / 5),
                )
                cr.stroke()

        elif name == "undo":
            cr.set_source_rgba(1, 1, 1, 0.65)
            # Curved arrow pointing left
            cr.arc(cx + 2, cy, 8, math.pi * 0.7, math.pi * 2.3)
            cr.stroke()
            px = cx + 2 + 8 * math.cos(math.pi * 0.7)
            py = cy + 8 * math.sin(math.pi * 0.7)
            cr.move_to(px, py)
            cr.line_to(px - 5, py - 1)
            cr.move_to(px, py)
            cr.line_to(px + 1, py - 6)
            cr.stroke()

        elif name == "redo":
            cr.set_source_rgba(1, 1, 1, 0.65)
            cr.arc_negative(cx - 2, cy, 8, math.pi * 0.3, -math.pi * 1.3)
            cr.stroke()
            px = cx - 2 + 8 * math.cos(math.pi * 0.3)
            py = cy + 8 * math.sin(math.pi * 0.3)
            cr.move_to(px, py)
            cr.line_to(px + 5, py - 1)
            cr.move_to(px, py)
            cr.line_to(px - 1, py - 6)
            cr.stroke()

        elif name == "clear":
            cr.set_source_rgba(1, 0.35, 0.35, 0.8)
            # Trash icon — vertically centered around (cx, cy)
            cr.set_line_width(1.8)
            top = cy - 9   # top of handle
            lid_y = cy - 5  # lid line
            bot = cy + 8   # bottom of can
            # Can body
            rounded_rect(cr, cx - 7, lid_y, 14, bot - lid_y, 2)
            cr.stroke()
            # Lid
            cr.move_to(cx - 9, lid_y)
            cr.line_to(cx + 9, lid_y)
            cr.stroke()
            # Handle
            cr.move_to(cx - 3, lid_y)
            cr.line_to(cx - 2, top)
            cr.line_to(cx + 2, top)
            cr.line_to(cx + 3, lid_y)
            cr.stroke()
            # Lines
            for dx in (-3, 0, 3):
                cr.move_to(cx + dx, lid_y + 3)
                cr.line_to(cx + dx, bot - 3)
                cr.stroke()

        elif name == "cursor":
            cr.set_source_rgba(1, 1, 1, alpha)
            # Classic mouse pointer cursor
            px, py = cx - 5, cy - 9
            cr.new_sub_path()
            cr.move_to(px, py)              # tip
            cr.line_to(px, py + 16)         # left edge down
            cr.line_to(px + 5, py + 12)     # notch inward
            cr.line_to(px + 8, py + 17)     # handle lower
            cr.line_to(px + 11, py + 15)    # handle right
            cr.line_to(px + 7, py + 10)     # handle upper
            cr.line_to(px + 12, py + 8)     # right wing
            cr.close_path()
            cr.fill_preserve()
            cr.set_source_rgba(0, 0, 0, 0.4)
            cr.set_line_width(1)
            cr.stroke()

        elif name == "close":
            cr.set_source_rgba(1, 1, 1, 0.65)
            cr.set_line_width(2.5)
            d = 7
            cr.move_to(cx - d, cy - d)
            cr.line_to(cx + d, cy + d)
            cr.move_to(cx + d, cy - d)
            cr.line_to(cx - d, cy + d)
            cr.stroke()

    # ── Pen Options Submenu ───────────────────────────────────────────────

    def _draw_pen_submenu(self, cr, width):
        """Draw color and stroke width picker below the pen button."""
        pen_btn = None
        for b in self.toolbar_buttons:
            if b["name"] == self.current_tool:
                pen_btn = b
                break
        if not pen_btn:
            for b in self.toolbar_buttons:
                if b["name"] == "pen":
                    pen_btn = b
                    break
        if not pen_btn:
            return

        # Calculate submenu size
        n_colors = len(COLOR_PRESETS)
        color_row_w = n_colors * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP
        n_strokes = len(STROKE_PRESETS)
        stroke_row_w = n_strokes * (SM_STROKE_PILL_W + 4) - 4

        sm_content_w = max(color_row_w, stroke_row_w)
        sm_w = sm_content_w + 32
        sm_h = 108
        sm_x = pen_btn["x"] + pen_btn["w"] / 2 - sm_w / 2
        sm_y = TOOLBAR_HEIGHT + 10

        # Clamp
        sm_x = max(8, min(sm_x, width - sm_w - 8))

        # Shadow
        for i in range(5):
            rounded_rect(cr, sm_x - i, sm_y - i, sm_w + 2 * i, sm_h + 2 * i, 14 + i)
            cr.set_source_rgba(0, 0, 0, 0.04)
            cr.fill()

        # Background
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        grad = cairo.LinearGradient(sm_x, sm_y, sm_x, sm_y + sm_h)
        grad.add_color_stop_rgba(0, 0.18, 0.18, 0.20, 0.96)
        grad.add_color_stop_rgba(1, 0.12, 0.12, 0.14, 0.96)
        cr.set_source(grad)
        cr.fill()

        # Border
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        cr.set_source_rgba(1, 1, 1, 0.06)
        cr.set_line_width(1)
        cr.stroke()

        # Pointer triangle
        tri_cx = pen_btn["x"] + pen_btn["w"] / 2
        tri_cy = sm_y
        cr.move_to(tri_cx - 8, tri_cy)
        cr.line_to(tri_cx, tri_cy - 6)
        cr.line_to(tri_cx + 8, tri_cy)
        cr.close_path()
        cr.set_source_rgba(0.18, 0.18, 0.20, 0.96)
        cr.fill()

        self.submenu_items = []
        pad_x = sm_x + 16
        cur_y = sm_y + 14

        # Label: COLOR
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("COLOR")
        cur_y += 8

        # Color swatches
        for i, (hex_c, _name) in enumerate(COLOR_PRESETS):
            sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP)
            sy = cur_y
            rc, gc, bc, _ = hex_to_rgba(hex_c)

            # Selection ring
            if hex_c == self.current_color:
                cr.new_sub_path()
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2 + 3, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.7)
                cr.set_line_width(2)
                cr.stroke()

            # Swatch circle
            cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                   SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
            cr.set_source_rgba(rc, gc, bc, 1)
            cr.fill()

            # Border for light colors
            if hex_c in ("#FFFFFF", "#FFCC00"):
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.2)
                cr.set_line_width(1)
                cr.stroke()

            self.submenu_items.append({
                "type": "color", "value": hex_c,
                "x": sx, "y": sy,
                "w": SM_COLOR_SWATCH, "h": SM_COLOR_SWATCH,
            })

        cur_y += SM_COLOR_SWATCH + 14

        # Label: STROKE
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("STROKE")
        cur_y += 8

        # Stroke width pills
        for i, sw in enumerate(STROKE_PRESETS):
            sx = pad_x + i * (SM_STROKE_PILL_W + 4)
            sy = cur_y
            is_sel = self.stroke_width == sw

            # Pill background
            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
            if is_sel:
                rc, gc, bc, _ = hex_to_rgba(self.current_color, 0.3)
                cr.set_source_rgba(rc, gc, bc, 0.3)
            else:
                cr.set_source_rgba(1, 1, 1, 0.06)
            cr.fill()

            if is_sel:
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
                cr.set_source_rgba(1, 1, 1, 0.15)
                cr.set_line_width(1)
                cr.stroke()

            # Dot proportional to stroke width
            dot_r = max(sw / 2.5, 1.5)
            cr.arc(sx + SM_STROKE_PILL_W / 2, sy + SM_STROKE_PILL_H / 2,
                   dot_r, 0, 2 * math.pi)
            cr.set_source_rgba(1, 1, 1, 0.85)
            cr.fill()

            self.submenu_items.append({
                "type": "pen_stroke", "value": sw,
                "x": sx, "y": sy,
                "w": SM_STROKE_PILL_W, "h": SM_STROKE_PILL_H,
            })

    def _draw_eraser_submenu(self, cr, w):
        eraser_btn = next((b for b in self.toolbar_buttons if b["name"] == "eraser"), None)
        if not eraser_btn:
            return

        sm_w = len(ERASER_RADIUS_PRESETS) * (SM_STROKE_PILL_W + 4) + 28
        sm_h = 70
        sm_x = min(max(eraser_btn["x"] + eraser_btn["w"] / 2 - sm_w / 2, 10), w - sm_w - 10)
        sm_y = TOOLBAR_HEIGHT + 8

        # Background
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        cr.set_source_rgba(0.12, 0.12, 0.14, 0.96)
        cr.fill()
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        cr.set_source_rgba(1, 1, 1, 0.06)
        cr.set_line_width(1)
        cr.stroke()

        # Pointer triangle
        tri_cx = eraser_btn["x"] + eraser_btn["w"] / 2
        tri_cy = sm_y
        cr.move_to(tri_cx - 8, tri_cy)
        cr.line_to(tri_cx, tri_cy - 6)
        cr.line_to(tri_cx + 8, tri_cy)
        cr.close_path()
        cr.set_source_rgba(0.18, 0.18, 0.20, 0.96)
        cr.fill()

        self.submenu_items = []
        pad_x = sm_x + 16
        cur_y = sm_y + 14

        # Label: RADIUS
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("RADIUS")
        cur_y += 8

        # Eraser radius pills
        for i, rad in enumerate(ERASER_RADIUS_PRESETS):
            sx = pad_x + i * (SM_STROKE_PILL_W + 4)
            sy = cur_y
            is_sel = self.eraser_radius == rad

            # Pill background
            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
            if is_sel:
                cr.set_source_rgba(1, 1, 1, 0.3)
            else:
                cr.set_source_rgba(1, 1, 1, 0.06)
            cr.fill()

            if is_sel:
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
                cr.set_source_rgba(1, 1, 1, 0.15)
                cr.set_line_width(1)
                cr.stroke()

            # Dot proportional to radius
            dot_r = max(rad / 3.5, 1.5)
            cr.arc(sx + SM_STROKE_PILL_W / 2, sy + SM_STROKE_PILL_H / 2,
                   dot_r, 0, 2 * math.pi)
            cr.set_source_rgba(1, 1, 1, 0.85)
            cr.fill()

            self.submenu_items.append({
                "type": "eraser_radius", "value": rad,
                "x": sx, "y": sy,
                "w": SM_STROKE_PILL_W, "h": SM_STROKE_PILL_H,
            })

    def _draw_text_submenu(self, cr, w):
        text_btn = next((b for b in self.toolbar_buttons if b["name"] == "text"), None)
        if not text_btn:
            return

        # Layout
        n_colors = len(COLOR_PRESETS)
        n_bg_colors = n_colors + 1 # include transparent
        n_sizes = len(TEXT_SIZE_PRESETS)
        
        color_row_w = n_colors * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP
        bg_row_w = n_bg_colors * (SM_COLOR_SWATCH + SM_COLOR_GAP) - SM_COLOR_GAP
        size_row_w = n_sizes * (SM_STROKE_PILL_W + 4) - 4
        
        sm_content_w = max(color_row_w, bg_row_w, size_row_w)
        sm_w = sm_content_w + 32
        sm_h = 160
        sm_x = min(max(text_btn["x"] + text_btn["w"] / 2 - sm_w / 2, 10), w - sm_w - 10)
        sm_y = TOOLBAR_HEIGHT + 8

        # Background
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        grad = cairo.LinearGradient(sm_x, sm_y, sm_x, sm_y + sm_h)
        grad.add_color_stop_rgba(0, 0.18, 0.18, 0.20, 0.96)
        grad.add_color_stop_rgba(1, 0.12, 0.12, 0.14, 0.96)
        cr.set_source(grad)
        cr.fill()
        
        rounded_rect(cr, sm_x, sm_y, sm_w, sm_h, 14)
        cr.set_source_rgba(1, 1, 1, 0.06)
        cr.set_line_width(1)
        cr.stroke()

        # Pointer triangle
        tri_cx = text_btn["x"] + text_btn["w"] / 2
        tri_cy = sm_y
        cr.move_to(tri_cx - 8, tri_cy)
        cr.line_to(tri_cx, tri_cy - 6)
        cr.line_to(tri_cx + 8, tri_cy)
        cr.close_path()
        cr.set_source_rgba(0.18, 0.18, 0.20, 0.96)
        cr.fill()

        self.submenu_items = []
        pad_x = sm_x + 16
        cur_y = sm_y + 14

        # 1. Label: TEXT COLOR
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("TEXT COLOR")
        cur_y += 8

        # Color swatches
        for i, (hex_c, _name) in enumerate(COLOR_PRESETS):
            sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP)
            sy = cur_y
            rc, gc, bc, _ = hex_to_rgba(hex_c)

            if hex_c == self.current_color:
                cr.new_sub_path()
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2 + 3, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.7)
                cr.set_line_width(2)
                cr.stroke()

            cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                   SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
            cr.set_source_rgba(rc, gc, bc, 1)
            cr.fill()

            if hex_c in ("#FFFFFF", "#FFCC00"):
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.2)
                cr.set_line_width(1)
                cr.stroke()

            self.submenu_items.append({
                "type": "color", "value": hex_c,
                "x": sx, "y": sy,
                "w": SM_COLOR_SWATCH, "h": SM_COLOR_SWATCH,
            })

        cur_y += SM_COLOR_SWATCH + 14

        # 2. Label: BACKGROUND
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("BACKGROUND")
        cur_y += 8

        # Background swatches (including transparent)
        bg_options = [("transparent", "Transparent")] + COLOR_PRESETS
        for i, (hex_c, _name) in enumerate(bg_options):
            sx = pad_x + i * (SM_COLOR_SWATCH + SM_COLOR_GAP)
            sy = cur_y
            
            if hex_c == self.text_bg_color:
                cr.new_sub_path()
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2 + 3, 0, 2 * math.pi)
                cr.set_source_rgba(1, 1, 1, 0.7)
                cr.set_line_width(2)
                cr.stroke()
                
            if hex_c == "transparent":
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
                cr.set_source_rgba(0.2, 0.2, 0.2, 1)
                cr.fill_preserve()
                cr.set_source_rgba(1, 0.3, 0.3, 1)
                cr.set_line_width(2)
                cr.move_to(sx + 4, sy + 4)
                cr.line_to(sx + SM_COLOR_SWATCH - 4, sy + SM_COLOR_SWATCH - 4)
                cr.stroke()
            else:
                rc, gc, bc, _ = hex_to_rgba(hex_c)
                cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                       SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
                cr.set_source_rgba(rc, gc, bc, 1)
                cr.fill()
                if hex_c in ("#FFFFFF", "#FFCC00"):
                    cr.arc(sx + SM_COLOR_SWATCH / 2, sy + SM_COLOR_SWATCH / 2,
                           SM_COLOR_SWATCH / 2, 0, 2 * math.pi)
                    cr.set_source_rgba(1, 1, 1, 0.2)
                    cr.set_line_width(1)
                    cr.stroke()

            self.submenu_items.append({
                "type": "text_bg_color", "value": hex_c,
                "x": sx, "y": sy,
                "w": SM_COLOR_SWATCH, "h": SM_COLOR_SWATCH,
            })

        cur_y += SM_COLOR_SWATCH + 14
        
        # 3. Label: SIZE
        cr.set_source_rgba(1, 1, 1, 0.4)
        cr.set_font_size(10)
        cr.move_to(pad_x, cur_y)
        cr.show_text("SIZE")
        cur_y += 8
        
        # Size pills
        for i, size in enumerate(TEXT_SIZE_PRESETS):
            sx = pad_x + i * (SM_STROKE_PILL_W + 4)
            sy = cur_y
            is_sel = self.text_size == size
            
            rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
            if is_sel:
                rc, gc, bc, _ = hex_to_rgba(self.current_color, 0.3)
                cr.set_source_rgba(rc, gc, bc, 0.3)
            else:
                cr.set_source_rgba(1, 1, 1, 0.06)
            cr.fill()

            if is_sel:
                rounded_rect(cr, sx, sy, SM_STROKE_PILL_W, SM_STROKE_PILL_H, 6)
                cr.set_source_rgba(1, 1, 1, 0.15)
                cr.set_line_width(1)
                cr.stroke()
                
            cr.set_source_rgba(1, 1, 1, 0.85)
            cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
            cr.set_font_size(12)
            ext = cr.text_extents(str(size))
            cr.move_to(sx + SM_STROKE_PILL_W/2 - ext.width/2 - ext.x_bearing, 
                       sy + SM_STROKE_PILL_H/2 - ext.height/2 - ext.y_bearing)
            cr.show_text(str(size))
            
            self.submenu_items.append({
                "type": "text_size", "value": size,
                "x": sx, "y": sy,
                "w": SM_STROKE_PILL_W, "h": SM_STROKE_PILL_H,
            })


    # ── Hit Testing ───────────────────────────────────────────────────────

    def _hit_toolbar(self, x, y):
        for b in self.toolbar_buttons:
            if b["name"].startswith("__sep"):
                continue
            if (b["x"] <= x <= b["x"] + b["w"]
                    and b["y"] <= y <= b["y"] + b["h"]):
                return b
        return None

    def _hit_submenu(self, x, y):
        for item in self.submenu_items:
            if (item["x"] <= x <= item["x"] + item["w"]
                    and item["y"] <= y <= item["y"] + item["h"]):
                return item
        return None

    def _in_toolbar_zone(self, y):
        return y <= TOOLBAR_HEIGHT

    def _in_submenu_zone(self, x, y):
        if not self.submenu_open:
            return False
        if self.submenu_open == "eraser_options":
            return TOOLBAR_HEIGHT < y < TOOLBAR_HEIGHT + 90
        if self.submenu_open == "text_options":
            return TOOLBAR_HEIGHT < y < TOOLBAR_HEIGHT + 180
        return TOOLBAR_HEIGHT < y < TOOLBAR_HEIGHT + 130

    # ── Input Events ──────────────────────────────────────────────────────

    def _on_button_press(self, widget, event):
        if event.type != Gdk.EventType.BUTTON_PRESS:
            return
        if event.button != 1:
            return
        x, y = event.x, event.y

        # Toolbar
        if self._in_toolbar_zone(y):
            # Bring app back to focus using the direct interaction timestamp
            self.present_with_time(event.time)
            
            # Clicking the toolbar should generally exit interact mode so the 
            # user can resume drawing, unless they specifically click a button
            # that manages passthrough itself.
            btn = self._hit_toolbar(x, y)
            if not btn and self._passthrough_mode:
                self._exit_passthrough()
                
            if btn:
                if self.is_typing:
                    self._commit_text()
                # Debounce: ignore duplicate events within 200ms
                now = event.time  # GDK timestamp in milliseconds
                if now - self._last_toolbar_click_time < 200:
                    return
                self._last_toolbar_click_time = now
                self._handle_toolbar_click(btn["name"])
            return

        # Submenu
        if self._in_submenu_zone(x, y):
            self.present_with_time(event.time)
            item = self._hit_submenu(x, y)
            if item:
                self._handle_submenu_click(item)
            return

        # Close submenu
        if self.submenu_open:
            self.submenu_open = None
            self.submenu_items = []
            if self._passthrough_mode:
                self._update_input_shape()
            self._schedule_draw()
            return

        if self.is_typing:
            self._commit_text()
            if self.current_tool != TOOL_TEXT:
                return

        if self.current_tool == TOOL_TEXT:
            self.is_typing = True
            self.typing_x = x
            self.typing_y = y
            self.typing_text = ""
            self._start_cursor_blink()
            self._schedule_draw()
            return

        # Start drawing
        self.is_drawing = True
        if self.current_tool in (TOOL_PEN, TOOL_ERASER):
            self.current_points = [(x, y)]
        else:
            self.shape_start = (x, y)
            self.shape_end = (x, y)

    def _on_button_release(self, widget, event):
        if event.button != 1 or not self.is_drawing:
            return
        self.is_drawing = False

        if self.current_tool == TOOL_PEN and len(self.current_points) >= 2:
            stroke = FreehandStroke(
                list(self.current_points), self.current_color, self.stroke_width
            )
            self._commit_stroke(stroke)

        elif self.current_tool == TOOL_ERASER and len(self.current_points) >= 2:
            stroke = FreehandStroke(
                list(self.current_points), "#000", self.eraser_radius * 2,
                is_eraser=True
            )
            self._commit_stroke(stroke)

        elif (self.current_tool in (TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW)
              and self.shape_start and self.shape_end):
            stroke = ShapeStroke(
                self.current_tool,
                *self.shape_start, *self.shape_end,
                self.current_color, self.stroke_width
            )
            self._commit_stroke(stroke)

        self.current_points = []
        self.shape_start = None
        self.shape_end = None
        self._schedule_draw()

    def _commit_stroke(self, stroke):
        self.strokes.append(stroke)
        self.redo_stack.clear()
        cr = cairo.Context(self.canvas_surface)
        stroke.draw(cr)

    def _commit_text(self):
        if self.is_typing and self.typing_text.strip():
            stroke = TextStroke(
                self.typing_text, self.typing_x, self.typing_y,
                self.current_color, self.text_size, self.text_bg_color
            )
            self._commit_stroke(stroke)
        self.is_typing = False
        self.typing_text = ""
        self._stop_cursor_blink()
        self._schedule_draw()

    # ── Cached Cursor Management ───────────────────────────────────────────

    def _get_cursor(self, name):
        """Get a cached Gdk.Cursor by name, creating it on first access."""
        if name not in self._cursors:
            self._cursors[name] = Gdk.Cursor.new_from_name(self.get_display(), name)
        return self._cursors[name]

    def _update_cursor_zone(self, x, y):
        """Update the cursor only if the zone has changed."""
        if self._in_toolbar_zone(y) or self._in_submenu_zone(x, y):
            zone = "toolbar"
        elif self.current_tool == TOOL_ERASER:
            zone = "eraser"
        elif self.current_tool == TOOL_TEXT:
            zone = "text"
        else:
            zone = "canvas"

        if zone != self._cursor_zone:
            self._cursor_zone = zone
            window = self.get_window()
            if window:
                cursor_name = {"toolbar": "default", "eraser": "cell", "text": "text", "canvas": "crosshair"}[zone]
                window.set_cursor(self._get_cursor(cursor_name))

    def _start_cursor_blink(self):
        self._stop_cursor_blink()
        self._blink_id = GLib.timeout_add(500, self._on_blink_tick)

    def _stop_cursor_blink(self):
        if self._blink_id is not None:
            GLib.source_remove(self._blink_id)
            self._blink_id = None

    def _on_blink_tick(self):
        if self.is_typing:
            self._schedule_draw()
            return True
        self._blink_id = None
        return False

    # ── Batched Draw Scheduling ───────────────────────────────────────────

    def _schedule_draw(self):
        """Schedule a single draw on the next idle, coalescing multiple requests."""
        if not self._draw_scheduled:
            self._draw_scheduled = True
            GLib.idle_add(self._do_scheduled_draw)

    def _do_scheduled_draw(self):
        """Execute the batched draw."""
        self._draw_scheduled = False
        self.queue_draw()
        return False  # don't repeat

    def _schedule_draw_area(self, x, y, w, h):
        """Invalidate a specific rectangular region instead of the full window."""
        self.queue_draw_area(int(x), int(y), int(w), int(h))

    # ── Motion Event Handling (Coalesced) ─────────────────────────────────

    def _on_motion(self, widget, event):
        """Handle motion events with coalescing for high-frequency input."""
        # Get the latest pointer position (coalesce intermediate events)
        window = event.window
        if window and self.is_drawing:
            # Use get_pointer to skip to the latest position
            _, x, y, _ = window.get_pointer()
            x, y = float(x), float(y)
        else:
            x, y = event.x, event.y

        old_x, old_y = self.mouse_x, self.mouse_y
        self.mouse_x = x
        self.mouse_y = y

        # Hover detection (toolbar only)
        old_hover = self.hover_button
        hit = self._hit_toolbar(x, y) if self._in_toolbar_zone(y) else None
        self.hover_button = hit["name"] if hit else None

        # Update cursor zone (cached — only changes cursor on zone transitions)
        self._update_cursor_zone(x, y)

        if self.is_drawing:
            if self.current_tool in (TOOL_PEN, TOOL_ERASER):
                # Point distance filtering: skip points too close together
                if self.current_points:
                    lx, ly = self.current_points[-1]
                    dx, dy = x - lx, y - ly
                    if dx * dx + dy * dy < self._min_point_dist_sq:
                        return  # skip this point, too close
                self.current_points.append((x, y))
            else:
                self.shape_end = (x, y)
            self._schedule_draw()
            return

        # Non-drawing updates
        needs_redraw = False
        if self.hover_button != old_hover:
            # Invalidate only the toolbar area for hover changes
            self._schedule_draw_area(0, 0, self.mon_width, TOOLBAR_HEIGHT + 5)
            return

        if self.current_tool == TOOL_ERASER:
            # Eraser cursor circle — invalidate both old and new cursor areas
            r = self.eraser_radius + 4
            self._schedule_draw_area(old_x - r, old_y - r, r * 2, r * 2)
            self._schedule_draw_area(x - r, y - r, r * 2, r * 2)
            return

    def _on_key_press(self, widget, event):
        keyval = event.keyval
        state = event.state
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)
        keyname = Gdk.keyval_name(keyval)

        if self.is_typing:
            if ctrl:
                if keyname in ("z", "Z"):
                    self._commit_text()
                    self._undo()
                elif keyname in ("y", "Y"):
                    self._commit_text()
                    self._redo()
                return
                
            if keyname in ("Return", "KP_Enter"):
                self._commit_text()
            elif keyname == "Escape":
                self.is_typing = False
                self.typing_text = ""
                self._stop_cursor_blink()
                self._schedule_draw()
            elif keyname == "BackSpace":
                self.typing_text = self.typing_text[:-1]
                self._schedule_draw()
            elif keyname == "space":
                self.typing_text += " "
                self._schedule_draw()
            else:
                uni = Gdk.keyval_to_unicode(keyval)
                if uni:
                    if uni >= 32 and uni != 127:
                        self.typing_text += chr(uni)
                        self._schedule_draw()
            return

        if keyname == TOGGLE_KEY:
            self._toggle_overlay()
        elif keyname == "Escape":
            # Close any open submenu; do NOT hide the overlay
            if self.submenu_open:
                self.submenu_open = None
                self.submenu_items = []
                self._schedule_draw()
        elif ctrl and keyname in ("z", "Z"):
            self._undo()
        elif ctrl and keyname in ("y", "Y"):
            self._redo()
        elif ctrl and keyname in ("x", "X"):
            self._clear_canvas()
        elif not ctrl and keyname in ("p", "P"):
            self._select_tool(TOOL_PEN)
        elif not ctrl and keyname in ("e", "E"):
            self._select_tool(TOOL_ERASER)
        elif not ctrl and keyname in ("t", "T"):
            self._select_tool(TOOL_TEXT)
        elif not ctrl and keyname in ("l", "L"):
            self._select_tool(TOOL_LINE)
        elif not ctrl and keyname in ("r", "R"):
            self._select_tool(TOOL_RECT)
        elif not ctrl and keyname in ("o", "O"):
            self._select_tool(TOOL_CIRCLE)
        elif not ctrl and keyname in ("a", "A"):
            self._select_tool(TOOL_ARROW)
        elif not ctrl and keyname in ("c", "C"):
            self._enter_passthrough()

    # ── Actions ───────────────────────────────────────────────────────────

    def _handle_toolbar_click(self, name):
        tool_names = {TOOL_LINE, TOOL_RECT, TOOL_CIRCLE, TOOL_ARROW, TOOL_TEXT}

        if name == "pen":
            if self._passthrough_mode:
                self._exit_passthrough()
            self._cursor_zone = None
            if self.current_tool != TOOL_PEN:
                # First click: just select the pen, close any open submenu
                self.current_tool = TOOL_PEN
                self.submenu_open = None
                self.submenu_items = []
            else:
                # Already selected — toggle submenu
                if self.submenu_open == "pen_options":
                    self.submenu_open = None
                else:
                    self.submenu_open = "pen_options"
                self.submenu_items = []
        elif name == "text":
            if self._passthrough_mode:
                self._exit_passthrough()
            self._cursor_zone = None
            if self.current_tool != TOOL_TEXT:
                self.current_tool = TOOL_TEXT
                self.submenu_open = None
                self.submenu_items = []
            else:
                if self.submenu_open == "text_options":
                    self.submenu_open = None
                else:
                    self.submenu_open = "text_options"
                self.submenu_items = []
        elif name == "eraser":
            if self._passthrough_mode:
                self._exit_passthrough()
            self._cursor_zone = None
            if self.current_tool != TOOL_ERASER:
                # First click: just select the eraser, close any open submenu
                self.current_tool = TOOL_ERASER
                self.submenu_open = None
                self.submenu_items = []
            else:
                # Already selected — toggle submenu
                if self.submenu_open == "eraser_options":
                    self.submenu_open = None
                else:
                    self.submenu_open = "eraser_options"
                self.submenu_items = []
        elif name in tool_names:
            self._select_tool(name)
            self.submenu_open = None
            self.submenu_items = []
        elif name == "undo":
            self._undo()
        elif name == "redo":
            self._redo()
        elif name == "clear":
            self._clear_canvas()
        elif name == "cursor":
            self._enter_passthrough()
        elif name == "close":
            self._hide_overlay()
        self._schedule_draw()
        
        # If we are in passthrough mode, clicking buttons like pen/eraser might 
        # have changed the submenu state, requiring an input shape update.
        if self._passthrough_mode:
            self._update_input_shape()

    def _handle_submenu_click(self, item):
        if item["type"] == "color":
            self.current_color = item["value"]
        elif item["type"] == "pen_stroke":
            self.stroke_width = item["value"]
        elif item["type"] == "eraser_radius":
            self.eraser_radius = item["value"]
        elif item["type"] == "text_bg_color":
            self.text_bg_color = item["value"]
        elif item["type"] == "text_size":
            self.text_size = item["value"]
        self._schedule_draw()

    def _select_tool(self, tool):
        if self.is_typing:
            self._commit_text()
        self.current_tool = tool
        self.submenu_open = None
        self.submenu_items = []
        self._cursor_zone = None  # force cursor update for new tool
        if self._passthrough_mode:
            self._exit_passthrough()
        self._schedule_draw()

    def _undo(self):
        if self.strokes:
            self.redo_stack.append(self.strokes.pop())
            self._rebuild_canvas()
            self._schedule_draw()

    def _redo(self):
        if self.redo_stack:
            stroke = self.redo_stack.pop()
            self.strokes.append(stroke)
            cr = cairo.Context(self.canvas_surface)
            stroke.draw(cr)
            self._schedule_draw()

    def _clear_canvas(self):
        self.strokes.clear()
        self.redo_stack.clear()
        self._rebuild_canvas()
        self._schedule_draw()

    # ── Visibility ────────────────────────────────────────────────────────

    def _update_input_shape(self):
        """Update the window's input shape region based on passthrough mode."""
        window = self.get_window()
        if not window:
            return

        if not self._passthrough_mode:
            # Full screen input
            region = cairo.Region(cairo.RectangleInt(0, 0, self.mon_width, self.mon_height))
            window.input_shape_combine_region(region, 0, 0)
            return

        # Passthrough mode: Only toolbar and submenu are clickable
        region = cairo.Region()

        first = None
        last = None
        for b in self.toolbar_buttons:
            if not b["name"].startswith("__sep"):
                if first is None:
                    first = b
                last = b
                
        if first and last:
            bar_x = first["x"] - 10
            bar_w = (last["x"] + last["w"]) - first["x"] + 20
            # Include drop shadow bounds
            region.union(cairo.RectangleInt(int(bar_x - 6), 0, int(bar_w + 12), TOOLBAR_HEIGHT + 6))

        if self.submenu_open:
            if self.submenu_open == "eraser_options":
                region.union(cairo.RectangleInt(0, TOOLBAR_HEIGHT, self.mon_width, 90))
            elif self.submenu_open == "text_options":
                region.union(cairo.RectangleInt(0, TOOLBAR_HEIGHT, self.mon_width, 180))
            else:
                region.union(cairo.RectangleInt(0, TOOLBAR_HEIGHT, self.mon_width, 130))

        window.input_shape_combine_region(region, 0, 0)

    def _toggle_overlay(self):
        if self._passthrough_mode:
            self._exit_passthrough()
        elif self.is_visible:
            self._hide_overlay()
        else:
            self._show_overlay()

    def _show_overlay(self):
        self.is_visible = True
        self.set_keep_above(True)
        self.show_all()
        self.present()
        window = self.get_window()
        if window:
            window.set_cursor(self._get_cursor("crosshair"))
        # Start periodic keep-alive to re-assert always-on-top
        self._start_focus_keepalive()

    def _hide_overlay(self):
        self.is_visible = False
        self._passthrough_mode = False
        self.submenu_open = None
        self.submenu_items = []
        self._stop_focus_keepalive()
        # Reset input shape before hiding
        self._update_input_shape()
        self.hide()

    def _enter_passthrough(self):
        """Make overlay click-through so the user can interact with content below."""
        self._passthrough_mode = True
        self.submenu_open = None
        self.submenu_items = []
        self._update_input_shape()
        self._schedule_draw()

    def _exit_passthrough(self):
        """Restore input handling on the overlay."""
        self._passthrough_mode = False
        self._update_input_shape()
        window = self.get_window()
        if window:
            window.set_cursor(self._get_cursor("crosshair"))
        self._cursor_zone = None  # force cursor zone re-evaluation
        self._schedule_draw()

    def _on_focus_out(self, widget, event):
        """Re-assert always-on-top when the window loses focus."""
        if self.is_visible:
            self.set_keep_above(True)
            # Re-present on next idle to reclaim top position
            GLib.idle_add(self._reassert_top)
        return False

    def _reassert_top(self):
        """Idle callback to re-assert window on top after focus loss."""
        if self.is_visible:
            self.set_keep_above(True)
            # Avoid self.present() here as it triggers focus-stealing prevention
            # in GNOME/Mutter, which ironically sinks the window.
            window = self.get_window()
            if window:
                window.raise_()
        return False  # don't repeat

    def _start_focus_keepalive(self):
        """Start a periodic timer that re-asserts keep_above every 500ms."""
        self._stop_focus_keepalive()
        self._focus_keepalive_id = GLib.timeout_add(500, self._focus_keepalive_tick)

    def _stop_focus_keepalive(self):
        """Stop the focus keep-alive timer."""
        if self._focus_keepalive_id is not None:
            GLib.source_remove(self._focus_keepalive_id)
            self._focus_keepalive_id = None

    def _focus_keepalive_tick(self):
        """Periodic tick to re-assert always-on-top."""
        if not self.is_visible:
            self._focus_keepalive_id = None
            return False  # stop timer
        self.set_keep_above(True)
        return True  # keep ticking

    def _on_destroy(self, widget):
        self._stop_focus_keepalive()


# ─── GNOME Custom Keybinding Setup ───────────────────────────────────────────

def _find_keybinding_slot():
    """Find or create a custom keybinding slot for Screen Draw."""
    try:
        result = subprocess.run(
            ["gsettings", "get", GS_SCHEMA, GS_KEY],
            capture_output=True, text=True
        )
        current = result.stdout.strip()

        # Find existing screen-draw binding
        if KEYBINDING_SLOT in current:
            # Already registered
            return None

        # Find next available slot
        if current == "@as []" or current == "[]":
            paths = []
        else:
            # Parse the GVariant array
            paths = [p.strip().strip("'") for p in
                     current.strip("[]").split(",") if p.strip()]

        new_path = f"{KEYBINDING_PATH_PREFIX}{KEYBINDING_SLOT}/"
        paths.append(new_path)
        return (paths, new_path)
    except Exception as e:
        print(f"[warn] Could not read keybindings: {e}")
        return None


def setup_global_hotkey():
    """Register F9 as a global hotkey via GNOME custom keybindings."""
    toggle_cmd = (
        f"gdbus call --session --dest {DBUS_NAME} "
        f"--object-path {DBUS_PATH} "
        f"--method {DBUS_IFACE}.Toggle"
    )

    slot_info = _find_keybinding_slot()
    if slot_info is None:
        # Check if our binding already exists and update command
        binding_path = f"{KEYBINDING_PATH_PREFIX}{KEYBINDING_SLOT}/"
        try:
            subprocess.run(
                ["gsettings", "set",
                 f"{GS_CUSTOM_SCHEMA}:{binding_path}",
                 "command", toggle_cmd],
                check=True, capture_output=True
            )
        except Exception:
            pass
        print("[✓] F9 hotkey already registered")
        return

    paths, new_path = slot_info

    try:
        # Set the custom keybinding properties
        subprocess.run(
            ["gsettings", "set",
             f"{GS_CUSTOM_SCHEMA}:{new_path}",
             "name", "Screen Draw Toggle"],
            check=True, capture_output=True
        )
        subprocess.run(
            ["gsettings", "set",
             f"{GS_CUSTOM_SCHEMA}:{new_path}",
             "command", toggle_cmd],
            check=True, capture_output=True
        )
        subprocess.run(
            ["gsettings", "set",
             f"{GS_CUSTOM_SCHEMA}:{new_path}",
             "binding", "F9"],
            check=True, capture_output=True
        )

        # Register the new slot in the list
        paths_str = "[" + ", ".join(f"'{p}'" for p in paths) + "]"
        subprocess.run(
            ["gsettings", "set", GS_SCHEMA, GS_KEY, paths_str],
            check=True, capture_output=True
        )

        print("[✓] F9 hotkey registered via GNOME custom keybindings")
    except subprocess.CalledProcessError as e:
        print(f"[warn] Failed to register hotkey: {e}")
        print("       You can manually set F9 → 'gdbus call --session"
              f" --dest {DBUS_NAME} --object-path {DBUS_PATH}"
              f" --method {DBUS_IFACE}.Toggle'")


# ─── D-Bus Method Handler ─────────────────────────────────────────────────────

class ScreenDrawDBusService:
    """Expose Toggle/Show/Hide/Quit methods over D-Bus."""

    def __init__(self, window):
        self.window = window
        self.node_info = Gio.DBusNodeInfo.new_for_xml(DBUS_XML)

        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        try:
            # Newer GLib API (avoids deprecation warning)
            self.registration_id = self.bus.register_object_with_closures(
                DBUS_PATH,
                self.node_info.interfaces[0],
                self._on_method_call,
                None,
                None,
            )
        except AttributeError:
            # Fallback for older versions
            self.registration_id = self.bus.register_object(
                DBUS_PATH,
                self.node_info.interfaces[0],
                self._on_method_call,
                None,
                None,
            )

        # Own the bus name
        self.owner_id = Gio.bus_own_name_on_connection(
            self.bus,
            DBUS_NAME,
            Gio.BusNameOwnerFlags.NONE,
            None,
            None,
        )

    def _on_method_call(self, connection, sender, path, iface, method, params, invocation):
        if method == "Toggle":
            GLib.idle_add(self.window._toggle_overlay)
        elif method == "Show":
            GLib.idle_add(self.window._show_overlay)
        elif method == "Hide":
            GLib.idle_add(self.window._hide_overlay)
        elif method == "Quit":
            GLib.idle_add(Gtk.main_quit)
        invocation.return_value(None)


# ─── Entry Point ──────────────────────────────────────────────────────────────

def main():
    print("┌─────────────────────────────────────┐")
    print("│  Screen Draw — Annotation Overlay   │")
    print("├─────────────────────────────────────┤")
    print("│  F9       Toggle overlay            │")
    print("│  P        Pen tool                  │")
    print("│  E        Eraser tool               │")
    print("│  T        Text tool                 │")
    print("│  L/R/O/A  Line/Rect/Circle/Arrow    │")
    print("│  Ctrl+Z   Undo                      │")
    print("│  Ctrl+Y   Redo                      │")
    print("│  Ctrl+X   Clear canvas              │")
    print("│  C        Interact mode             │")
    print("│  Esc      Close submenu             │")
    print("└─────────────────────────────────────┘")

    # Handle SIGINT gracefully
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    # Set up the global hotkey
    setup_global_hotkey()

    app = ScreenDrawApp()
    app.run(None)


if __name__ == "__main__":
    main()
