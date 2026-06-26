# Screen Draw

A lightweight screen annotation tool for Linux (GNOME/Wayland).

Draw directly over your screen with pens, shapes, and erasers — toggle it on/off with **F9**.

## Features

- **Toggle overlay** with `F9` (via GNOME custom keybindings + D-Bus)
- **Pen tool** with customizable color and stroke width (click pen icon for options)
- **Eraser tool** with visual cursor circle
- **Predefined shapes**: Rectangle, Circle, Arrow, Line
- **Undo/Redo** support
- **Clear canvas** shortcut
- Smooth Bézier curve rendering for freehand strokes
- **Interact mode**: Click-through the overlay to interact with other apps while keeping the toolbar accessible
- Dark, floating toolbar with modern glassmorphic design
- Runs natively on **GNOME/Wayland** (Fedora, Ubuntu, etc.)

## Requirements

- Python 3.10+
- GTK 3 (`python3-gobject`, `gtk3`)
- GNOME desktop (for custom keybinding registration)

## Installation (Fedora)

Run the included installation script to install dependencies, copy the app to `/usr/local/bin`, and create a desktop entry:

```bash
./install.sh
```

To uninstall the application, run:

```bash
./uninstall.sh
```

## Usage

Run the app from your application launcher or by executing `screen-draw` in the terminal.

The app starts hidden. Press **F9** to toggle the drawing overlay.

## Architecture

- **Global Hotkey**: Registered via GNOME custom keybindings (`gsettings`). Pressing F9 triggers a `gdbus` call to the app's D-Bus service.
- **D-Bus Service**: Exposes `Toggle`, `Show`, `Hide`, and `Quit` methods at `com.tools.ScreenDraw`.
- **Drawing**: Runs via XWayland (`GDK_BACKEND=x11`) to ensure the transparent overlay can securely stay always-on-top in GNOME Wayland. Uses Cairo on a fullscreen GTK window with `UTILITY` type hint.
- **Canvas**: Persistent `ImageSurface` with stroke-based undo/redo.

## Keyboard Shortcuts

| Key       | Action                     |
|-----------|----------------------------|
| `F9`      | Toggle overlay (global)    |
| `P`       | Pen tool                   |
| `E`       | Eraser tool                |
| `L`       | Line shape                 |
| `R`       | Rectangle shape            |
| `O`       | Circle/oval shape          |
| `A`       | Arrow shape                |
| `Ctrl+Z`  | Undo                       |
| `Ctrl+Y`  | Redo                       |
| `Ctrl+X`  | Clear canvas               |
| `C`       | Interact (click-through) mode|
| `Escape`  | Close open submenu         |

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
