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

echo ""
echo "[✓] Dependencies installed successfully!"
echo ""
echo "Usage:"
echo "  python3 screen_draw.py"
echo ""
echo "Press F9 to toggle the drawing overlay."
