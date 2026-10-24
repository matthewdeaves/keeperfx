#!/bin/bash
# Copies the original Dungeon Keeper files KeeperFX needs into this folder.
# KeeperFX can't ship them; you need your own copy of Dungeon Keeper / Dungeon
# Keeper Gold (GOG, EA, Steam or the original CD). Point this at a folder that
# contains them (for GOG/CD: the mounted or extracted CD image).
#
# Usage: double-click, then drag your Dungeon Keeper folder into the window
#        (or: ./"Add Dungeon Keeper Files.command" /path/to/dungeon-keeper)
set -u
cd "$(dirname "$0")"
LIST="files_required_from_original_dk.txt"
[ -f "$LIST" ] || { echo "Missing $LIST next to this script." >&2; read -r -p "Press Return..." _; exit 1; }

SRC="${1:-}"
if [ -z "$SRC" ]; then
    echo "Drag the folder that contains your Dungeon Keeper files into this window,"
    echo "then press Return."
    read -r SRC
fi
SRC="${SRC%\"}"; SRC="${SRC#\"}"; SRC="${SRC//\\ / }"; SRC="${SRC%/}"
[ -d "$SRC" ] || { echo "Not a folder: $SRC" >&2; read -r -p "Press Return..." _; exit 1; }

copied=0; missing=0
while IFS= read -r rel; do
    rel="${rel#./}"
    [ -n "$rel" ] || continue
    # Match case-insensitively anywhere under SRC (CD layouts vary: DATA/, keeper/data/, ...).
    found="$(find "$SRC" -type f -ipath "*/$rel" 2>/dev/null | head -n 1)"
    if [ -n "$found" ]; then
        mkdir -p "$(dirname "$rel")"
        cp "$found" "$rel" && copied=$((copied+1))
    else
        echo "  not found: $rel"
        missing=$((missing+1))
    fi
done < <(grep '^\./' "$LIST")

echo
echo "Copied $copied file(s); $missing missing."
if [ "$missing" -eq 0 ]; then
    echo "All set. Open KeeperFX.app to play."
else
    echo "Some files weren't found. For the GOG version, use the files inside its CD"
    echo "image (mount or extract it first). See $LIST."
fi
read -r -p "Press Return to close..." _
