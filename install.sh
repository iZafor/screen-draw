#!/bin/bash
# install.sh — Install dependencies for Screen Draw on Fedora

set -e

echo "╔══════════════════════════════════════════╗"
echo "║       Screen Draw — Install Script       ║"
echo "╚══════════════════════════════════════════╝"
echo ""

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

echo "[*] Installing application..."
sudo cp screen_draw.py /usr/local/bin/screen-draw
sudo chmod +x /usr/local/bin/screen-draw

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

echo ""
echo "[✓] Screen Draw installed successfully!"
echo ""
echo "Usage:"
echo "  Run 'screen-draw' from your app drawer or terminal."
echo ""
echo "Press F9 to toggle the drawing overlay."
