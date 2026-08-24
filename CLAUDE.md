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
gotchas encountered setting it up: see `DEV_SETUP.md`. For rebuilding
`libs/mplayer-ce-build/libmplayer.a` itself (a separate, optional,
much-longer process — cross-compiling MPlayer CE + FFmpeg from source, not
something `docker-build.sh` does), see `MPLAYER_CE_BUILD.md`.

## Deploy

Two ways to get a fresh `WiiFin.dol` onto a real Wii:

```bash
WIILOAD=tcp:<wii-ip> tools/dev/bin/wiiload WiiFin.dol   # needs Wii sitting at the Homebrew Channel
curl -T WiiFin.dol ftp://<wii-ip>/sd/apps/WiiFin/boot.dol  # needs ftpii (or similar) running on the Wii
```

`wiiload` is HBC's own network-loader protocol — it only works while HBC is
actively listening. "Connection refused" despite a successful `ping` means
the Wii is mid-app, not at HBC; back out and retry. FTP works regardless of
what's currently running, as long as *something* with an FTP server is —
note the destination filename is `boot.dol`, not `WiiFin.dol` (standard HBC
app-folder layout: `sd:/apps/<name>/boot.dol` + `meta.xml` + `icon.png`).

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
- `SYS_Report()` is a no-op on real hardware without a USB Gecko attached —
  easy to add a debug print, test on real hardware, and see nothing. For
  anything that needs to be visible without a Gecko, use `DebugLog::Log()`
  (`source/core/DebugLog.h`) instead: it feeds a small on-screen HUD ring, a
  300-entry in-app Log Viewer (Settings → View Debug Log), and — opt-in via
  the Debug Log File setting — a batched (not per-line) file on the SD card.
- GameCube pad / Classic Controller input is merged into the same
  `WPAD_BUTTON_*`-shaped bitmask as Wiimote/IR (`Input::gcButtonsAsWpad()`),
  but `vo_gx.c` inside the vendored `libmplayer.a` only ever reads raw
  Wiimote data for its own in-video overlay/pause/seek handling — it's
  completely blind to GC pad state. That's why `PlayerOverlay` has a
  separate "nativeMode" rendering path (`onFrame()`/`drawNativeControls()`
  etc., writing straight into the Y-plane texture) for any session revealed
  via GC pad: `vo_gx.c`'s own overlay would simply never appear otherwise.
- MPlayer fetches the actual video/audio stream itself, via its own stream
  modules (`stream_https_wii.c`/`stream_http_wii.c`, inside the vendored
  `libmplayer.a` — see `MPLAYER_CE_BUILD.md`) — **not** through
  `JellyfinClient::httpRequest()`. Only API calls and image fetches go
  through `JellyfinClient`; anything instrumenting or throttling network
  behavior there (timing, retries, throughput measurement) has no effect on
  the video stream itself.
