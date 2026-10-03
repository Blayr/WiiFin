# Font reload after playback overwrites WiiFin's code

## Symptoms

- After stopping playback (often after scrubbing, often on the 2nd–5th
  session), the Wii throws an exception screen and reboots, or freezes.
  Exception types varied: Program exception, DSI, at different addresses
  each time.
- Under Dolphin with MMU emulation (`STRICT=1`): `IntCPU: Unknown
  instruction 00000000 at 8xxxxxxx`, or other garbage opcodes
  (`01efe1f5`, `00501110`, `27d7ef2f`), at a different PC each time.
  Once a `GFX FIFO: Unknown Opcode 0x0f`.
- No pattern in which code crashed — the common factor was "something ran
  after a playback session ended".

## Root cause

WiiFin calls `GRRLIB_Exit()` before handing the GPU to MPlayer and
`GRRLIB_Init()` again afterwards, once per playback session.

Stock `GRRLIB_Exit()` frees the system font. The next `GRRLIB_Init()`
reloads it through libogc's `SYS_InitFont()`, which reads the packed (Yay0)
font out of the boot ROM with **EXI DMA**.

EXI DMA can only address **MEM1** (the first 24 MB). Its address register
is too narrow for MEM2. On the first load, at app startup, the heap is
still entirely in MEM1, so it works. But after an MPlayer session the heap
has grown into MEM2, so the re-allocated font buffer can land there. The
DMA engine then truncates the address and writes the ROM data into MEM1
instead — which is where WiiFin's code (`.text`) lives.

Result: roughly 12–16 KB of code silently overwritten, at a different
address each time depending on where malloc put the font buffer. The crash
happens later, whenever the CPU next runs that code, which is why the
crash sites looked random.

## How it was found

1. Dolphin's `IntCPU: Unknown instruction` at varying PCs pointed to code
   being modified, not a logic bug in any one function.
2. Added `source/core/CodeIntegrity.cpp`: a checksum per 4 KB page of
   `.text` and the embedded-asset blob section, recorded at startup and
   re-checked every 2 s by the watchdog plus at checkpoints around playback
   teardown. Section bounds come from symbols added to
   `tools/wii_wiifin.ld`.
3. `CODE CORRUPTED` fired consistently between the checkpoints just before
   and just after the post-playback `GRRLIB_Init()`.
4. The overwritten page started with the bytes `Yay0` — the header of the
   packed system font — which identified exactly what was being written.

## Fix

`libs/grrlib/grrlib_keep_system_font_patch.diff`, applied to GRRLIB in
`Dockerfile.dev` after the existing reentrant-init patch. It removes the
`GRRLIB_ExitSystemFont()` call from `GRRLIB_Exit()`, so the font loaded at
startup (while the heap was still all MEM1) stays loaded for the app's
lifetime. `GRRLIB_InitSystemFont()` already returns early when the font is
loaded, so no reload and no DMA ever happens again.

Cost: the font buffer (allocated once at startup) stays resident
during playback instead of being freed.

Note: the `wiifin-dev` Docker image must be rebuilt to pick up the patch
(`docker build -f Dockerfile.dev -t wiifin-dev .`) — `docker-build.sh` only
builds the image when it is missing.

## Verification

Dolphin, `STRICT=1`, build `93afb846`: 55 playback sessions, 84 scrubs,
zero `CODE CORRUPTED` reports (the checker was running throughout).
Real-hardware verification pending.

## Files

- `libs/grrlib/grrlib_keep_system_font_patch.diff` — the fix
- `Dockerfile.dev` — applies the patch
- `source/core/CodeIntegrity.{h,cpp}` — the detector
- `tools/wii_wiifin.ld` — `__wiifin_text_start/end`, `__wiifin_blobs_start/end`
