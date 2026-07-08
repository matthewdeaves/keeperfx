# 1. Per-user data locations (macOS / Linux / Windows), and screenshots in Application Support

- Status: Accepted
- Date: 2026-07-08
- Scope: this fork (`matthewdeaves/keeperfx`)

## Context

Historically KeeperFX writes user-generated files — save games, `settings.toml`,
high scores, netplay config, screenshots — **into the game install directory**
(resolved from `keeper_runtime_directory` in `_resolve_file_path_internal`,
`src/config.c`). For the native macOS `.app`, which is designed to be dropped
next to the game data and can live in a read-only or app-replaceable location,
this is wrong on two counts:

1. Replacing the `.app` (e.g. downloading a new build) can wipe saves that sat
   beside it.
2. It assumes the install directory is writable, which is not a safe assumption
   for a distributed application bundle.

We want user data in the **proper per-user location for each platform**, with the
game/install folder treated as read-only. This ADR records the locations chosen
and, specifically, why screenshots go to Application Support rather than
`~/Pictures`.

## Decision

### Locations (macOS, implemented)

All per-user writable data goes under a single root:

```
~/Library/Application Support/KeeperFX/
  save/       <- save games, settings.toml, high scores, netplay config
  scrshots/   <- screenshots
```

This is Apple's designated location for user-specific application support files.
Apple's *File System Programming Guide* states user-specific support files belong
in `~/Library/Application Support`, and that user-visible directories "must never
be used to store data files that your app creates and manages automatically."

### Screenshots: Application Support, **not** `~/Pictures`

`~/Pictures/KeeperFX` was considered because screenshots are user-facing content
that people want to find and share, and Pictures is the idiomatic home for
images. It was **rejected** because on macOS the Desktop, Documents, Downloads
**and Pictures** folders are TCC (privacy) protected: the first write triggers a
system permission prompt ("KeeperFX would like to access files in your Pictures
folder"). A screenshot is captured **mid-game, frequently in fullscreen**, so
that prompt would fire during play, steal focus, and can render badly over a
fullscreen game.

`~/Library/Application Support` is **not** TCC-protected for the app's own
subtree, so writing screenshots there never prompts. We accept the tradeoff that
`~/Library` is a hidden folder: the path is documented for users, and avoiding a
mid-game interruption is worth more than one-click discoverability for a niche
action.

### Mechanism (the reusable seam)

`_resolve_file_path_internal` gains an optional base dir,
`keeper_userdata_directory`. When it is empty the resolver behaves exactly as
before; when set, `FGrp_Save` and `FGrp_SShots` resolve under it (in `save/` and
`scrshots/` subdirs). A single per-platform entry point,
`setup_userdata_directories()` (declared in `platform.h`, called once at startup
after `keeper_runtime_directory` is known), creates the dirs, sets the global,
and migrates any existing saves once (copy, never move). It is implemented in
`src/linux.cpp` (macOS and Linux) and `src/windows.cpp` (Windows).

Two supporting fixes shipped with this:
- `create_directory_for_file` (`src/bflib_fileio.c`) skips the empty leading
  component of an absolute path; previously `mkdir("")` failed with `ENOENT`,
  which would silently abort save creation into an absolute userdata dir.
- `src/scrcapt.c` now routes the screenshot path through the resolver instead of
  a hardcoded CWD-relative `"scrshots/"` (the one write site that bypassed it).

### Locations on each platform (implemented)

A single per-user root per platform, holding `save/` and `scrshots/`:

| Platform | Root | Resolved via |
| --- | --- | --- |
| macOS | `~/Library/Application Support/KeeperFX` | `$HOME` |
| Linux | `$XDG_DATA_HOME/keeperfx` (default `~/.local/share/keeperfx`) | XDG Base Directory spec; falls back to `$HOME/.local/share` |
| Windows | `%APPDATA%\KeeperFX` (Roaming) | `APPDATA` env var |

We keep **one root per platform** (saves and screenshots together) rather than
scattering across per-category folders, for consistency with the single-seam
mechanism and because it is simplest to reason about. The stricter per-category
"ideal" (below) is a possible future refinement, not a requirement.

Rationale for the roots:
- **macOS** — Apple's designated user app-support location (see above).
- **Linux** — `$XDG_DATA_HOME` is exactly "user-specific data files" per the XDG
  Base Directory Specification; the documented `~/.local/share` fallback is used
  when it is unset or not absolute.
- **Windows** — `%APPDATA%` (Roaming AppData) is a proper official user-data
  location and is what many games use. It is chosen over `FOLDERID_SavedGames`
  because we want one root for saves **and** screenshots/settings, and because
  `%APPDATA%` is reliably resolved from the env var, whereas Saved Games has no
  env var and must go through the Known Folders API. This also matters because a
  Windows install under `Program Files` is read-only, so writing beside the
  binary fails — moving userdata out is a correctness fix there, not just tidying.

**Deferred per-category ideal** (not implemented; would need extra base dirs):

| Data | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Regen caches (`tables.dat`, `*.col`) | `~/Library/Caches/KeeperFX` | `$XDG_CACHE_HOME/keeperfx` | `%LOCALAPPDATA%\KeeperFX\cache` |
| Log | `~/Library/Logs/KeeperFX` | `$XDG_STATE_HOME/keeperfx` | `%LOCALAPPDATA%\KeeperFX\logs` |
| Screenshots (discoverable) | — (stays in App Support, see above) | `$XDG_PICTURES_DIR` | `FOLDERID_Pictures\KeeperFX` |

**Windows caveat for any future Saved Games / Pictures use:** resolve via the
Known Folders API (`SHGetKnownFolderPath(FOLDERID_SavedGames …)`), **not** the
`%USERPROFILE%\Saved Games` string — the latter breaks when the folder is
relocated. `%APPDATA%`/`%LOCALAPPDATA%` env vars are reliable and don't need it.

**Screenshots elsewhere:** Windows and Linux have no mid-game TCC-prompt problem,
so a discoverable Pictures location would be reasonable there in future; for now
they share the single root for simplicity.

## Consequences

- On all three platforms, saves and screenshots move out of the game folder, so
  it can be read-only (macOS `.app` replacement, Windows `Program Files`) without
  losing or failing to write saves.
- Existing saves are migrated once (copy, never move) from the old in-place
  `save/` dir, so upgrading users don't lose them and the originals remain as a
  fallback.
- When `keeper_userdata_directory` is left unset (e.g. a platform we haven't
  wired up, or `$HOME`/`%APPDATA%` missing) behaviour is exactly as before —
  saves stay beside the binary. No hard dependency on the new location.
- macOS: no privacy prompt is ever shown for saving or screenshotting. Screenshots
  live in a hidden folder; users are told the path (README / release notes),
  reachable via Finder **Go → Go to Folder…** →
  `~/Library/Application Support/KeeperFX/scrshots`.
- Regenerated caches (`tables.dat`, `*.col`) and the log file still write to the
  install/CWD for now; moving them to per-platform cache/log dirs is a documented
  follow-up (the cache case is the one entangled `FGrp_StdData` read/write group).

## References

- Apple, *File System Programming Guide* — File System Basics:
  https://developer.apple.com/library/archive/documentation/FileManagement/Conceptual/FileSystemProgrammingGuide/FileSystemOverview/FileSystemOverview.html
- freedesktop.org, *XDG Base Directory Specification*:
  https://specifications.freedesktop.org/basedir/latest/
- Microsoft *Known Folders* / `FOLDERID_SavedGames` (see PCGamingWiki summary):
  https://www.pcgamingwiki.com/wiki/Glossary:Game_data
