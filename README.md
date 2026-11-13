# Screen Draw

A blazing-fast native C++17 / Xlib / Cairo screen annotation overlay for Linux (GNOME/Wayland via XWayland, or any X11 desktop). No GTK or GLib required.

Draw directly over your screen with pens, shapes, text, and erasers — and freely **move** or **resize** drawn shapes — toggle it on/off with **F9**.

## Features

- **Native C++17 & Cairo engine**: Offscreen toolbar caching and dirty-region partial invalidation with a shared-memory (MIT-SHM) back buffer for zero-lag drawing and manipulation at high refresh rates
- **Toggle overlay** with `F9` (via GNOME custom keybindings + `screen-draw --toggle`)
- **Movable & Resizable Shapes**:
  - **Select / Move & Resize tool (`S` or `V`)**: Click and drag any shape, text, or stroke to move it anywhere on screen
  - **Interactive Resize Handles**: Newly drawn shapes are automatically selected with interactive handles — drag any of the 8 bounding-box handles (`NW`, `N`, `NE`, `E`, `SE`, `S`, `SW`, `W`) on Rectangles, Circles/Ellipses, or Freehand strokes, the 2 endpoint handles on Lines and Arrows, or the 4 corner handles on Text to resize on the fly
  - **Live Style Editing & Deletion**: Change the color, stroke width, or font size of a selected shape directly from the submenu, or press `Delete` / `BackSpace` to remove it
- **Pen tool** with customizable color and stroke width (click pen icon for options)
- **Eraser tool** with live erasing and visual cursor circle
- **Text tool** for adding text annotations with customizable color, background color, and font size
  - **Edit existing text**: double-click a text annotation (any tool), click it with the Text tool, or select it and press `Enter` / `F2`. Reopening places the cursor at the exact clicked location
  - **Full cursor navigation**: click anywhere within the text or drag to position the cursor; navigate with `Left` / `Right` arrows, `Home` / `End` / `Up` / `Down`, and `Ctrl+Left` / `Ctrl+Right` (by word)
  - **In-place editing & deletion**: edit or insert text at the cursor position; delete with `BackSpace`, `Delete`, `Ctrl+BackSpace`, and `Ctrl+Delete`. Clearing all text removes the item; edits are undoable
  - **LaTeX math support (`$...$` / `$$...$$`)**: write LaTeX math commands inside `$...$` or `$$...$$` (e.g., `$$\frac{a}{b}$$`, `$\alpha + \beta$`, `\sqrt`, superscripts/subscripts, `\sum`, `\int`, `\left( ... \right)`, matrices, accents, `\mathbb`, `\text`, and `\color`) with live preview while typing and full raw-source editing on double-click / `F2`
- **Predefined shapes**: Rectangle, Circle/Ellipse, Arrow, Line
- **Undo/Redo** (`Ctrl+Z` / `Ctrl+Y`) across drawing, moving, resizing, restyling, and deleting
- **Clear canvas** shortcut (`Ctrl+X`)
- Smooth Bézier curve rendering for freehand strokes
- **Interact mode (`C`)**: Click-through the overlay to interact with other apps while keeping the toolbar accessible
- Dark, floating toolbar with modern glassmorphic design and hover tooltips
- Runs on **GNOME/Wayland** (Fedora, Ubuntu, etc.)

## Build Requirements

- C++17 compiler (`g++`) and `make`
- `pkg-config`, `cairo`, `x11`, `xext`, `xrandr`
  - Fedora: `cairo-devel libX11-devel libXext-devel libXrandr-devel`
  - Debian/Ubuntu: `libcairo2-dev libx11-dev libxext-dev libxrandr-dev`
- An X server or XWayland, and a compositor for transparency
- GNOME desktop (optional, for automatic F9 keybinding registration; on other desktops bind a key to `screen-draw --toggle`)

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

- **Global Hotkey**: Registered via GNOME custom keybindings (`gsettings`, if available). Pressing F9 runs `screen-draw --toggle`.
- **Single-instance control**: The running instance listens on a Unix socket (`$XDG_RUNTIME_DIR/screen-draw.sock`); launching `screen-draw` again forwards `toggle` / `show` / `hide` / `quit` to it.
- **Drawing & Performance**: Talks to the X server directly with Xlib (XWayland on GNOME Wayland) so the transparent overlay stays always-on-top. Uses a 32-bit ARGB window, accumulated dirty-region repainting into a MIT-SHM back buffer, an offscreen cached toolbar surface, and an in-place cleared persistent canvas surface. Click-through uses the XShape input region.

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
| `Left` / `Right`  | Move cursor 1 character while typing text   |
| `Ctrl+Left` / `Ctrl+Right` | Move cursor 1 word while typing text |
| `Home` / `End`    | Move cursor to start / end of text line     |
| `BackSpace` / `Delete` | Delete character before / after cursor |
| `Ctrl+BackSpace` / `Ctrl+Delete` | Delete word before / after cursor |
| `Enter` / `F2`    | Edit the selected text annotation           |
| `Ctrl+Z`          | Undo                                        |
| `Ctrl+Y`          | Redo                                        |
| `Ctrl+X`          | Clear canvas                                |
| `C`               | Interact (click-through) mode               |
| `Escape`          | Deselect shape / Close open submenu         |

## Command-Line Control

You can also control a running instance from scripts or your own keybindings:

```bash
screen-draw            # start in the background (or toggle if already running)
screen-draw --toggle   # toggle the overlay (starts the app if needed)
screen-draw --show     # show the overlay
screen-draw --hide     # hide the overlay
screen-draw --quit     # quit the running instance
```

## License

MIT
