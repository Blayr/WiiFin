#include "Input.h"
#include <wiiuse/wpad.h>
#include <ogc/pad.h>

static u32 buttonsDown = 0;

void Input::update() {
    WPAD_ScanPads();
    PAD_ScanPads();
    buttonsDown = WPAD_ButtonsDown(0) | Input::gcButtonsAsWpad(/*held=*/false);
}

// Main stick deflection translated to a WPAD_BUTTON_UP/DOWN/LEFT/RIGHT-shaped
// bitmask -- most players rest a thumb on the stick rather than the digital
// D-pad, so treat a clear deflection as the same "direction pressed" signal.
static u32 gcStickDirBits() {
    s8 x = PAD_StickX(PAD_CHAN0);
    s8 y = PAD_StickY(PAD_CHAN0);
    const s8 DEADZONE = 60; // PAD_StickX/Y is roughly -100..100 once calibrated
    u32 out = 0;
    if (y >  DEADZONE) out |= WPAD_BUTTON_UP;
    if (y < -DEADZONE) out |= WPAD_BUTTON_DOWN;
    if (x < -DEADZONE) out |= WPAD_BUTTON_LEFT;
    if (x >  DEADZONE) out |= WPAD_BUTTON_RIGHT;
    return out;
}

u32 Input::gcButtonsAsWpad(bool held) {
    u16 pad = held ? PAD_ButtonsHeld(PAD_CHAN0) : PAD_ButtonsDown(PAD_CHAN0);

    u32 out = 0;
    u32 dirBits = 0;
    if (pad & PAD_BUTTON_UP)    dirBits |= WPAD_BUTTON_UP;
    if (pad & PAD_BUTTON_DOWN)  dirBits |= WPAD_BUTTON_DOWN;
    if (pad & PAD_BUTTON_LEFT)  dirBits |= WPAD_BUTTON_LEFT;
    if (pad & PAD_BUTTON_RIGHT) dirBits |= WPAD_BUTTON_RIGHT;

    u32 curStick = gcStickDirBits();
    if (held) {
        dirBits |= curStick;
    } else {
        // Edge-detect the same way WPAD_ButtonsDown() does: only the frame
        // the stick crosses the deadzone counts as "just pressed", or every
        // frame it's held past the deadzone would register as a fresh press.
        static u32 prevStick = 0;
        dirBits |= curStick & ~prevStick;
        prevStick = curStick;
    }

    /* The GC pad's small digital D-pad (and the analog stick's deadzone
     * corners) can report a spurious secondary axis alongside an intended
     * press -- e.g. Down registering with a faint Right too. Harmless for
     * movement, but wrong for this app's menus, where Up/Down and
     * Left/Right are bound to unrelated actions (volume vs. seek): a
     * diagonal blip must never fire both from one press. Vertical wins. */
    if (dirBits & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN))
        dirBits &= ~(WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT);
    out |= dirBits;

    if (pad & PAD_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (pad & PAD_BUTTON_B)     out |= WPAD_BUTTON_B;
    // isLPressed()/isRPressed() are bound to Wiimote -/+ (tab/page switching),
    // not literal shoulder buttons -- L/R trigger-click is the natural
    // GameCube equivalent of that same "switch page/tab" action.
    if (pad & PAD_TRIGGER_L)    out |= WPAD_BUTTON_MINUS;
    if (pad & PAD_TRIGGER_R)    out |= WPAD_BUTTON_PLUS;
    // No GameCube equivalent for HOME -- Start/Menu is the closest analog.
    if (pad & PAD_BUTTON_START) out |= WPAD_BUTTON_HOME;
    // X/Y reserved as the eventual GameCube equivalents of WPAD_BUTTON_1/2
    // (search shortcut, audio/subtitle track cycling) -- those call sites
    // still read WPAD_ButtonsDown(0) directly today, so this mapping has no
    // effect yet; harmless to have in place ahead of that follow-up.
    if (pad & PAD_BUTTON_X)     out |= WPAD_BUTTON_1;
    if (pad & PAD_BUTTON_Y)     out |= WPAD_BUTTON_2;

    return out;
}

bool Input::isHomePressed()  { return buttonsDown & WPAD_BUTTON_HOME; }
bool Input::isUpPressed()    { return buttonsDown & WPAD_BUTTON_UP; }
bool Input::isDownPressed()  { return buttonsDown & WPAD_BUTTON_DOWN; }
bool Input::isLeftPressed()  { return buttonsDown & WPAD_BUTTON_LEFT; }
bool Input::isRightPressed() { return buttonsDown & WPAD_BUTTON_RIGHT; }
bool Input::isAJustPressed() { return buttonsDown & WPAD_BUTTON_A; }
bool Input::isBPressed()     { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isBackPressed()  { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isLPressed()     { return buttonsDown & WPAD_BUTTON_MINUS; }
bool Input::isRPressed()     { return buttonsDown & WPAD_BUTTON_PLUS; }
