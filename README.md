# Screen Draw

A lightweight screen annotation tool for Linux (Wayland/GNOME), inspired by Gromit-MPX.

Draw directly over your screen with pens, shapes, and erasers — toggle it on/off with a keyboard shortcut.

## Features

- **Toggle overlay** with `F9` (configurable)
- **Pen tool** with customizable color and stroke width
- **Eraser tool**
- **Predefined shapes**: Rectangle, Circle, Arrow, Line
- **Undo/Redo** support
- **Clear canvas** shortcut
- Runs natively on Wayland (GNOME) via `gtk-layer-shell`

## Requirements

- Python 3.10+
- GTK 3 (`python3-gobject`, `gtk3`)
- `gtk-layer-shell` (for Wayland overlay)
- `libkeybinder3` (for global hotkey)

### Install dependencies (Fedora)

```bash
sudo dnf install -y gtk-layer-shell gtk3-layer-shell python3-gobject keybinder3
```

## Usage

```bash
python3 screen_draw.py
```

Press **F9** to toggle the drawing overlay on/off.

## Keyboard Shortcuts

| Key       | Action                  |
|-----------|-------------------------|
| `F9`      | Toggle overlay          |
| `Ctrl+Z`  | Undo                    |
| `Ctrl+Y`  | Redo                    |
| `Ctrl+C`  | Clear canvas            |
| `Escape`  | Hide overlay            |

## License

MIT
