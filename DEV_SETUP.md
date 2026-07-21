# WiiFin Development Environment

How the local build environment for this repo was set up, why it looks the
way it does, and how to reproduce or hand it off. WiiFin is Nintendo Wii
homebrew (a `.dol`/`.wad`), cross-compiled with devkitPro's devkitPPC
toolchain — there is no "just `npm install`" here, so this doc exists to
save the next person (or LLM) from re-deriving all of it.

## TL;DR

```bash
tools/dev/docker-build.sh          # builds WiiFin.dol (first run ~2-5 min, rest are fast)
tools/dev/docker-build.sh clean    # make clean, inside the container
tools/dev/docker-shell.sh          # interactive shell in the build container
```

Requires Docker (Docker Desktop or engine) running locally. That's the only
host dependency — everything else (compiler, libraries) lives in the
container image or in `libs/` inside the repo.

## Why Docker, not a native devkitPro install

The "normal" way to set this up is devkitPro's `dkp-pacman` installed via
`https://apt.devkitpro.org/install-devkitpro-pacman`. From this machine's
network, that host's Cloudflare bot-protection returns a JS challenge (403)
to `curl`/`wget`, so the installer script can't be fetched directly.
`dkp-pacman`'s actual package sync (`dkp-pacman -Sy` / `-S <pkg>`) works
fine once installed — it hits a different endpoint that isn't blocked —
but the initial bootstrap is what's unreachable.

devkitPro also publishes the whole devkitPPC toolchain as an official
Docker image (`devkitpro/devkitppc`) on Docker Hub, which sidesteps the
issue entirely and is arguably nicer anyway: fully reproducible, doesn't
touch the host system, easy to blow away and rebuild.

If `apt.devkitpro.org` is reachable from wherever this is being read, a
native install works too — just install `devkitPPC`, `libogc`, `wii-dev`,
plus the pacman packages `libfat-ogc ppc-libpng ppc-freetype
ppc-libjpeg-turbo`, then follow the "GRRLIB / libpngu" and "mbedTLS"
sections below (same steps, just without `docker run` wrapping them), and
point `DEVKITPRO`/`DEVKITPPC` at the native install instead of running
`./build.sh` through the container.

## What's in the image (`Dockerfile.dev`)

Base: `devkitpro/devkitppc:latest`. This already includes:
- `devkitPPC` (the `powerpc-eabi-*` cross toolchain)
- `libogc` — `libfat`, `libwiiuse`, `libbte`, `libasnd`, `libaesnd`,
  `libwiikeyboard`, `libdi`, `libiso9660`, `libtinysmb`, `libmad`, etc.
- devkitPro portlibs — `libpng`, `freetype`, `libjpeg(-turbo)`, `libbrotli*`,
  and many others (see `/opt/devkitpro/portlibs/{ppc,wii}/lib`)
