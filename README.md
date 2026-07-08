# KeeperFX — Apple Silicon fork

> **Personal, unofficial fork. Use at your own risk.**
>
> Dungeon Keeper was a favourite game of my childhood, and I wanted a version of
> [KeeperFX](https://github.com/dkfans/keeperfx) — the open-source Dungeon Keeper
> remake — that builds and runs **natively on Apple Silicon (arm64)**. I'm not a
> KeeperFX expert; I've just made judgment calls along the way and tried to keep
> the changes clean rather than hacky.
>
> This fork is **not affiliated with or endorsed by** the upstream KeeperFX team.
> It comes with no guarantees or support: it may break, and the macOS app is
> ad-hoc signed (not notarized). Nearly all of the game is upstream's work — I've
> only touched what was needed to get it building and running natively on Apple
> Silicon.
>
> **This fork is temporary.** Native macOS support is being worked on upstream;
> once the official project ships a macOS / Apple Silicon build, this fork will
> have done its job and I'll stop maintaining it. Until then I keep it building
> and roughly in step with upstream.
>
> **If you just want to play KeeperFX, use the official project:**
> **https://github.com/dkfans/keeperfx**

## Download (Apple Silicon)

A prebuilt, self-contained **`KeeperFX.app`** is published here — no build needed:

**https://github.com/matthewdeaves/keeperfx/releases/latest**

(Versioned releases are tagged `macos-v*`; that link always resolves to the
newest one.)

Drop it next to your existing KeeperFX data (and the original Dungeon Keeper
files) and double-click. First-launch Gatekeeper and folder-layout notes are
under [macOS: running](#macos-running); or [build it yourself](#macos-build-from-source).

## Changes in this fork

Everything below is what this fork adds on top of upstream `dkfans/keeperfx`; the
game itself, and Windows/Linux support, are upstream's work. This is the one
place I keep updated as the fork evolves.

### Apple Silicon / arm64
- **Native arm64 build** (`macos.mk`) — a real Mach-O binary, no Rosetta or
  emulation — plus a self-contained, self-locating `KeeperFX.app` that bundles
  its dylibs and its own config defaults (so it still works dropped next to an
  older KeeperFX data install).
- **arm64 correctness fixes** — unaligned-access (SIGBUS) crashes in the
  isometric render, sprite/pixel draw, computer-player gold scan, and the
  named-field config framework; in-memory-only packed structs unpacked for
  natural alignment. x86 behaviour is unchanged.
- **Safety fixes** — bounds-check the RNC decompressor and the script
  command-name lookup; guard the power-hand path against a bad dungeon pointer or
  out-of-range creature model.

### User-data locations
- Saves, settings, high scores, netplay config and screenshots now write to the
  proper per-user location instead of the game folder, so the install can be
  read-only and replacing the app never touches your saves. Existing saves are
  migrated once (copied, never moved).
  - **macOS:** `~/Library/Application Support/KeeperFX`
  - **Linux:** `$XDG_DATA_HOME/keeperfx` (default `~/.local/share/keeperfx`)
  - **Windows:** `%APPDATA%\KeeperFX`
- On macOS, screenshots go to Application Support, **not** `~/Pictures`, so taking
  one never triggers a privacy permission prompt mid-game. Full rationale in
  [`docs/adr/0001-macos-userdata-locations.md`](docs/adr/0001-macos-userdata-locations.md).

### Packaging & CI
- Optional starter `keeperfx.cfg` shipped with the macOS download, with a calmer
  GUI flash rate (5 vs upstream's 1). Only used if the game folder has none.
- CI builds and checks the macOS, Windows and Linux builds on every push to
  master.

## Requirements

Like every KeeperFX build, this needs a game folder containing the KeeperFX data
**plus the original Dungeon Keeper files** (from an old CD, or the digital
editions on [GOG](https://www.gog.com/game/dungeon_keeper) /
[EA](https://www.ea.com/games/dungeon-keeper/dungeon-keeper) /
[Steam](https://store.steampowered.com/app/1996630/Dungeon_Keeper_Gold/)) listed
in [`docs/files_required_from_original_dk.txt`](docs/files_required_from_original_dk.txt).
The install gate is simply the presence of `data/bluepal.dat`.

I built and tested this fork against **Dungeon Keeper Gold** from
[GOG](https://www.gog.com/game/dungeon_keeper).

## macOS: build from source

```sh
brew install pkg-config sdl2 sdl2_image sdl2_mixer sdl2_net ffmpeg luajit \
    openal-soft libspng minizip miniupnpc libnatpmp zlib curl dylibbundler
./tools/build_macos_deps.sh                 # one-time: builds astronomy/centijson/enet6
make -f macos.mk -j"$(sysctl -n hw.ncpu)"   # -> bin/keeperfx (arm64 Mach-O)
```

The full write-up is in [`docs/MACOS_ARM64_PORT.md`](docs/MACOS_ARM64_PORT.md).

## macOS: package a self-contained `KeeperFX.app`

```sh
tools/make_macos_app.sh          # -> dist/KeeperFX.app
```

This bundles the engine's libraries (via `dylibbundler`, including the SDL3 that
`sdl2-compat` loads at runtime) and ad-hoc signs it, so the `.app` runs on any
Apple Silicon Mac with **no Homebrew installed**.

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

> Tip: if you have GOG's *Dungeon Keeper Gold* installed, the required original
> files ship **uncompressed** inside its app bundle at
> `Contents/Resources/game/{DATA,SOUND}/` (and the `keeper0*.ogg` soundtrack in
> its game root) — no CD-image extraction needed. Copy them in with lowercase
> names.

## Windows / Linux

**Use the official project, not this fork.** This fork exists for the macOS /
Apple Silicon build. On Windows and Linux, upstream KeeperFX is the real thing —
get it from **https://github.com/dkfans/keeperfx**. I do my best to keep the
Windows and Linux builds working here (CI checks them on every push, and they
build with `make` and `make -f linux.mk`), but there's no reason to use this fork
over upstream on those platforms.

## License

GNU General Public License v2.0, same as upstream — see [LICENSE](LICENSE).
