# KeeperFX — Apple Silicon fork

Unofficial fork of [KeeperFX](https://github.com/dkfans/keeperfx), the
open-source Dungeon Keeper remake, with a native Apple Silicon (arm64) macOS
build. Not affiliated with or endorsed by the KeeperFX team; no support; the app
is ad-hoc signed, not notarized. The fork ends when upstream ships an official
macOS build. For the game itself, and on Windows/Linux, use
**https://github.com/dkfans/keeperfx**.

## Download (Apple Silicon)

Self-contained **`KeeperFX.app`** (currently `macos-v1.4.9`, tags `macos-v*`):
**https://github.com/matthewdeaves/keeperfx/releases/latest**

Drop it next to your existing KeeperFX data (and the original Dungeon Keeper
files) and double-click. First-launch Gatekeeper and folder-layout notes are
under [macOS: running](#macos-running); or [build it yourself](#macos-build-from-source).

## Changes in this fork

What this fork adds on top of upstream `dkfans/keeperfx`:

### Apple Silicon / arm64
- **Native arm64 build** (`macos.mk`), plus a self-contained, self-locating `KeeperFX.app` that bundles
  its dylibs and its own config defaults (so it still works dropped next to an
  older KeeperFX data install).
- **arm64 correctness fixes** — unaligned-access (SIGBUS) crashes in the
  isometric render, sprite/pixel draw, computer-player gold scan, and the
  named-field config framework; in-memory-only packed structs unpacked for
  natural alignment. x86 behaviour is unchanged.
- **Safety fixes** — bounds-check the RNC decompressor and the script
  command-name lookup; guard the power-hand path against a bad dungeon pointer or
  out-of-range creature model.
- **Thought bubbles** — creature states flagged both to show a thought bubble
  and as sneaky now show it (a one-byte flag was read as two bytes).
- **OpenGL renderer by default on macOS** — fixes a render-thread deadlock and a
  black window, so upstream's OpenGL renderer works on Apple Silicon, and makes it
  the default. `RENDERER=SOFTWARE` in `keeperfx.cfg` selects the software
  renderer. If OpenGL fails to start, the game falls back to software.
- **Text rendering: fixed a redundant-redraw bug** in word-wrapped text with no
  alignment set (notably the scrolling tooltip box), where every word-wrap
  point re-emitted the whole string from the start — an upstream bug, also
  fixed here. Also skip glyphs fully outside the current clip rect instead of
  laying them out. Measured on an Apple Silicon Mac: a scrolling tooltip that
  dropped rendering to 4 FPS now holds 30+.

### User-data locations
- Saves, settings, high scores, netplay config and screenshots now write to the
  proper per-user location instead of the game folder, so the install can be
  read-only and replacing the app never touches your saves. Existing saves are
  migrated once (copied, never moved).
  - **macOS:** `~/Library/Application Support/KeeperFX`
  - **Linux:** `$XDG_DATA_HOME/keeperfx` (default `~/.local/share/keeperfx`)
  - **Windows:** `%APPDATA%\KeeperFX`
- On macOS, screenshots go to Application Support, not `~/Pictures`, so there is
  no privacy prompt. See
  [`docs/adr/0001-macos-userdata-locations.md`](docs/adr/0001-macos-userdata-locations.md).

### Packaging & CI
- Optional starter `keeperfx.cfg` shipped with the macOS download: upstream's
  defaults with a calmer GUI flash rate (5 vs upstream's 1). Only used if the
  game folder has none.
- CI builds and checks the macOS, Windows and Linux builds on every push to
  master.

## Requirements

Like every KeeperFX build, this needs a game folder containing the KeeperFX data
**plus the original Dungeon Keeper files** (from an old CD, or the digital
editions on [GOG](https://www.gog.com/game/dungeon_keeper) /
[EA](https://www.ea.com/games/dungeon-keeper/dungeon-keeper) /
[Steam](https://store.steampowered.com/app/1996630/Dungeon_Keeper_Gold/)) listed
in [`docs/files_required_from_original_dk.txt`](docs/files_required_from_original_dk.txt).

The app needs an **Apple Silicon Mac on macOS 15 or later**. Tested on a MacBook
Air (M5, macOS 26 and 27) with **Dungeon Keeper Gold** from GOG.

## macOS: build from source

```sh
brew install pkg-config sdl3 sdl3_image sdl3_mixer ffmpeg luajit \
    openal-soft libspng minizip miniupnpc libnatpmp zlib curl dylibbundler
./tools/build_macos_deps.sh                 # one-time: builds astronomy/centijson/enet6
make -f macos.mk -j"$(sysctl -n hw.ncpu)"   # -> bin/keeperfx (arm64 Mach-O)
```

The full write-up is in [`docs/MACOS_ARM64_PORT.md`](docs/MACOS_ARM64_PORT.md).

## macOS: package a self-contained `KeeperFX.app`

```sh
tools/make_macos_app.sh          # -> dist/KeeperFX.app
```

This bundles the engine's libraries (via `dylibbundler`) and ad-hoc signs it, so
the `.app` runs with **no Homebrew installed**.

## macOS: running

`KeeperFX.app` is a *drop-in* engine: on startup it locates itself and changes
the working directory to the folder containing the `.app`, so it finds the game
data next to it. The layout is:

```
YourKeeperFX/            <- any folder (including Desktop/Documents)
├── KeeperFX.app         <- drop the app in here, next to the data
├── data/  sound/  ldata/  fxdata/  creatrs/  campgns/  levels/  music/  ...
└── keeperfx.cfg
```

Double-click `KeeperFX.app`. On first launch:

- **If you downloaded the `.app`**, macOS blocks it with "Apple could not
  verify…" (it's ad-hoc signed, not notarized). Clear the download flag once — in
  Terminal: `xattr -dr com.apple.quarantine /path/to/KeeperFX.app` — or
  double-click, then System Settings → Privacy & Security → **Open Anyway**. (A
  locally built `.app` has no such flag and just opens.)
- If the folder is privacy-protected (Desktop, Documents, Downloads), macOS asks
  to let KeeperFX access files there — click **Allow**.

GOG's *Dungeon Keeper Gold* app has the original files uncompressed at
`Contents/Resources/game/{DATA,SOUND}/` (soundtrack `keeper0*.ogg` in its game
root); copy them in with lowercase names.

## Windows / Linux

**Use the official project: https://github.com/dkfans/keeperfx.** CI still
builds Windows and Linux here (with CMake, as upstream does), but there's no
reason to use this fork on those platforms.

## License

GNU General Public License v2.0, same as upstream — see [LICENSE](LICENSE).
