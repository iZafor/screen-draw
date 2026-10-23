# Screen Draw

A blazing-fast native C++17 / GTK3 / Cairo screen annotation overlay for Linux (GNOME/Wayland).

Draw directly over your screen with pens, shapes, text, and erasers — and freely **move** or **resize** drawn shapes — toggle it on/off with **F9**.

## Features

- **Native C++17 & Cairo engine**: Offscreen toolbar caching and dirty-region partial invalidation (`gtk_widget_queue_draw_area`) for zero-lag drawing and manipulation at high refresh rates
- **Toggle overlay** with `F9` (via GNOME custom keybindings + D-Bus)
- **Movable & Resizable Shapes**:
  - **Select / Move & Resize tool (`S` or `V`)**: Click and drag any shape, text, or stroke to move it anywhere on screen
  - **Interactive Resize Handles**: Newly drawn shapes are automatically selected with interactive handles — drag any of the 8 bounding-box handles (`NW`, `N`, `NE`, `E`, `SE`, `S`, `SW`, `W`) on Rectangles, Circles/Ellipses, or Freehand strokes, the 2 endpoint handles on Lines and Arrows, or the 4 corner handles on Text to resize on the fly
  - **Live Style Editing & Deletion**: Change the color, stroke width, or font size of a selected shape directly from the submenu, or press `Delete` / `BackSpace` to remove it
- **Pen tool** with customizable color and stroke width (click pen icon for options)
- **Eraser tool** with live erasing and visual cursor circle
- **Text tool** for adding text annotations with customizable color, background color, and font size
- **Predefined shapes**: Rectangle, Circle/Ellipse, Arrow, Line
- **Undo/Redo** (`Ctrl+Z` / `Ctrl+Y`) across drawing, moving, resizing, restyling, and deleting
- **Clear canvas** shortcut (`Ctrl+X`)
- Smooth Bézier curve rendering for freehand strokes
- **Interact mode (`C`)**: Click-through the overlay to interact with other apps while keeping the toolbar accessible
- Dark, floating toolbar with modern glassmorphic design and hover tooltips
- Runs on **GNOME/Wayland** (Fedora, Ubuntu, etc.)

## Build Requirements

- C++17 compiler (`g++`) and `make`
- `pkg-config`, `gtk+-3.0` (`gtk3-devel` / `libgtk-3-dev`), and `cairo`
- GNOME desktop (for custom keybinding registration)

## Building & Installation (Fedora)

Compile the native binary directly with `make`:

```bash
make
```

Or run the included installation script to install dependencies, compile the C++ binary, copy it to `/usr/local/bin/screen-draw`, and create an autostart desktop entry:

```bash
./install.sh
```

To uninstall the application, run:

```bash
./uninstall.sh
```

## AppImage Release

Build an AppImage with:

```bash
./packaging/appimage/build-appimage.sh
```

The script compiles `screen-draw`, creates an AppDir under `build/appimage/`, and writes the final AppImage to `dist/`.
It requires `appimagetool` on `PATH`; download it from the AppImage project releases if your distribution does not package it.

You can set the release version explicitly:

```bash
VERSION=1.0.0 ./packaging/appimage/build-appimage.sh
```

For offline or CI builds, the script also accepts `APPIMAGETOOL=/path/to/appimagetool`, `APPIMAGE_RUNTIME_FILE=/path/to/runtime`, and `APPIMAGE_NO_APPSTREAM_CHECK=1`.

Run the AppImage directly:

```bash
chmod +x Screen_Draw-*.AppImage
./Screen_Draw-*.AppImage
```

## Usage

Run the app from your application launcher or by executing `./screen-draw` (or `screen-draw` if installed) in the terminal.

The app starts hidden. Press **F9** to toggle the drawing overlay.

## Architecture

- **Global Hotkey**: Registered via GNOME custom keybindings (`gsettings`). Pressing F9 triggers a `gdbus` call to the app's D-Bus service.
- **D-Bus Service**: Exposes `Toggle`, `Show`, `Hide`, and `Quit` methods at `com.tools.ScreenDraw`.
- **Drawing & Performance**: Runs in native C++17 via XWayland (`GDK_BACKEND=x11`) so the transparent overlay stays always-on-top in GNOME Wayland. Uses dirty-rectangle clipping (`gdk_cairo_get_clip_rectangle`), an offscreen cached toolbar surface, and an in-place cleared persistent canvas surface.

## Keyboard Shortcuts

| Key               | Action                                      |
|-------------------|---------------------------------------------|
| `F9`              | Toggle overlay (global)                     |
| `S` / `V`         | Select, Move & Resize tool                  |
| `P`               | Pen tool                                    |
| `E`               | Eraser tool                                 |
| `T`               | Text tool                                   |
| `L`               | Line shape                                  |
| `R`               | Rectangle shape                             |
| `O`               | Circle/oval shape                           |
| `A`               | Arrow shape                                 |
| `Delete`/`BackSpace` | Delete currently selected shape          |
| `Ctrl+Z`          | Undo                                        |
| `Ctrl+Y`          | Redo                                        |
| `Ctrl+X`          | Clear canvas                                |
| `C`               | Interact (click-through) mode               |
| `Escape`          | Deselect shape / Close open submenu         |

## D-Bus Control

You can also control Screen Draw programmatically:

```bash
# Toggle the overlay
gdbus call --session --dest com.tools.ScreenDraw \
  --object-path /com/tools/ScreenDraw \
  --method com.tools.ScreenDraw.Toggle

# Quit the app
gdbus call --session --dest com.tools.ScreenDraw \
  --object-path /com/tools/ScreenDraw \
  --method com.tools.ScreenDraw.Quit
```

## License

MIT
