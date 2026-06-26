#!/bin/bash
# install.sh — Install or update Screen Draw on Fedora

set -e

UPDATE_ONLY=0
for arg in "$@"; do
    if [ "$arg" == "--update" ] || [ "$arg" == "-u" ]; then
        UPDATE_ONLY=1
    fi
done

# Auto-detect update if the executable already exists
if [ -x /usr/local/bin/screen-draw ]; then
    UPDATE_ONLY=1
fi

echo "╔══════════════════════════════════════════╗"
if [ $UPDATE_ONLY -eq 1 ]; then
    echo "║        Screen Draw — Update Script       ║"
else
    echo "║       Screen Draw — Install Script       ║"
fi
echo "╚══════════════════════════════════════════╝"
echo ""

if [ $UPDATE_ONLY -eq 1 ]; then
    echo "[*] Closing any running instances of Screen Draw..."
    pkill -f "screen-draw" || true
    pkill -f "screen_draw.py" || true
fi

if [ $UPDATE_ONLY -eq 0 ]; then
    # Check if running on Fedora
    if [ -f /etc/fedora-release ]; then
        echo "[*] Detected Fedora"
    else
        echo "[!] This script is designed for Fedora. Adjust package names for your distro."
    fi

    echo "[*] Installing dependencies..."
    sudo dnf install -y \
        python3-gobject \
        gtk3 \
        gtk-layer-shell \
        keybinder3
fi

echo "[*] Copying application files..."
sudo cp screen_draw.py /usr/local/bin/screen-draw
sudo chmod +x /usr/local/bin/screen-draw

if [ $UPDATE_ONLY -eq 0 ]; then
    echo "[*] Creating desktop entry..."
    cat <<EOF | sudo tee /usr/share/applications/screen-draw.desktop > /dev/null
[Desktop Entry]
Name=Screen Draw
Comment=Draw annotations over your screen
Exec=screen-draw
Icon=applications-graphics
Terminal=false
Type=Application
Categories=Utility;Graphics;
Keywords=draw;annotate;screen;overlay;
StartupNotify=false
EOF
fi

echo ""
if [ $UPDATE_ONLY -eq 1 ]; then
    echo "[✓] Screen Draw updated successfully!"
else
    echo "[✓] Screen Draw installed successfully!"
fi
echo ""
echo "Usage:"
echo "  Run 'screen-draw' from your app drawer or terminal."
echo ""
echo "Press F9 to toggle the drawing overlay."
