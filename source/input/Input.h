#pragma once
#include <wiiuse/wpad.h>

class Input {
public:
    static void update();
    static bool isHomePressed();
    static bool isUpPressed();
    static bool isDownPressed();
    static bool isLeftPressed();
    static bool isRightPressed();
    static bool isAJustPressed();
    static bool isBPressed();
    static bool isBackPressed();  // alias for B
    static bool isLPressed();
    static bool isRPressed();

    // Reads a GameCube pad (if connected) and translates its buttons onto
    // WPAD_BUTTON_* bit positions, so the result can be OR'd directly into a
    // WPAD_ButtonsDown()/WPAD_ButtonsHeld()-shaped value -- letting any
    // caller (this class, or WiiPlayer.cpp's background playback thread)
    // treat "Wiimote, Classic Controller, or GameCube pad" uniformly without
    // duplicating the translation table. Classic Controller buttons need no
    // separate handling: WPAD_ButtonsDown/Held/Up() already OR them into the
    // same return value as core Wiimote buttons (upper 16 bits), since a
    // Classic Controller is read as a Wiimote expansion, not a separate
    // device.
    //
    // Pass held=false for edge-triggered ("just pressed this poll") checks
    // (reads PAD_ButtonsDown), held=true for continuously-held checks (reads
    // PAD_ButtonsHeld). Safe to call even with no GameCube pad connected
    // (returns 0).
    static u32 gcButtonsAsWpad(bool held);

    // GC C-stick X: -1/+1 while held past the deadzone, else 0. Not edge-triggered.
    static int gcCStickHeldX();
};
