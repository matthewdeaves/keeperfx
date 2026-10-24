#!/bin/bash
# Build the downloadable KeeperFX.dmg: a ready-to-copy KeeperFX folder (app +
# KeeperFX's own data + starter config + helper to add the user's own Dungeon
# Keeper files) and a READ ME FIRST.txt. The original Dungeon Keeper files can't
# be shipped, so the user adds them with the helper.
#
# Usage: tools/make_macos_dmg.sh <KeeperFX.app> <data-dir> <keeperfx.cfg> <out.dmg>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$1"; DATA="$2"; CFG="$3"; OUT="$4"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
GAME="$STAGE/KeeperFX"
mkdir -p "$GAME"

rsync -a "$DATA/" "$GAME/"
ditto "$APP" "$GAME/KeeperFX.app"
cp "$CFG" "$GAME/keeperfx.cfg"
rm -f "$GAME/launcher-auto-file-removal.txt"   # Windows launcher housekeeping
cp "$ROOT/docs/files_required_from_original_dk.txt" "$GAME/"
cp "$ROOT/tools/macos_dmg/Add Dungeon Keeper Files.command" "$GAME/"
cp "$ROOT/tools/macos_dmg/READ ME FIRST.txt" "$STAGE/"
chmod +x "$GAME/Add Dungeon Keeper Files.command"

rm -f "$OUT"
hdiutil create -volname "KeeperFX" -srcfolder "$STAGE" -format UDZO -ov "$OUT" >/dev/null
ls -lh "$OUT"
