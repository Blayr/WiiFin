# GameCube pad: B held at playback start quits playback instantly

## Symptoms

Starting playback with a GameCube controller sometimes stopped it straight
away and dropped back to the menu, which could then retry and stop again
— a loop. Mostly seen right after pressing B to back out of something,
then starting a new playback while B was still physically down.

## Root cause

The player's input loop in `WiiPlayer.cpp` detects button *presses* as
"held now, not held last frame". The Wiimote's previous-state value was
seeded from the real current state at session start, but the GC pad's was
not — it started at 0. So any GC button already held when the session
began (most often B) counted as a fresh press on the first frame, and B
with the overlay hidden means "stop playback".

## Fix

At session start, scan the pads and seed both the GC-specific and combined
previous-state values from the current state:

```cpp
PAD_ScanPads();
u32 prevGcHeld = Input::gcButtonsAsWpad(/*held=*/true);
u32 prevHeld = (wd0 ? wd0->btns_h : 0) | prevGcHeld;
```

A button held at session start now has to be released and pressed again
to do anything.

## Files

- `source/player/WiiPlayer.cpp` — seeding at the top of the input loop
