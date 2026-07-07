#!/bin/bash
# Assemble a self-contained, ad-hoc-signed KeeperFX.app for Apple Silicon.
#
# macos.mk links against Homebrew dylibs by absolute path; this script bundles
# those dylibs and rewrites the load paths (dylibbundler) so the .app runs on any
# arm64 Mac with no Homebrew. The engine is the bundle's main executable and
# chdir's to the folder containing the .app on startup (macos_chdir_to_bundle_parent
# in src/linux.cpp), so users drop KeeperFX.app next to their game data and run it.
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

LIBS="$APP/Contents/libs"

# dylibbundler only follows LC_LOAD_DYLIB entries (what `otool -L` shows). Any
# library a dependency pulls in at runtime via dlopen()/SDL_LoadObject is
# invisible to it, so those must be added by hand — AND then re-processed so
# their own transitive dependencies get bundled and path-rewritten too. (The
# original SDL3 handling copied the file but skipped that second step, leaving
# any of SDL3's own non-system deps dangling at /opt/homebrew.)
#
# add_runtime_dylib <resolved-src-path> [dest-name]
add_runtime_dylib() {
    local src="$1" name="${2:-$(basename "$1")}"
    [ -f "$src" ] || return 0
    [ -f "$LIBS/$name" ] && return 0
    echo "== bundling runtime (dlopen'd) dylib: $name =="
    cp "$src" "$LIBS/$name"
    chmod u+w "$LIBS/$name"
    install_name_tool -id "@executable_path/../libs/$name" "$LIBS/$name"
    # Pull in and rewrite this library's own deps. --overwrite-files (not
    # --overwrite-dir, which would rm -r $LIBS first) leaves the rest of $LIBS intact.
    dylibbundler \
        --overwrite-files \
        --bundle-deps \
        --fix-file "$LIBS/$name" \
        --dest-dir "$LIBS" \
        --install-path "@executable_path/../libs" >/dev/null
}

# --- SDL3 (dlopen'd by the sdl2-compat shim as @loader_path/libSDL3.dylib) ---
# The shim's first search path is @loader_path, i.e. right next to libSDL2 in
# Contents/libs, so the unversioned name is what it looks for.
if [ -f "$LIBS/libSDL2-2.0.0.dylib" ]; then
    SDL3_PREFIX="$(brew --prefix sdl3 2>/dev/null)"
    # Glob the real SONAME instead of hardcoding a version: a future sdl3 major
    # bump would move libSDL3.0.dylib and otherwise ship a silently-broken bundle.
    SDL3_SRC="$(ls "$SDL3_PREFIX"/lib/libSDL3*.dylib 2>/dev/null | head -1)"
    if [ -z "$SDL3_SRC" ] || [ ! -f "$SDL3_SRC" ]; then
        echo "error: libSDL3 not found under '$SDL3_PREFIX' — the sdl2-compat shim dlopens it (brew install sdl3)" >&2
        exit 1
    fi
    add_runtime_dylib "$SDL3_SRC" "libSDL3.dylib"
    # The shim dlopens exactly @loader_path/libSDL3.dylib; a missing copy = crash on a non-Homebrew Mac.
    test -f "$LIBS/libSDL3.dylib" \
        || { echo "error: libSDL3.dylib was not bundled into Contents/libs" >&2; exit 1; }
fi

# --- SDL2_mixer audio decoders (loaded at runtime) ---------------------------
# SDL2_mixer pulls in its Ogg/FLAC/MP3/Opus/MOD decoders at runtime, so on a Mac
# without Homebrew they must be bundled or music goes silent. Mirror the decoder
# set into Contents/libs under both versioned and unversioned SONAMEs (mixer may
# request either); dylibbundler then rewrites each one's own dependencies.
if [ -f "$LIBS/libSDL2_mixer-2.0.0.dylib" ]; then
    BREW_LIB="$(brew --prefix)/lib"
    for stem in libvorbisfile libvorbis libogg libFLAC libmpg123 \
                libopusfile libopus libxmp libmodplug libwavpack libfluidsynth; do
        real="$(ls "$BREW_LIB/$stem".*.dylib 2>/dev/null | head -n1 || true)"
        [ -n "$real" ] || continue
        add_runtime_dylib "$real" "$(basename "$real")"   # versioned SONAME
        add_runtime_dylib "$real" "$stem.dylib"           # unversioned alias
    done
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

# Bundle KeeperFX's own shipped config defaults (fxdata/, creatrs/) into
# Contents/Resources. The engine records this dir (keeper_defaults_directory) and
# the file resolver falls back to it when a config file is missing from the user's
# game folder — so the app works even when dropped next to an OLDER KeeperFX data
# install that predates a config file this engine needs (e.g. fxdata/sounds.cfg).
# These are GPL KeeperFX files (not copyrighted DK assets), so shipping them is fine.
# Same source of truth `make package` deploys — no duplication.
for cfgdir in fxdata creatrs; do
    if [ -d "$ROOT/config/$cfgdir" ]; then
        # Clear any stale copy from a previous run, then copy the tree fresh
        # (Contents/Resources already exists from the mkdir -p above).
        rm -rf "$APP/Contents/Resources/$cfgdir"
        cp -R "$ROOT/config/$cfgdir" "$APP/Contents/Resources/$cfgdir"
    fi
done
# Sanity: the shipped defaults must be present, or the app would silently
# regress the "sounds missing next to an older KeeperFX data folder" bug.
test -f "$APP/Contents/Resources/fxdata/sounds.cfg" \
    || { echo "error: config/fxdata/sounds.cfg was not bundled into the app" >&2; exit 1; }

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

echo "== verifying bundle is self-contained (no Homebrew/local paths) =="
# Walk EVERY Mach-O — the main binary and every bundled dylib — not just the
# main binary. A link-time dep of a bundled library that dylibbundler failed to
# rewrite would leak here. (This cannot see a *missing* dlopen'd library, which
# is referenced by bare name; that class of gap is verified at runtime instead.)
leaked=0
for macho in "$APP/Contents/MacOS/keeperfx" "$LIBS"/*.dylib; do
    [ -f "$macho" ] || continue
    if otool -L "$macho" | grep -Eq "/opt/homebrew|/usr/local/"; then
        echo "error: $(basename "$macho") still references a non-bundled path:" >&2
        otool -L "$macho" | grep -E "/opt/homebrew|/usr/local/" | sed 's/^/    /' >&2
        leaked=1
    fi
done
[ "$leaked" -eq 0 ] || { echo "bundling incomplete — see above" >&2; exit 1; }

echo "== done: $APP =="
echo "   bundled dylibs: $(ls "$APP/Contents/libs" | wc -l | tr -d ' ')"
# Verify the whole bundle carries a valid signature (this is what macOS checks
# at launch and what the TCC privacy grant is attributed to).
codesign --verify --verbose=2 "$APP" \
    && echo "   bundle: signed & valid (ad-hoc)"
