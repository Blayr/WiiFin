# WiiFin

Jellyfin client for the Nintendo Wii. C++ homebrew, cross-compiled with
devkitPro/devkitPPC, built on GRRLIB (2D/3D graphics) + MPlayer CE (video
playback). Output is a `.dol`/`.wad` run on real hardware or Dolphin — not
a normal desktop app, so it can't be run directly on this machine.

## Build

```bash
tools/dev/docker-build.sh          # builds WiiFin.dol via Docker (needs Docker running)
tools/dev/docker-build.sh clean    # make clean
tools/dev/docker-shell.sh          # interactive shell in the build container
```

First run builds a `wiifin-dev` Docker image and cross-compiles mbedTLS
(a few minutes); after that, `docker-build.sh` is a normal incremental
`make` and is fast. **Always verify changes compile with
`tools/dev/docker-build.sh` before considering a change done** — there's
no other way to catch errors short of running on hardware/emulator.

Full explanation of why this is Docker-based, what's in the image, and
gotchas encountered setting it up: see `DEV_SETUP.md`.

## Structure

```
source/core/      App lifecycle (App.cpp), background music, sound FX, utils
source/input/     Wiimote + USB keyboard input
source/jellyfin/  Jellyfin HTTP API client (JellyfinClient.cpp), HTTPS via mbedTLS
source/player/    MPlayer CE integration (WiiPlayer.cpp), player overlay HUD
source/ui/        Views: Connect, Library (largest file by far), MusicPlayer, Profile, Settings
data/             PNG/TTF/mp3 assets, embedded into the binary at build time via xxd
libs/mbedtls/     Vendored mbedTLS headers + config; lib/*.a is gitignored, built locally
libs/mplayer-ce-build/  Prebuilt libmplayer.a — committed to git, don't touch
libs/grrlib/      Patch applied to GRRLIB during the Docker build (see below)
tools/dev/        Docker-based build tooling (see above)
```

`LibraryView.cpp` is the largest source file (~4000+ lines) — covers
library browsing, detail views, search, and playlists in one file.

## Local dev notes

`LOCAL_NOTES.md` (gitignored) has machine-specific info not fit for the
repo — currently a test Jellyfin server + credentials for exercising the
connect/login/library/playback flow. Check it before assuming there's no
way to test against a real server.

## Conventions / gotchas

- Assets in `data/` are embedded as C arrays (`xxd -i`) at build time —
  adding a new asset means adding both the file and an explicit Makefile
  rule (see existing entries in `Makefile` for the pattern), there's no
  glob-based asset pickup.
- `libs/mbedtls/lib/` and `build/` are gitignored and machine-generated —
  never hand-edit or expect them to be present in a fresh clone.
  `libs/mplayer-ce-build/` looks gitignored but is actually committed
  (added before the ignore rule existed) — don't delete it.
- No CI is wired up in this repo (no `.github/workflows/*`) despite READMEs
  referencing "CI" for mbedTLS builds — this repo's Docker setup is the
  substitute for local development.
- Wii constraints worth keeping in mind per `CONTRIBUTING.md`: low RAM,
  fixed 640x480 output.
- GRRLIB is built from a pinned upstream commit + a patch applied during
  the Docker image build (`Dockerfile.dev`), not vendored as source —
  see `libs/grrlib/grrlib_reentrant_init_patch.diff` for why: stock
  GRRLIB_Init()/GRRLIB_Exit() are one-shot (by design, since ~2009), but
  WiiFin calls both repeatedly across every video play/stop cycle. Without
  the patch this is a real, hardware-only use-after-free (freed
  xfb/gp_fifo pointers, destroyed FreeType instance, still in use after
  the first video stops) — intermittent hang/crash, not reproducible in
  Dolphin. If `Dockerfile.dev`'s cached `wiifin-dev` image predates this
  patch, rebuild it explicitly (`docker build -f Dockerfile.dev -t
  wiifin-dev .`) — `docker-build.sh` only builds the image if it's missing
  entirely, so it won't pick this up on its own.