- `dkp-pacman`, in case more portlibs are ever needed (`dkp-pacman -Ss
  <name>` to search, `dkp-pacman -S <name>` to install — this works fine
  from inside the container even though the installer bootstrap URL
  doesn't)

Added on top, because devkitPro does **not** package these (confirmed via
`dkp-pacman -Ss grrlib` / `-Ss pngu` returning nothing — GRRLIB's own
README says both are "supplied as source code"):
- **GRRLIB** (`libgrrlib.a`) — the 2D/3D graphics library WiiFin's UI is
  built on
- **libpngu** (`libpngu.a`) — GRRLIB's PNG-loading dependency

Both are built straight from `github.com/GRRLIB/GRRLIB` (`lib/pngu` then
`GRRLIB`, each `make clean all install`) and land in
`/opt/devkitpro/portlibs/wii/{lib,include}`, which is exactly where
WiiFin's `Makefile` already looks (`-lgrrlib -lpngu`) — no Makefile changes
needed.

Also installed via `apt`: `xxd` (the Makefile embeds assets as C arrays via
`xxd -i` — not present in the base image) and `python3-jsonschema` /
`python3-jinja2` (needed transiently by mbedTLS's code generator, see
below; harmless to leave installed).

## mbedTLS — built separately, not baked into the image

`libs/mbedtls/lib/*.a` is **gitignored** (see `.gitignore`) and *not* built
into `Dockerfile.dev`. It's built by `tools/dev/build_mbedtls.sh`,
run against the actual checkout, because:

WiiFin vendors a project-specific `libs/mbedtls/include/mbedtls/mbedtls_config.h`
— a minimal bare-metal config (no filesystem I/O, no POSIX threads, no
built-in net sockets since the Jellyfin client supplies its own bio
callbacks via libogc). mbedTLS must be compiled against *that exact
config* or the resulting struct layouts/enabled features won't match what
`source/jellyfin/*` expects. Baking a generic mbedTLS build into the base
image would silently produce something that doesn't match.

**How the mbedTLS version was determined**: the repo has the headers but
no version macro checked in visibly, so `libs/mbedtls/include` was diffed
against a few upstream tag checkouts. `v3.6.2` matched almost exactly —
the only differences were in config-cascade files (`mbedtls_config.h`,
`build_info.h`, `check_config.h`, `config_adjust_*.h`, plus a handful of
headers with config-conditional sections), all consistent with "same
version, customized config." If `libs/mbedtls/include` is ever
re-vendored from a newer upstream, update `MBEDTLS_REF` in
`tools/dev/build_mbedtls.sh` to match and re-run it.

**Build steps** (what the script does, condensed):
```bash
git clone --depth 1 --branch v3.6.2 https://github.com/Mbed-TLS/mbedtls.git
git submodule update --init --recursive framework   # needed for codegen (see gotcha below)
rm -rf mbedtls/include && cp -r <repo>/libs/mbedtls/include mbedtls/include
make -C mbedtls/library CC=powerpc-eabi-gcc AR=powerpc-eabi-ar \
     CFLAGS="-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -I<mbedtls include>" \
     static
cp mbedtls/library/lib{mbedcrypto,mbedx509,mbedtls}.a <repo>/libs/mbedtls/lib/
```

`tools/dev/docker-build.sh` runs this automatically the first time
`libs/mbedtls/lib/libmbedtls.a` is missing (e.g. fresh clone, or after
`git clean`). It's a few minutes; subsequent builds skip it.

### Gotchas hit while getting this working (in case they resurface)

- `make -C library lib` — wrong target name; it's `make -C library static`
  (or `shared`, or bare `all` for both).
- `powerpc-eabi-gcc: No such file or directory` — devkitPPC's `bin/` isn't
  on `PATH` by default in a bare `docker exec`; `docker-build.sh` /
  `docker-shell.sh` handle this, but if invoking `make` manually make sure
  `PATH` includes `$DEVKITPPC/bin`.
- `ModuleNotFoundError: jsonschema` / `jinja2` — mbedTLS 3.6's build
  unconditionally regenerates `psa_crypto_driver_wrappers.h` via a Python
  script, even though this config doesn't enable
  `MBEDTLS_PSA_CRYPTO_C`-driven codegen paths that matter here. Needs both
  packages available to `python3`.
- `ModuleNotFoundError: mbedtls_framework` — mbedTLS 3.6.x split some
  build/codegen tooling into a git submodule (`framework`,
  `Mbed-TLS/mbedtls-framework`). A shallow `git clone` alone won't fetch
  it; needs `git submodule update --init --recursive`.
- `xxd: not found` — not in the base `devkitpro/devkitppc` image; added
  via `apt-get install xxd` in `Dockerfile.dev`.

## MPlayer CE — already prebuilt, nothing to do

Unlike mbedTLS, `libs/mplayer-ce-build/libmplayer.a` (and `libfribidi.a`)
**are committed to git** despite the `.gitignore` entry (they were
force-added before the ignore rule existed — see `git log -- libs`). The
main `Makefile` auto-detects and links against them if present. No action
needed; video playback works out of the box in this checkout. See
`MPLAYER_CE_BUILD.md` if it ever needs to be rebuilt from source.

## Verifying a build

```bash
tools/dev/docker-build.sh
ls -la WiiFin.dol WiiFin.elf WiiFin.map
```

A successful build produces `WiiFin.dol` (~11MB) in the repo root. This
was verified end-to-end from a clean checkout (deleted `build/`,
`libs/mbedtls/lib/`, and all build artifacts, then ran
`tools/dev/docker-build.sh` with nothing else — it rebuilt mbedTLS and the
full project without intervention).

## Testing in Dolphin

Installed via flatpak: `flatpak install flathub org.DolphinEmu.dolphin-emu`.
Launch with `flatpak run org.DolphinEmu.dolphin-emu -e WiiFin.dol`.

**Note:** `dolphin`/`dolphin-emu` may already exist on `PATH` as *KDE's file
manager* on some distros (same binary name, completely unrelated project) —
check `dpkg -S $(which dolphin)` before assuming it's the emulator.

Three config gotchas hit getting Wiimote input actually working, all in
`~/.var/app/org.DolphinEmu.dolphin-emu/config/dolphin-emu/`:

- **`Dolphin.ini` `[Interface] LockCursor = True`** — without this, the
  emulated Wiimote pointer never tracks the mouse in-game (it works fine in
  Dolphin's own config dialogs, which is what makes this confusing — the
  cursor-grab is what continuous pointer tracking depends on via
  `XGrabPointer` on Linux).
- **`Dolphin.ini` `[Input] BackgroundInput = True`** — without this,
  keyboard/mouse input isn't delivered to the emulated game at all once the
  render surface isn't the OS-reported focused window (which can happen
  transiently depending on window manager/compositor).
