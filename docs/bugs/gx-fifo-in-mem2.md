# GPU hang after several playbacks: GX FIFO allocated in MEM2

## Symptoms

- After a number of playback sessions (anywhere from ~4 to ~12), the UI
  froze solid: spinner stopped turning ("Saving position…", "Stopping
  transcode…"), or a menu/details page stopped responding to input.
- Real hardware only. Never reproduced in Dolphin.
- Interrupts were still running (VI retraces kept counting), so it wasn't a
  CPU lockup.

## What the watchdog showed

Both hangs caught with `DIAG=1`:

- Main thread blocked in `GX_DrawDone()` / waiting for FIFO space inside
  `GRRLIB_Render()`.
- GPU busy but stalled: `readIdle=0 cmdIdle=0`, CP read pointer frozen for
  40+ s (the second one for over an hour), FIFO full (`overhi=1`).
- The commands at the read pointer were **valid** (e.g. GRRLIB's per-pixel
  TTF point draws), so the GPU wasn't choking on garbage.
- The FIFO was in **MEM2** both times (`base 0x10219680`, `base 0x10002020`),
  with MEM1 nearly exhausted (~100–180 KB left).

## Root cause

`GRRLIB_Init()` allocates its 256 KB GX command FIFO with `memalign()`, and
`GRRLIB_Exit()` frees it. WiiFin runs that cycle around every playback. At
startup the heap is all MEM1, so the FIFO lands there. After the first
MPlayer session MEM1 is effectively full, so every later FIFO is allocated
from MEM2, and the GPU eventually stalls while reading it.

This fits the long-standing pattern of "only after several playbacks, never
early on". Why the GP stalls on a MEM2 FIFO on real hardware isn't confirmed;
the correlation (2 of 2 caught hangs, 0 hangs with the FIFO in MEM1) is the
evidence.

## Fix

`libs/grrlib/grrlib_static_fifo_patch.diff` (applied in `Dockerfile.dev`
after the other two GRRLIB patches): GRRLIB uses one static, 32-byte-aligned
256 KB buffer for every `GRRLIB_Init()` instead of `memalign()`, and
`GRRLIB_Exit()` no longer frees it. Static storage is in `.bss`, which is
always in MEM1 (`gp_fifo_mem` links at `0x80B5EC40`). It also removes 256 KB
of heap churn per session.

Rebuild the `wiifin-dev` image to pick it up
(`docker build -f Dockerfile.dev -t wiifin-dev .`), then `make clean` so the
app relinks against the new `libgrrlib.a`.

## Verification

Real Wii, build `acfc9459`: MEM1 was down to 176 KB after the first playback
(so the old allocation would have been in MEM2 from then on), then 4
playbacks (3 autoplayed) and the details page afterwards with no hang. Free
memory also returned to the same values after every session, where earlier
logs showed ~150 KB lost per playback. More long sessions are still needed
before calling it solved.

## Files

- `libs/grrlib/grrlib_static_fifo_patch.diff` — the fix
- `Dockerfile.dev` — applies it
- `source/core/Watchdog.cpp` — the FIFO/GP state dump that found it
