#!/bin/bash
# uninstall.sh — Uninstall Screen Draw

set -e

echo "╔══════════════════════════════════════════╗"
echo "║      Screen Draw — Uninstall Script      ║"
echo "╚══════════════════════════════════════════╝"
echo ""

echo "[*] Removing application..."
if [ -f /usr/local/bin/screen-draw ]; then
    sudo rm /usr/local/bin/screen-draw
    echo "  Removed /usr/local/bin/screen-draw"
fi

echo "[*] Removing desktop entry..."
if [ -f /usr/share/applications/screen-draw.desktop ]; then
    sudo rm /usr/share/applications/screen-draw.desktop
    echo "  Removed /usr/share/applications/screen-draw.desktop"
fi

echo ""
echo "[✓] Screen Draw uninstalled successfully!"