- **GameCube controller port defaulting to the same mouse device as the
  Wiimote** causes input contention — set GC ports to "None"
  (`SIDevice0..3 = 0` in `Dolphin.ini`) unless actually testing GC input.

## Deploying to real hardware over the network (wiiload)

No SD card swap needed. `wiiload` isn't packaged anywhere reachable from
this environment (not in apt/pip/snap, and devkitPro's own `dkp-pacman`
bootstrap is blocked here — see above) but it's a tiny two-file host tool,
so it's built from source instead:

```bash
git clone --depth 1 https://github.com/devkitPro/wiiload.git
gcc -O2 -o wiiload source/main.c source/gecko.c -lz
```

Built binary lives at `tools/dev/bin/wiiload` (gitignored, host-arch
binary — rebuild if handed off to a different machine).

To send a build: on the Wii, open the Homebrew Channel and leave it idle at
the main menu — it shows its IP address on screen once network's up. Then:

```bash
export WIILOAD=tcp:<wii-ip>
tools/dev/bin/wiiload WiiFin.dol      # HBC receives it and launches immediately
```

(The main `Makefile` already has a `run:` target wrapping exactly this —
`WIILOAD` just needs to be set first.)

## What's *not* set up (out of scope so far)

- **WAD packaging** (`apps/WiiFin/`, the `tools/` WAD packager mentioned in
  the README) — untouched; only the `.dol` build path was set up.
- **CI** — there's no `.github/workflows/*` in this repo despite the
  READMEs mentioning "CI" for mbedTLS; this local Docker setup replaces
  that for development purposes but isn't wired into GitHub Actions.

## File reference

| Path | Purpose |
|---|---|
| `Dockerfile.dev` | devkitPPC + GRRLIB + libpngu build image |
| `tools/dev/build_mbedtls.sh` | Cross-compiles mbedTLS against the vendored config; run once per checkout |
| `tools/dev/docker-build.sh` | Main entry point: builds image if needed, builds mbedTLS if needed, runs `make` |
| `tools/dev/docker-shell.sh` | Interactive shell in the build container, for debugging build issues by hand |
| `tools/dev/bin/wiiload` | Host binary, sends a `.dol` to a Wii running HBC over the network (see above); gitignored, rebuild per machine |
| `tools/dev/throttle_proxy.py` | Root-free TCP proxy that rate-limits + logs traffic between WiiFin and the real Jellyfin server, to approximate real Wii wifi speeds when testing in Dolphin (`--help` for usage) |
