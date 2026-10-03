# Hard freeze after several playbacks: LWP queue pool exhaustion

## Symptoms

- After roughly 4–6 playback sessions (count depended on how many
  scrubs happened, since each scrub restarts MPlayer), WiiFin froze solid:
  blue spinner stopped turning, or the last UI frame stayed on screen
  (season list, episode detail, activity screen…).
- Nothing in the debug log, no exception screen, B and HOME did nothing.
  The watchdog thread didn't report either — it stopped running too.
- Under Dolphin: same freeze, no warnings, emulation still "running".

## Root cause

libogc 3.1 implements the old LWP wait-queue API on top of its new kernel
(tuxedo) with a **fixed pool of 64 queue slots**, tracked by a 64-bit
bitmask (`lwpc_queue_used`). Only `LWP_CloseQueue()` returns a slot.

`GX_Init()` and `VIDEO_Init()` each call `LWP_InitQueue()` on a static
handle **every time they're called**, without closing the queue from the
previous call. WiiFin re-runs them several times per playback session
(MPlayer's own GX init when playback starts, then `GRRLIB_Init()` after it
stops, and again on each scrub restart). Each call leaks a slot.

After about 60 leaked slots the pool is empty:

1. `LWP_InitQueue()` fails and leaves the handle as `LWP_TQUEUE_NULL`
   (`0xFFFFFFFF`).
2. `LWP_ThreadSleep()` on that handle returns -1 immediately instead of
   sleeping.
3. `GX_WaitDrawDone()` / `GX_DrawDone()` wait for the GPU's draw-done
   interrupt in a loop around `LWP_ThreadSleep()` **with interrupts
   disabled**. With the sleep returning instantly, the loop never ends and,
   because interrupts are off, nothing else ever gets to run.

That's a total lockup — no thread switches, no interrupts, so no logging,
no watchdog, no input. It only shows up after enough GX/VI re-inits, which
is why it took several sessions and why scrubbing made it come sooner.

## How it was found

Freezes with interrupts off leave no trace in any log, so it was found by
attaching to Dolphin's GDB stub while frozen (`GDB=1 ./run-dolphin.sh`,
then `tools/dev/dolphin-gdb.sh`):

- The CPU was looping inside `GX_WaitDrawDone`.
- `_gxwaitfinish` (the GX draw-done queue handle) was `0xFFFFFFFF` —
  never successfully initialized.
- `lwpc_queue_used` was `0x7fffffffffffffff`: every usable slot taken.

## Fix

`source/core/LwpQueueFix.cpp`, linked with
`-Wl,--wrap=LWP_InitQueue -Wl,--wrap=LWP_CloseQueue` (`Makefile`). `--wrap`
redirects every call, including the ones inside libogc itself, to the
wrapper:

- `__wrap_LWP_InitQueue(q)`: if this same handle variable `q` was given a
  queue by an earlier call, still holds that exact handle, and the queue
  was never closed, reuse it and return success. Otherwise allocate
  normally via `__real_LWP_InitQueue` and record which variable got which
  handle.
- `__wrap_LWP_CloseQueue(q)`: mark the handle closed and forget its owner,
  then close it for real.

Reusing the queue is what libogc meant to do anyway: the handle is a wait
list for the same event each time. A cleaner long-term fix would be
upstream in libogc (`GX_Init`/`VIDEO_Init` closing or reusing their queue);
the wrapper avoids patching libogc.

### Why the ownership check matters

The first version reused any handle that merely *looked* live (a value of
1–64 that hadn't been closed). That passed 55 sessions in Dolphin but
crashed at boot on a real Wii, every time, in different places (a DSI
inside zlib, then a corrupted SD-card devoptab inside `mkdir`).

The reason: Dolphin starts with zeroed RAM; a real Wii's RAM still holds
whatever the Homebrew Channel left there. Some code initializes queues in
uninitialized heap or stack memory. On hardware that garbage could be a
small number that looked like a live handle, so the wrapper handed back
*another subsystem's* queue. Two waiters then shared one queue, one woke
on the other's event before its IOS request (SD card, network) had
finished, and IOS later wrote its reply into memory that had already been
reused — heap corruption during startup.

Tracking the owning variable means only a handle this wrapper itself gave
to that exact variable is ever reused.

## Verification

- **Dolphin** (first version), `STRICT=1`, 55 playback sessions and 84
  scrubs (each scrub re-inits GX): no freeze, zero `LWP_InitQueue FAILED`.
  `lwpc_queue_used` read via GDB at the end was `0x3f` (6 slots), lower
  than the `0x7f` at boot.
- **Real Wii** (ownership-checking version, builds `c55c9594` and
  `0bd07330`): boots reliably, and two runs of 8 and 12 sessions absorbed
  up to 38 re-inits with no failures. Both runs ended in a separate GPU
  hang (still under investigation), not queue exhaustion.

`MemStats` debug-log lines include "queue re-inits absorbed N"
(`LwpQueueFix_ReusedCount()`), which should climb by a few per session.

## Files

- `source/core/LwpQueueFix.cpp` — the wrapper
- `Makefile` — `--wrap` linker flags
- `tools/dev/dolphin-gdb.sh` — persistent gdb session against Dolphin's stub (debugging tooling, not part of the fix)
