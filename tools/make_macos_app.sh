#!/bin/bash
# Assemble a self-contained, ad-hoc-signed KeeperFX.app for Apple Silicon.
#
# The binary produced by macos.mk links against Homebrew dylibs by absolute
# path (/opt/homebrew/...). This script copies those dylibs into the bundle and
# rewrites the load paths (via dylibbundler) so the .app runs on any arm64 Mac
# with no Homebrew installed.
#
# The app is a "drop-in" engine: the engine is the bundle's main executable and,
# on startup, chdir's to the folder CONTAINING the .app (see
# macos_chdir_to_bundle_parent in src/linux.cpp). So a user drops KeeperFX.app
# next to their KeeperFX game data (the folder with data/, sound/, fxdata/,
# campgns/, ...) and double-clicks it.
#
# Because the engine is a real signed main executable (not a script wrapper) and
# the Info.plist declares folder-access usage strings, macOS shows a normal
# permission prompt the first time it reads data from a privacy-protected folder
# (Desktop/Documents/Downloads) — so it works from any location once allowed.
#
# Usage: tools/make_macos_app.sh [path/to/keeperfx-binary] [output-dir]
#   defaults: bin/keeperfx   ->   dist/KeeperFX.app
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/bin/keeperfx}"
OUTDIR="${2:-$ROOT/dist}"
APP="$OUTDIR/KeeperFX.app"

if [ ! -x "$BIN" ]; then
    echo "error: binary not found or not executable: $BIN" >&2
    echo "build it first: make -f macos.mk" >&2
    exit 1
fi
if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "error: dylibbundler not found (brew install dylibbundler)" >&2
    exit 1
fi

VER_MAJOR=$(grep -E '^VER_MAJOR=' "$ROOT/version.mk" | cut -d= -f2)
VER_MINOR=$(grep -E '^VER_MINOR=' "$ROOT/version.mk" | cut -d= -f2)
VER_RELEASE=$(grep -E '^VER_RELEASE=' "$ROOT/version.mk" | cut -d= -f2)
VERSION="${VER_MAJOR}.${VER_MINOR}.${VER_RELEASE}"

echo "== assembling $APP (version $VERSION) =="
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/libs"

# The engine is the bundle's main executable (CFBundleExecutable=keeperfx).
cp "$BIN" "$APP/Contents/MacOS/keeperfx"
chmod +x "$APP/Contents/MacOS/keeperfx"

# Bundle & fix all non-system dylibs into Contents/libs, rewriting the binary's
# load paths to @executable_path/../libs.
dylibbundler \
    --overwrite-dir \
    --bundle-deps \
    --fix-file "$APP/Contents/MacOS/keeperfx" \
    --dest-dir "$APP/Contents/libs" \
    --install-path "@executable_path/../libs"

# Homebrew's `sdl2` is `sdl2-compat`: a thin libSDL2 that dlopens SDL3 at
# runtime. Because that load is dynamic (not a link-time dependency), the pass
# above can't see it and SDL3 is left out — the shim then fails in its
# initializer. The shim's first search path is `@loader_path/libSDL3.dylib`
# (unversioned), i.e. right next to libSDL2 in Contents/libs. Bundle libSDL3
# under exactly that name and the shim finds it with no env vars or rpath.
SDL3_SRC="$(brew --prefix sdl3 2>/dev/null)/lib/libSDL3.0.dylib"
if [ -f "$APP/Contents/libs/libSDL2-2.0.0.dylib" ] && [ -f "$SDL3_SRC" ] \
   && [ ! -f "$APP/Contents/libs/libSDL3.dylib" ]; then
    echo "== bundling libSDL3 (sdl2-compat runtime dependency) =="
    cp "$SDL3_SRC" "$APP/Contents/libs/libSDL3.dylib"
    chmod u+w "$APP/Contents/libs/libSDL3.dylib"
    install_name_tool -id "@executable_path/../libs/libSDL3.dylib" \
        "$APP/Contents/libs/libSDL3.dylib"
fi

