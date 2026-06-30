#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APPDIR="$ROOT_DIR/build/appimage/ScreenDraw.AppDir"
DIST_DIR="$ROOT_DIR/dist"
VERSION="${VERSION:-$(git -C "$ROOT_DIR" describe --tags --always --dirty 2>/dev/null || echo dev)}"
ARCH="${ARCH:-$(uname -m)}"
export ARCH

case "$ARCH" in
    x86_64|aarch64)
        ;;
    *)
        echo "Unsupported AppImage architecture: $ARCH" >&2
        echo "Set ARCH=x86_64 or ARCH=aarch64 if you are cross-building." >&2
        exit 1
        ;;
esac

rm -rf "$APPDIR"
mkdir -p \
    "$APPDIR/usr/bin" \
    "$APPDIR/usr/share/applications" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
    "$APPDIR/usr/share/metainfo" \
    "$DIST_DIR"

cp "$ROOT_DIR/screen_draw.py" "$APPDIR/usr/bin/screen-draw"
cp "$ROOT_DIR/packaging/appimage/AppRun" "$APPDIR/AppRun"
cp "$ROOT_DIR/packaging/appimage/screen-draw.desktop" "$APPDIR/screen-draw.desktop"
cp "$ROOT_DIR/packaging/appimage/screen-draw.desktop" "$APPDIR/usr/share/applications/screen-draw.desktop"
cp "$ROOT_DIR/packaging/appimage/screen-draw.svg" "$APPDIR/screen-draw.svg"
cp "$ROOT_DIR/packaging/appimage/screen-draw.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/screen-draw.svg"
cp "$ROOT_DIR/packaging/appimage/io.github.iZafor.ScreenDraw.metainfo.xml" "$APPDIR/usr/share/metainfo/io.github.iZafor.ScreenDraw.metainfo.xml"
chmod 0755 "$APPDIR/AppRun" "$APPDIR/usr/bin/screen-draw" 2>/dev/null || true
ln -sf "usr/share/icons/hicolor/scalable/apps/screen-draw.svg" "$APPDIR/.DirIcon"

APPIMAGETOOL="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
if [ -z "$APPIMAGETOOL" ]; then
    echo "AppDir prepared at: $APPDIR" >&2
    echo "Install appimagetool, then rerun this script to create the AppImage." >&2
    echo "Or set APPIMAGETOOL=/path/to/appimagetool." >&2
    echo "Download: https://github.com/AppImage/appimagetool/releases" >&2
    exit 127
fi

OUTPUT="$DIST_DIR/Screen_Draw-${VERSION}-${ARCH}.AppImage"
APPIMAGETOOL_ARGS=()
if [ "${APPIMAGE_NO_APPSTREAM_CHECK:-0}" = "1" ]; then
    APPIMAGETOOL_ARGS+=(-n)
fi
if [ -n "${APPIMAGE_RUNTIME_FILE:-}" ]; then
    APPIMAGETOOL_ARGS+=(--runtime-file "$APPIMAGE_RUNTIME_FILE")
fi

"$APPIMAGETOOL" "${APPIMAGETOOL_ARGS[@]}" "$APPDIR" "$OUTPUT"
chmod +x "$OUTPUT" 2>/dev/null || true

echo "Built: $OUTPUT"
