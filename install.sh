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

echo "[*] Configuring autostart..."
sudo mkdir -p /etc/xdg/autostart
sudo cp /usr/share/applications/screen-draw.desktop /etc/xdg/autostart/screen-draw.desktop

echo "[*] Starting Screen Draw in background..."
if [ -n "$SUDO_USER" ]; then
    USER_ID=$(id -u "$SUDO_USER")
    sudo -u "$SUDO_USER" env \
        DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$USER_ID/bus" \
        XDG_RUNTIME_DIR="/run/user/$USER_ID" \
        WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
        DISPLAY="${DISPLAY:-:0}" \
        screen-draw >/dev/null 2>&1 &
else
    screen-draw >/dev/null 2>&1 &
fi

echo ""
if [ $UPDATE_ONLY -eq 1 ]; then
    echo "[✓] Screen Draw updated successfully!"
else
    echo "[✓] Screen Draw installed successfully!"
fi
echo ""
echo "Screen Draw is now running in the background and will start automatically on boot."
echo "Press F9 anywhere to toggle the drawing overlay!"