# Icon (optional): build a .icns from the repo's PNG icon set via iconutil.
ICON_KEY=''
if command -v iconutil >/dev/null 2>&1 && [ -f "$ROOT/res/keeperfx_icon016-08bpp.png" ]; then
    ICONSET="$(mktemp -d)/KeeperFX.iconset"
    mkdir -p "$ICONSET"
    cp "$ROOT/res/keeperfx_icon016-08bpp.png" "$ICONSET/icon_16x16.png"
    cp "$ROOT/res/keeperfx_icon032-08bpp.png" "$ICONSET/icon_16x16@2x.png"
    cp "$ROOT/res/keeperfx_icon032-08bpp.png" "$ICONSET/icon_32x32.png"
    cp "$ROOT/res/keeperfx_icon064-08bpp.png" "$ICONSET/icon_32x32@2x.png"
    cp "$ROOT/res/keeperfx_icon128-24bpp.png" "$ICONSET/icon_128x128.png"
    cp "$ROOT/res/keeperfx_icon256-24bpp.png" "$ICONSET/icon_128x128@2x.png"
    cp "$ROOT/res/keeperfx_icon256-24bpp.png" "$ICONSET/icon_256x256.png"
    cp "$ROOT/res/keeperfx_icon512-24bpp.png" "$ICONSET/icon_256x256@2x.png"
    cp "$ROOT/res/keeperfx_icon512-24bpp.png" "$ICONSET/icon_512x512.png"
    if iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/KeeperFX.icns" 2>/dev/null; then
        ICON_KEY='<key>CFBundleIconFile</key><string>KeeperFX</string>'
    fi
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>KeeperFX</string>
    <key>CFBundleDisplayName</key><string>KeeperFX</string>
    <key>CFBundleIdentifier</key><string>com.keeperfx.keeperfx</string>
    <key>CFBundleVersion</key><string>${VERSION}</string>
    <key>CFBundleShortVersionString</key><string>${VERSION}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleExecutable</key><string>keeperfx</string>
    ${ICON_KEY}
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSDesktopFolderUsageDescription</key><string>KeeperFX needs to read its game data files (data, sound, campaigns) from the folder it was placed in.</string>
    <key>NSDocumentsFolderUsageDescription</key><string>KeeperFX needs to read its game data files (data, sound, campaigns) from the folder it was placed in.</string>
    <key>NSDownloadsFolderUsageDescription</key><string>KeeperFX needs to read its game data files (data, sound, campaigns) from the folder it was placed in.</string>
    <key>NSRemovableVolumesUsageDescription</key><string>KeeperFX needs to read its game data files (data, sound, campaigns) from the folder it was placed in.</string>
</dict>
</plist>
PLIST

echo 'APPL????' > "$APP/Contents/PkgInfo"

# Ad-hoc code signatures. arm64 requires every Mach-O to carry at least an
# ad-hoc signature to execute. Sign the bundled dylibs first (inside-out), then
# sign the whole bundle: that seals the engine main executable and gives macOS a
# stable app identity to attach the user's folder-access grant to (needed for
# the privacy prompt described in the Info.plist usage strings).
find "$APP/Contents/libs" -name '*.dylib' -exec codesign --force --sign - {} +
codesign --force --sign - "$APP"

echo "== verifying no Homebrew paths remain =="
if otool -L "$APP/Contents/MacOS/keeperfx" | grep -q "/opt/homebrew"; then
    echo "error: binary still references /opt/homebrew — bundling incomplete" >&2
    otool -L "$APP/Contents/MacOS/keeperfx" | grep "/opt/homebrew" >&2
    exit 1
fi

echo "== done: $APP =="
echo "   bundled dylibs: $(ls "$APP/Contents/libs" | wc -l | tr -d ' ')"
# Verify the whole bundle carries a valid signature (this is what macOS checks
# at launch and what the TCC privacy grant is attributed to).
codesign --verify --verbose=2 "$APP" \
    && echo "   bundle: signed & valid (ad-hoc)"
