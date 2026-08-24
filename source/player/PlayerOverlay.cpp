/*
 * PlayerOverlay.cpp — in-player control HUD for WiiFin.
 *
 * Visual layer:
 *   The progress bar is drawn directly into the Y-plane (luma) texture of the
 *   video before the GPU renders it.  This means no GX state changes and no
 *   GRRLIB calls are needed: we just write pixel brightness values into memory.
 *
 *   Text labels (time, track names, skip-intro prompt, pause indicator) are
 *   displayed through MPlayer's built-in OSD via mp_input_queue_cmd(), which
 *   is safe to call from a background thread (mutex-protected in MPlayer CE).
 *
 * Button mapping (Wii Remote held horizontally):
 *   A            — toggle play/pause  (when overlay visible)
 *                  confirm selection  (in sub-menus)
 *   B            — close overlay / cancel sub-menu
 *   ← / →        — seek −10 s / +10 s
 *   ↑ / ↓        — volume up / down
 *   +            — next episode  (only if available)
 *   −            — previous episode  (only if available)
 *   1            — open audio track picker
 *   2            — open subtitle track picker
 *   HOME         — stop playback (return to library)
 *   Any button   — reveal overlay if hidden
 */

#include "PlayerOverlay.h"
#include "WiiPlayer.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ogc/system.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

/* Defined in WiiPlayer.cpp (extern "C") — consumed flag set by vo_gx.c's
 * GX overlay when button A is handled there; read here to skip A-press. */
extern "C" volatile int g_wiifin_gx_btn_consumed;

/* GX overlay callback — registered by vo_gx.c preinit() when video mode is
 * active; nullptr in audio mode.  Used here to detect whether ov_tick is
 * running (video mode) or not (audio mode) so bgThread can decide whether
 * to handle A-press actions itself or leave them to ov_tick. */
extern "C" void (*g_wiifin_gx_overlay_cb)(void);

/* Defined in WiiPlayer.cpp (extern "C"):
 *   g_wiifin_gx_overlay_visible — set by ov_tick(); read in onFrame() to
 *                                 skip Y-texture controls when GX overlay
 *                                 is already drawing them. */
extern "C" volatile int g_wiifin_gx_overlay_visible;
/* Set to 1 before calling wii_player_stop() so ov_gx_draw blacks out
 * immediately without waiting for async_quit_request to propagate. */
extern "C" volatile int g_wiifin_stopping;

/* -----------------------------------------------------------------------
 * Y-texture pixel helpers
 *
 * Format: GX_TF_I8, 8×4 tiles — byte offset:
 *   ((y & ~3) * W) + ((x & ~7) << 2) + ((y & 3) << 3) + (x & 7)
 * ----------------------------------------------------------------------- */
static inline void setYpix(uint8_t* Y, int W, int x, int y, uint8_t v)
{
    Y[ ((y & ~3) * W) + ((x & ~7) << 2) + ((y & 3) << 3) + (x & 7) ] = v;
}

/* -----------------------------------------------------------------------
 * Compact 5×7 pixel font — column-major, bit 0 = top row.
 * 95 glyphs, ASCII 0x20 (space) to 0x7E (~), 5 bytes per glyph.
 * ----------------------------------------------------------------------- */
static const uint8_t FONT5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '\''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 0x30 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ';'
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D '='
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 0x4D 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 0x59 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C 'l'
    {0x7C,0x04,0x18,0x04,0x7C}, // 0x6D 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 't'
    {0x3C,0x40,0x40,0x40,0x3C}, // 0x75 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 'v'
    {0x3C,0x40,0x20,0x40,0x3C}, // 0x77 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 0x79 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D '}'
    {0x08,0x04,0x08,0x10,0x08}, // 0x7E '~'
};

/* Draw one glyph scaled s× into the Y luma texture. cx,cy = top-left corner. */
static void drawCharScaled(uint8_t* Y, int W, int cx, int cy,
                            char c, uint8_t val, int s)
{
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return;
    const uint8_t* col = FONT5x7[(unsigned char)c - 0x20];
    for (int fc = 0; fc < 5; fc++) {
        uint8_t bits = col[fc];
        for (int fr = 0; fr < 7; fr++) {
            if (bits & (1 << fr)) {
                for (int sy = 0; sy < s; sy++)
                    for (int sx = 0; sx < s; sx++)
                        setYpix(Y, W, cx + fc*s + sx, cy + fr*s + sy, val);
            }
        }
    }
}

/* Returns the pixel width of 'text' rendered at scale s (5px char + 1px gap). */
static int textWidthScaled(const char* text, int s)
{
    int n = 0; while (text[n]) n++;
    return n > 0 ? (6*n - 1) * s : 0;
}

/* Render null-terminated ASCII string into Y luma texture at scale s. */
static void drawTextScaled(uint8_t* Y, int W, int x, int y,
                            const char* text, uint8_t val, int s)
{
    for (int i = 0; text[i]; i++)
        drawCharScaled(Y, W, x + i * 6 * s, y, text[i], val, s);
}

/* -----------------------------------------------------------------------
 * Control-button layout constants (texture-space, 848×480)
 *
 * 5 buttons, each BWIDTH px wide, spaced BGAP px apart, centred on the
 * 848-wide texture.  Vertically above the 20-row progress bar.
 * ----------------------------------------------------------------------- */
static const int CTRL_Y0   = 416;
static const int CTRL_Y1   = 459;  /* 44 rows */
static const int CTRL_BW   = 48;   /* button width  */
static const int CTRL_BGAP = 10;   /* gap between buttons */
/* total = 5×48 + 4×10 = 280  →  start at (848-280)/2 = 284 */
static const int CTRL_BX0  = 284;

/* Return the left texture-x of button i (0–4). */
static inline int btnLeft(int i)  { return CTRL_BX0 + i * (CTRL_BW + CTRL_BGAP); }
/* Return the right texture-x of button i (0–4). */
static inline int btnRight(int i) { return btnLeft(i) + CTRL_BW - 1; }

/* Map a texture-x to the IR-space x (IR VRes = 640×480, texture = 848×480). */
static inline float texToIr(int texX) { return texX * 640.0f / 848.0f; }

/* -----------------------------------------------------------------------
 * Singleton
 * ----------------------------------------------------------------------- */
PlayerOverlay* PlayerOverlay::instance = nullptr;

/* -----------------------------------------------------------------------
 * Construction
 * ----------------------------------------------------------------------- */
PlayerOverlay::PlayerOverlay(PlayerOverlayContext& c) : ctx(c)
{
    instance = this;

    /* Seed picker selections from context */
    audioPickSel = 0;
    for (int i = 0; i < (int)ctx.audioStreams.size(); ++i)
        if (ctx.audioStreams[i].index == ctx.currentAudio) { audioPickSel = i; break; }

    subPickSel = 0; /* 0 = "None" in the sub list */
    for (int i = 0; i < (int)ctx.subStreams.size(); ++i)
        if (ctx.subStreams[i].index == ctx.currentSub) { subPickSel = i + 1; break; }

    /* Starts hidden (State::Hidden is the default member initializer) --
     * any button or IR movement reveals it, per the Hidden-state handling
     * in tick(). */
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
const char* PlayerOverlay::fmtTime(float secs, char* buf, int bufsz)
{
    int s  = (int)secs;
    int h  = s / 3600;
    int m  = (s % 3600) / 60;
    int ss = s % 60;
    if (h > 0)
        snprintf(buf, (size_t)bufsz, "%d:%02d:%02d", h, m, ss);
    else
        snprintf(buf, (size_t)bufsz, "%d:%02d", m, ss);
    return buf;
}

void PlayerOverlay::showOSD(const char* text, int durationMs)
{
    wii_player_show_osd(text, durationMs);
}

void PlayerOverlay::refreshTimeOSD(float timePos, int paused)
{
    char tPos[16], tDur[16], line[64];
    fmtTime(timePos, tPos, sizeof(tPos));
    fmtTime(g_mplayer_duration > 0 ? g_mplayer_duration : 0, tDur, sizeof(tDur));

    if (paused)
        snprintf(line, sizeof(line), "|| PAUSE  %s / %s", tPos, tDur);
    else
        snprintf(line, sizeof(line), ">  %s / %s", tPos, tDur);

    showOSD(line, 5000);
}

/* Activate main overlay (seek bar + controls) */
void PlayerOverlay::activateMain()
{
    state      = State::Main;
    hideTimer  = 150; /* 5 s at ~30 Hz */
    mainFocus  = 2;   /* Play/Pause -- most-used action, native icon row only */
    barFocused = false;
}

void PlayerOverlay::activateHelp()
{
    state     = State::Help;
    hideTimer = 300;
}

void PlayerOverlay::activateAudioPick()
{
    state     = State::AudioPick;
    hideTimer = 300;
    if (ctx.audioStreams.empty()) { activateMain(); return; }

    char buf[512] = "Audio track:\n";
    for (int i = 0; i < (int)ctx.audioStreams.size(); ++i) {
        char line[96];
        const char* marker = (i == audioPickSel) ? "> " : "  ";
        snprintf(line, sizeof(line), "%s[%d] %s\n",
                 marker, i + 1, ctx.audioStreams[i].displayTitle.c_str());
        if (strlen(buf) + strlen(line) + 1 < sizeof(buf))
            strcat(buf, line);
    }
    showOSD(buf, 10000);
}

void PlayerOverlay::activateSubPick()
{
    state     = State::SubPick;
    hideTimer = 300;

    char buf[512] = "Subtitles:\n";
    const char* marker = (subPickSel == 0) ? "> " : "  ";
    char line[96];
    snprintf(line, sizeof(line), "%sOff\n", marker);
        strcat(buf, line);
    for (int i = 0; i < (int)ctx.subStreams.size(); ++i) {
        marker = (i + 1 == subPickSel) ? "> " : "  ";
        const MediaStream& s = ctx.subStreams[i];
        // Build a readable label: prefer DisplayTitle, fall back to
        // language + codec, then just "Track N" if both are absent.
        char label[64];
        if (!s.displayTitle.empty()) {
            snprintf(label, sizeof(label), "%s", s.displayTitle.c_str());
        } else if (!s.language.empty() && !s.codec.empty()) {
            snprintf(label, sizeof(label), "%s (%s)", s.language.c_str(), s.codec.c_str());
        } else if (!s.language.empty()) {
            snprintf(label, sizeof(label), "%s", s.language.c_str());
        } else if (!s.codec.empty()) {
            snprintf(label, sizeof(label), "%s", s.codec.c_str());
        } else {
            snprintf(label, sizeof(label), "Track %d", i + 1);
        }
        snprintf(line, sizeof(line), "%s[%d] %s\n", marker, i + 1, label);
        if (strlen(buf) + strlen(line) + 1 < sizeof(buf))
            strcat(buf, line);
    }
    showOSD(buf, 10000);
}

void PlayerOverlay::hide()
{
    state       = State::Hidden;
    hideTimer   = 0;
    nativeMode  = false;
    barFocused  = false;
    /* Clear the OSD text */
    showOSD(" ", 1);
}

void PlayerOverlay::adjustVolume(int delta)
{
    if (delta > 0) wii_player_vol_up();
    else            wii_player_vol_down();
    volLevel += delta;
    if (volLevel < 0)          volLevel = 0;
    if (volLevel > volLevelMax) volLevel = volLevelMax;
    volIndicatorTimer = 60; /* ~2 s at the ~30 Hz bgThread tick rate */
}

void PlayerOverlay::showBitrateIndicator(int kbps, int resW, int resH)
{
    // Describes this specific bitrate choice, not a claim about the
    // physical connection (wired-vs-wireless labeling turned out to be
    // unreliable -- see JellyfinClient::networkKbps's comment). Buckets are
    // relative to the app's own floor(700)/ceiling(3000) range.
    const char* label = kbps >= 2200 ? "Fast" : kbps >= 1200 ? "Medium" : "Slow";
    snprintf(bitrateIndicatorText, sizeof(bitrateIndicatorText),
             "%d kbps %dx%d (%s)", kbps, resW, resH, label);
    // Don't start the countdown yet -- onFrame() does that on the first real
    // frame it draws (see the loading_active check there).
    bitrateIndicatorPending = true;
    bitrateIndicatorExpireTicks = 0;
}

void PlayerOverlay::updateIntroPrompt(float timePos)
{
    if (!ctx.intro.hasIntro) return;

    float showAt = ctx.intro.showPromptAt > 0
                   ? ctx.intro.showPromptAt
                   : ctx.intro.introStart;
    float hideAt = ctx.intro.hidePromptAt > 0
                   ? ctx.intro.hidePromptAt
                   : ctx.intro.introEnd + 2.0f;

    bool inRange = (timePos >= showAt && timePos < hideAt);
    if (inRange && !introPromptShown) {
        introPromptShown = true;
        showOSD("Press A to skip intro", 6000);
    } else if (!inRange && introPromptShown) {
        introPromptShown = false;
    }
}

/* -----------------------------------------------------------------------
 * tick() — called ~30 Hz from the background LWP thread
 * ----------------------------------------------------------------------- */
void PlayerOverlay::tick(float timePos, int paused, uint32_t btnsDown, uint32_t btnsHeld,
                         float irX, float irY, bool irValid, uint32_t gcDown)
{
    (void)btnsHeld; /* reserved for hold-to-seek in the future */

    if (volIndicatorTimer > 0) --volIndicatorTimer;

    /* If the GX overlay (vo_gx.c) already handled buttons this frame,
     * drop them from btnsDown so we don't double-fire actions.
     * g_wiifin_gx_btn_consumed is a WPAD bitmask of consumed buttons. */
    if (g_wiifin_gx_btn_consumed) {
        btnsDown &= ~(uint32_t)g_wiifin_gx_btn_consumed;
        g_wiifin_gx_btn_consumed = 0;
    }

    /* ---- IR pointer: track hover, reset auto-hide -- but do NOT auto-reveal
     * from Hidden. It used to: any IR movement flipped nativeMode to false
     * and revealed vo_gx.c's old overlay -- but WPAD tracks IR from a
     * Wiimote that's merely powered on and roughly facing the sensor bar,
     * whether or not it's the controller actually in use. A GC-pad session
     * would randomly get yanked into the old overlay from incidental
     * Wiimote presence. Now the old overlay only ever appears from a real
     * Wiimote *button* press (still unavoidable -- vo_gx.c reads raw WPAD
     * independently and can't be told about WiiFin's overlay state without
     * patching the vendored MPlayer library), never from pointer movement
     * alone. */
    if (irValid) {
        bool moved = (lastIrX >= 0.0f) &&
                     ((irX - lastIrX) * (irX - lastIrX) +
                      (irY - lastIrY) * (irY - lastIrY)) > 16.0f; /* >4 px */
        if (moved && state == State::Main) {
            hideTimer = 150; /* reset auto-hide on pointer movement */
        }
        lastIrX = irX;
        lastIrY = irY;
    } else {
        lastIrX = lastIrY = -1.0f;
    }

    /* Update hover button index (render thread reads this in onFrame) */
    irHover = -1;
    if (irValid && state == State::Main &&
        irY >= (float)CTRL_Y0 && irY <= (float)CTRL_Y1) {
        for (int i = 0; i < 5; ++i) {
            if (irX >= texToIr(btnLeft(i)) && irX <= texToIr(btnRight(i))) {
                irHover = i;
                break;
            }
        }
    }
    /* Check intro prompt independently of overlay visibility */
    updateIntroPrompt(timePos);

    /* HOME: stop player; runPlaySession will show the clean GRRLIB HOME
     * overlay. Checked before the Hidden-state handling below so it always
     * works regardless of overlay visibility. */
    if (btnsDown & WPAD_BUTTON_HOME) {
        g_wiifin_stopping    = 1;
        g_player_stop_reason = PLAYER_STOP_HOME;
        wii_player_stop();
        return;
    }

    /* While hidden: A reveals the overlay. Left/Right/etc are deliberately
     * NOT wake buttons for a real Wiimote press -- vo_gx.c's own independent
     * seek/pause handling on MPlayer's main thread has no knowledge of
     * WiiFin's overlay visibility and can't be gated by it, so a raw
     * Wiimote button here must stay a no-op to avoid double-handling. B
     * exits playback directly (paired with B hiding the overlay from
     * State::Main below, so B reads as a natural "step back" -- first press
     * hides controls, second press exits).
     *
     * GC-pad presses are the exception: vo_gx.c never sees GC input at all,
     * so there's no double-handling risk -- let seek/volume/episode-switch
     * work directly as power-user shortcuts without needing to open the
     * menu first. Every other (real-Wiimote-sourced) button is swallowed
     * with no effect while hidden. */
    if (state == State::Hidden) {
        if (btnsDown & WPAD_BUTTON_B) {
            g_wiifin_stopping    = 1;
            g_player_stop_reason = PLAYER_STOP_EOF;
            wii_player_stop();
            return;
        }
        if (btnsDown & WPAD_BUTTON_A) {
            /* vo_gx.c never sees GC pad state, so if this A came from the
             * GC pad, its own overlay/pause handling is guaranteed inactive
             * -- safe to own pause/seek ourselves and draw our own HUD. */
            nativeMode = (gcDown & WPAD_BUTTON_A) != 0;
            activateMain();
            refreshTimeOSD(timePos, paused);
            return;
        }
        if (gcDown & WPAD_BUTTON_LEFT)  { wii_player_seek_rel(-10.0f); showOSD("-10s", 1200); }
        if (gcDown & WPAD_BUTTON_RIGHT) { wii_player_seek_rel(+10.0f); showOSD("+10s", 1200); }
        if (gcDown & WPAD_BUTTON_UP)    adjustVolume(+1);
        if (gcDown & WPAD_BUTTON_DOWN)  adjustVolume(-1);
        if (gcDown & WPAD_BUTTON_MINUS) {
            if (ctx.episodeIdx > 0) wii_player_request_prev();
            else showOSD("No previous episode", 1500);
        }
        if (gcDown & WPAD_BUTTON_PLUS) {
            if ((int)ctx.episodeIdx + 1 < (int)ctx.episodes.size()) wii_player_request_next();
            else showOSD("No next episode", 1500);
        }
        return; /* consume every other button while hidden */
    }

    /* AUTO-HIDE timer — disabled while video is paused (keep the overlay
     * visible so the user can click play to resume), or while the Help
     * panel or a track picker is open (reading/choosing shouldn't race a
     * timer, and closing back out of the menu it interrupted is jarring). */
    if (state != State::Hidden) {
        if (g_mplayer_paused || state == State::Help ||
            state == State::AudioPick || state == State::SubPick) {
            /* Freeze timer and keep overlay alive — ensure it stays visible */
            if (hideTimer <= 0) hideTimer = 1;
        } else {
            if (hideTimer > 0) --hideTimer;
            else { hide(); return; }
        }
    }

    /* Update time display once per second */
    if (state == State::Main) {
        if ((int)timePos != (int)lastTimePos)
            refreshTimeOSD(timePos, paused);
        lastTimePos = timePos;
    }

    /* ---- Input handling per state ---- */
    switch (state) {

    case State::Main: {
        if (nativeMode && barFocused) {
            /* Scrubbing the progress bar (entered via Down from the icon
             * row below): Left/Right move a pending seek target instead of
             * actually seeking on every press -- lets you land on an
             * arbitrary position (e.g. 16:30) instead of only ever moving
             * 10s at a time. Tapping steps a fixed amount; holding
             * accelerates (throttled to every few ticks so it doesn't fly
             * past the target at ~30 Hz) so short and long jumps are both
             * practical. A commits with wii_player_seek_abs(); B or Up
             * cancel back to the icon row without seeking. */
            bool holding = (btnsHeld & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) != 0;
            if (holding) {
                barHoldFrames++;
                barMissFrames = 0;
            } else {
                /* Tolerate a few missed ticks before giving up on the hold --
                 * a genuinely-released button stays missed past this window,
                 * but a single noisy tick (e.g. GC pad diagonal-suppression
                 * briefly zeroing L/R when a worn stick/D-pad bleeds a tiny
                 * vertical component) no longer restarts acceleration from
                 * scratch, which was making long holds never get past the
                 * slowest step tier. */
                if (++barMissFrames > 3) barHoldFrames = 0;
            }
            bool activeHold = barHoldFrames > 0 && barMissFrames <= 3;

            bool freshPress = (btnsDown & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) != 0;
            bool repeatTick = activeHold && !freshPress && barHoldFrames > 12 && (barHoldFrames % 4 == 0);
            if (freshPress || repeatTick) {
                float step = 5.0f;
                if (barHoldFrames > 90)      step = 60.0f; /* held >3s: fly across long content */
                else if (barHoldFrames > 45) step = 20.0f; /* held >1.5s */
                if (btnsHeld & WPAD_BUTTON_LEFT)  scrubPos -= step;
                if (btnsHeld & WPAD_BUTTON_RIGHT) scrubPos += step;
                if (scrubPos < 0.0f) scrubPos = 0.0f;
                if (g_mplayer_duration > 0.0f && scrubPos > g_mplayer_duration)
                    scrubPos = g_mplayer_duration;
                hideTimer = 150;
            }

            if (btnsDown & WPAD_BUTTON_A) {
                /* A plain in-stream seek only works reliably within whatever
                 * the demuxer has already buffered -- jumping far ahead of
                 * that (the whole point of the scrub bar) silently lands
                 * near the edge of the buffered window instead of the real
                 * target. Request a fresh transcode at the target position
                 * instead (same mechanism as an audio/sub track switch). */
                wii_player_request_seek(scrubPos);
                barFocused = false;
                hideTimer  = 150;
            }
            if (btnsDown & (WPAD_BUTTON_B | WPAD_BUTTON_UP)) {
                barFocused = false;
                refreshTimeOSD(timePos, paused); /* restore the real position readout */
                hideTimer  = 150;
            }
            break;
        }

        if (nativeMode) {
            /* Native icon row (see drawNativeControls): Left/Right move
             * focus between 6 icons -- Prev/-10s/Play-Pause/+10s/Next/Help
             * -- A activates the focused one. vo_gx.c never sees GC input,
             * so it's always safe to act on these directly (no ov_tick
             * double-handling risk, unlike the legacy Wiimote/IR branch
             * below). L/R-trigger stays a direct shortcut even while the
             * menu is shown -- it doesn't conflict with the horizontal
             * focus axis. Up/Down used to be direct volume shortcuts here;
             * Down now opens the scrub bar above instead, so volume only
             * moves to the icon row's Up/Down when nothing else claims it
             * -- it's still reachable via the Hidden-state GC shortcuts. */
            if (btnsDown & WPAD_BUTTON_LEFT)  { mainFocus = (mainFocus + 5) % 6; hideTimer = 150; }
            if (btnsDown & WPAD_BUTTON_RIGHT) { mainFocus = (mainFocus + 1) % 6; hideTimer = 150; }

            if (btnsDown & WPAD_BUTTON_A) {
                if (introPromptShown && ctx.intro.hasIntro && !g_mplayer_paused) {
                    wii_player_seek_abs(ctx.intro.introEnd + 0.5f);
                    introPromptShown = false;
                    showOSD("Skipped intro", 2000);
                } else switch (mainFocus) {
                case 0: /* Prev episode */
                    if (ctx.episodeIdx > 0) wii_player_request_prev();
                    else showOSD("No previous episode", 2000);
                    break;
                case 1: /* Seek -10 s */
                    wii_player_seek_rel(-10.0f);
                    refreshTimeOSD(timePos - 10.0f, paused);
                    break;
                case 2: /* Play/Pause */
                    wii_player_pause_toggle();
                    break;
                case 3: /* Seek +10 s */
                    wii_player_seek_rel(+10.0f);
                    refreshTimeOSD(timePos + 10.0f, paused);
                    break;
                case 4: /* Next episode */
                    if ((int)ctx.episodeIdx + 1 < (int)ctx.episodes.size())
                        wii_player_request_next();
                    else showOSD("No next episode", 2000);
                    break;
                case 5: /* Help */
                    activateHelp();
                    return;
                }
                hideTimer = 150;
            }
            if (btnsDown & WPAD_BUTTON_B) { hide(); break; }
            if (btnsDown & WPAD_BUTTON_DOWN) {
                barFocused    = true;
                scrubPos      = timePos;
                barHoldFrames = 0;
                barMissFrames = 0;
                hideTimer     = 150;
            } else if (btnsDown & WPAD_BUTTON_UP) {
                adjustVolume(+1);
            }
            if (btnsDown & WPAD_BUTTON_PLUS) {
                if ((int)ctx.episodeIdx + 1 < (int)ctx.episodes.size())
                    wii_player_request_next();
                else showOSD("No next episode", 2000);
            }
            if (btnsDown & WPAD_BUTTON_MINUS) {
                if (ctx.episodeIdx > 0) wii_player_request_prev();
                else showOSD("No previous episode", 2000);
            }
            if ((btnsDown & WPAD_BUTTON_1) && !ctx.audioStreams.empty())
                activateAudioPick();
            if (btnsDown & WPAD_BUTTON_2)
                activateSubPick();
            break;
        }

        if (btnsDown & WPAD_BUTTON_A) {
            /* In video mode, ov_tick (vo_gx.c) already processes every A press
             * from its own thread via mp_input_queue_cmd("pause") or the seek
             * path.  Letting bgThread also call wii_player_pause_toggle() for
             * the same press triggers a double-toggle: video pauses then
             * immediately resumes (or double-seeks).
             *
             * Detect video mode by g_wiifin_gx_overlay_cb being non-null
             * (registered by vo_gx preinit; null in -vo null / audio mode).
             * In video mode, skip the pause/seek actions below and let ov_tick
             * own them.  The intro-skip is WiiFin-specific so it is kept for
             * both modes.
             *
             * g_wiifin_gx_btn_consumed: A already removed from btnsDown at the
             * top of this function (line ~511) when ov_tick set the flag — keep
             * that early-out as a belt-and-braces guard for cases not covered
             * by the video-mode check.
             *
             * This legacy branch only ever runs for non-native (real
             * Wiimote/IR) sessions -- see the nativeMode branch above. */
            bool videoMode = (g_wiifin_gx_overlay_cb != nullptr);

            if (irValid && irY >= 460.0f) {
                /* IR click on progress bar → seek (only when not paused and
                 * duration is known).  ov_tick handles this too in video mode
                 * (~offset 0x10e4 of ov_tick), so skip in video mode. */
                if (!videoMode && !g_mplayer_paused && g_mplayer_duration > 0.5f) {
                    float frac = irX / 640.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    float target = frac * g_mplayer_duration;
                    wii_player_seek_abs(target);
                    refreshTimeOSD(target, paused);
                }
            } else if (irHover >= 0) {
                /* IR click on a HUD control button. */
                switch (irHover) {
                case 0: /* Prev episode */
                    if (ctx.episodeIdx > 0) wii_player_request_prev();
                    else showOSD("No previous episode", 2000);
                    break;
                case 1: /* Seek -10 s */
                    if (!g_mplayer_paused) {
                        wii_player_seek_rel(-10.0f);
                        refreshTimeOSD(timePos - 10.0f, paused);
                    }
                    break;
                case 2: /* Play/Pause — ov_tick owns this in video mode. */
                    if (!videoMode)
                        wii_player_pause_toggle();
                    break;
                case 3: /* Seek +10 s */
                    if (!g_mplayer_paused) {
                        wii_player_seek_rel(+10.0f);
                        refreshTimeOSD(timePos + 10.0f, paused);
                    }
                    break;
                case 4: /* Next episode */
                    if ((int)ctx.episodeIdx + 1 < (int)ctx.episodes.size())
                        wii_player_request_next();
                    else showOSD("No next episode", 2000);
                    break;
                }
            } else {
                /* A with no specific IR target. */
                if (introPromptShown && ctx.intro.hasIntro && !g_mplayer_paused) {
                    /* Intro-skip: WiiFin-specific, ov_tick has no knowledge of
                     * it, so bgThread handles it in both audio and video mode. */
                    wii_player_seek_abs(ctx.intro.introEnd + 0.5f);
                    introPromptShown = false;
                    showOSD("Skipped intro", 2000);
                } else if (!videoMode) {
                    /* No-hover pause toggle: ov_tick owns this in video mode
                     * (offset 0x10b0 of ov_tick queues "pause" when IR is
                     * invalid or cursor is outside all button regions). */
                    wii_player_pause_toggle();
                }
            }
            hideTimer = 150;
        }
        if (btnsDown & WPAD_BUTTON_B) {
            /* First B press hides the controls rather than exiting outright
             * -- see the State::Hidden handling above for the second press. */
            hide();
            break;
        }

        if (btnsDown & WPAD_BUTTON_RIGHT) {
            wii_player_seek_rel(+10.0f);
            hideTimer = 150;
            refreshTimeOSD(timePos + 10.0f, paused);
        }
        if (btnsDown & WPAD_BUTTON_LEFT) {
            wii_player_seek_rel(-10.0f);
            hideTimer = 150;
            refreshTimeOSD(timePos - 10.0f, paused);
        }
        if (btnsDown & WPAD_BUTTON_UP)   { adjustVolume(+1); }
        if (btnsDown & WPAD_BUTTON_DOWN) { adjustVolume(-1); }

        if (btnsDown & WPAD_BUTTON_PLUS) {
            if ((int)ctx.episodeIdx + 1 < (int)ctx.episodes.size())
                wii_player_request_next();
            else
                showOSD("No next episode", 2000);
        }
        if (btnsDown & WPAD_BUTTON_MINUS) {
            if (ctx.episodeIdx > 0)
                wii_player_request_prev();
            else
                showOSD("No previous episode", 2000);
        }

        if ((btnsDown & WPAD_BUTTON_1) && !ctx.audioStreams.empty())
            activateAudioPick();
        if ((btnsDown & WPAD_BUTTON_2))
            activateSubPick();
        break;
    }

    case State::AudioPick: {
        if (btnsDown & WPAD_BUTTON_B)    { activateMain(); break; }
        if (btnsDown & WPAD_BUTTON_UP) {
            if (audioPickSel > 0) --audioPickSel;
            activateAudioPick(); /* re-render with new selection */
        }
        if (btnsDown & WPAD_BUTTON_DOWN) {
            if (audioPickSel + 1 < (int)ctx.audioStreams.size()) ++audioPickSel;
            activateAudioPick();
        }
        if (btnsDown & WPAD_BUTTON_A) {
            ctx.selectedAudio = ctx.audioStreams[audioPickSel].index;
            wii_player_request_audio_switch();
        }
        break;
    }

    case State::SubPick: {
        if (btnsDown & WPAD_BUTTON_B)    { activateMain(); break; }
        if (btnsDown & WPAD_BUTTON_UP) {
            if (subPickSel > 0) --subPickSel;
            activateSubPick();
        }
        if (btnsDown & WPAD_BUTTON_DOWN) {
            if (subPickSel < (int)ctx.subStreams.size()) ++subPickSel;
            activateSubPick();
        }
        if (btnsDown & WPAD_BUTTON_A) {
            ctx.selectedSub = (subPickSel == 0) ? -1
                              : ctx.subStreams[subPickSel - 1].index;
            wii_player_request_sub_switch();
        }
        break;
    }

    case State::Help: {
        if ((btnsDown & WPAD_BUTTON_B) || (btnsDown & WPAD_BUTTON_A)) {
            /* Return to Main without activateMain()'s mainFocus reset --
             * focus was already sitting on the "?" icon (mainFocus == 5)
             * when Help was opened, so leaving it alone means closing
             * Help lands back on "?" instead of jumping to Play/Pause. */
            state     = State::Main;
            hideTimer = 150;
        }
        break;
    }

    case State::Hidden:
    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * drawLoadingScreen() — fill Y with black and draw a rotating ring
 * spinner (matching the app's ring.png loader).  The CbCr/UV plane is
 * cleared to 128 (neutral chroma) by mpgxRunOverlay() /
 * mpgxForceLoadingFrame() in the patched gx_supp.c when
 * g_wiifin_loading_active is set, so the screen is truly black.
 * ----------------------------------------------------------------------- */
static void drawLoadingScreen(uint8_t* Y, int W, int H)
{
    static int frame = 0;

    /* ITU-R BT.601 black */
    memset(Y, 16, (size_t)W * (size_t)H);

    const int cx = W / 2;
    const int cy = H / 2;
    const int outerR  = 50;
    const int innerR  = 38;
    const int outerR2 = outerR * outerR;
    const int innerR2 = innerR * innerR;
    const float PI2   = 6.2831853f;

    /* Head angle — full rotation every 90 frames (~1.5 s at 60 fps) */
    float headAngle = (float)(frame % 90) * (PI2 / 90.0f);

    for (int dy = -outerR; dy <= outerR; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= H) continue;
        for (int dx = -outerR; dx <= outerR; dx++) {
            int distSq = dx * dx + dy * dy;
            if (distSq < innerR2 || distSq > outerR2)
                continue;

            int px = cx + dx;
            if (px < 0 || px >= W) continue;

            /* Angle of this pixel (0..2π, screen-Y inverted) */
            float a = atan2f((float)-dy, (float)dx);
            if (a < 0.0f) a += PI2;

            /* Angular distance trailing behind the head */
            float trail = headAngle - a;
            if (trail < 0.0f) trail += PI2;

            /* Tail covers ~300° (5/6), gap covers ~60° (1/6) */
            const float tailLen = PI2 * (5.0f / 6.0f);
            uint8_t bri;
            if (trail <= tailLen) {
                bri = (uint8_t)(235 - trail * (205.0f / tailLen));
            } else {
                bri = 25;
            }

            setYpix(Y, W, px, py, bri);
        }
    }

    /* "Loading..." label centred below the ring, scale 2 */
    const char* label = "Loading...";
    int tw = textWidthScaled(label, 2);
    int tx = cx - tw / 2;
    int ty = cy + outerR + 20;
    if (tx >= 0 && ty >= 0 && tx + tw < W && ty + 14 < H)
        drawTextScaled(Y, W, tx, ty, label, 180, 2);

    frame++;
}

/* -----------------------------------------------------------------------
 * drawNativeControls() / drawHelpScreen() — WiiFin-owned fallback HUD for
 * sessions revealed via the GC pad (nativeMode).  vo_gx.c's own overlay
 * (ov_gx_draw) only ever reacts to real Wiimote/IR input, so it never
 * draws for these sessions; this reimplements a progress-bar + icon-row
 * control panel directly into the Y-texture with the pixel-font helpers
 * above, navigated with a focus cursor (Left/Right moves focus, A
 * activates) rather than the text-legend/direct-click hybrid from the
 * first attempt.
 * ----------------------------------------------------------------------- */
static void fmtTimeLocal(float secs, char* buf, int bufsz)
{
    int s  = (int)secs;
    int h  = s / 3600;
    int m  = (s % 3600) / 60;
    int ss = s % 60;
    if (h > 0)
        snprintf(buf, (size_t)bufsz, "%d:%02d:%02d", h, m, ss);
    else
        snprintf(buf, (size_t)bufsz, "%d:%02d", m, ss);
}

/* Draw text on a solid background box for contrast against live video --
 * setYpix has no alpha blending, so this is a flat fill rather than a
 * translucent scrim, but it's enough to keep white-on-bright-frame text
 * readable. Caller-supplied x/y/text/scale must already fit in bounds;
 * only the padding here is clamped. */
static void drawTextBoxScaled(uint8_t* Y, int W, int H, int x, int y,
                                const char* text, uint8_t textVal, uint8_t bgVal,
                                int scale, int pad)
{
    int tw = textWidthScaled(text, scale);
    int th = 7 * scale;
    int bx0 = x - pad, bx1 = x + tw + pad - 1;
    int by0 = y - pad, by1 = y + th + pad - 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 >= W) bx1 = W - 1;
    if (by1 >= H) by1 = H - 1;
    for (int yy = by0; yy <= by1; yy++)
        for (int xx = bx0; xx <= bx1; xx++)
            setYpix(Y, W, xx, yy, bgVal);
    drawTextScaled(Y, W, x, y, text, textVal, scale);
}

static void drawNativeControls(uint8_t* Y, int W, int H, int focusIdx,
                                 bool barFocused, float scrubPos)
{
    int   paused = g_mplayer_paused;
    /* g_mplayer_time_pos resets to 0 at the start of every transcode session
     * (resume, track switch, scrub restart) -- add the session's absolute
     * start so a resumed/scrubbed session shows the real position, not time
     * elapsed since this particular session began. */
    float pos    = g_wiifin_session_start_secs + g_mplayer_time_pos;
    float dur    = g_mplayer_duration;

    /* CRT-safe layout, independent of vo_gx.c's CTRL_Y0/Y1 constants (those
     * are tuned for vo_gx's own drawing, not ours): a real CRT commonly
     * overscans/crops several percent off every edge. Stack rows bottom-up
     * from a safe margin instead of an absolute Y. */
    const int marginX   = W / 14;
    const int marginBot = H / 10;
    const int scale     = 2;
    const int textPad   = 5;
    const int timeRowH  = 7 * scale + 2 * textPad;
    const int rowGap    = 14;
    const int barH      = 10;

    /* Button width/gap are derived from available width, not fixed pixel
     * constants -- W is no longer always 848 now that Settings can request
     * a reduced transcode resolution (see JellyfinClient::videoResMode). A
     * fixed btnW sized for 848 went negative at 480 (6*82 + 5*14 = 632 >
     * 480 - 2*marginX), wrapping the row around the texture. */
    const int nBtn      = 6; /* Prev, -10s, Play/Pause, +10s, Next, Help(?) */
    const int btnH      = 56;
    const int availW    = W - 2 * marginX;
    const int btnGap    = availW / 60;
    const int btnW      = (availW - (nBtn - 1) * btnGap) / nBtn;
    const int totalBtnW = nBtn * btnW + (nBtn - 1) * btnGap;
    const int btnX0     = (W - totalBtnW) / 2;

    int cursorY = H - marginBot;
    cursorY -= timeRowH;
    int timeTextY = cursorY + textPad;
    cursorY -= rowGap;
    cursorY -= barH;
    int barY0 = cursorY, barY1 = barY0 + barH - 1;
    cursorY -= rowGap;
    cursorY -= btnH;
    int btnY0 = cursorY, btnY1 = btnY0 + btnH - 1;

    /* Progress bar -- a couple pixels taller and brighter-bordered while
     * scrubbing so it's obvious focus has moved off the icon row. */
    const int barX0 = marginX, barX1 = W - marginX;
    int barTop = barFocused ? barY0 - 3 : barY0;
    int barBot = barFocused ? barY1 + 3 : barY1;
    if (barFocused) {
        for (int x = barX0 - 2; x <= barX1 + 2; x++) {
            setYpix(Y, W, x, barTop - 1, 220);
            setYpix(Y, W, x, barBot + 1, 220);
        }
    }
    for (int y = barTop; y <= barBot; y++)
        for (int x = barX0; x <= barX1; x++)
            setYpix(Y, W, x, y, 80);
    if (dur > 0.5f) {
        float frac = pos / dur;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        int fillX = barX0 + (int)(frac * (barX1 - barX0));
        for (int y = barTop; y <= barBot; y++)
            for (int x = barX0; x <= fillX; x++)
                setYpix(Y, W, x, y, 235);
    }
    /* Scrub target marker -- a bright tick distinct from the fill above,
     * so the pending (not-yet-committed) position is unambiguous. */
    if (barFocused && dur > 0.5f) {
        float sfrac = scrubPos / dur;
        if (sfrac < 0.0f) sfrac = 0.0f;
        if (sfrac > 1.0f) sfrac = 1.0f;
        int markX = barX0 + (int)(sfrac * (barX1 - barX0));
        for (int y = barTop - 5; y <= barBot + 5; y++)
            for (int dx = -2; dx <= 2; dx++) {
                int x = markX + dx;
                if (x >= 0 && x < W) setYpix(Y, W, x, y, 250);
            }
    }

    /* Time text, on a background box for contrast against bright video --
     * shows the pending scrub target instead of the real position while
     * scrubbing, since that's what A is about to commit to. */
    char tPos[16], tDur[16], line[48];
    fmtTimeLocal(barFocused ? scrubPos : pos, tPos, sizeof(tPos));
    fmtTimeLocal(dur > 0.0f ? dur : 0.0f, tDur, sizeof(tDur));
    if (barFocused)
        snprintf(line, sizeof(line), "Seek to %s / %s", tPos, tDur);
    else
        snprintf(line, sizeof(line), "%s %s / %s", paused ? "||" : ">", tPos, tDur);
    int lineW = textWidthScaled(line, scale);
    drawTextBoxScaled(Y, W, H, (W - lineW) / 2, timeTextY, line, 235, 20, scale, textPad);

    /* Icon row -- Left/Right moves focus (see State::Main's nativeMode
     * branch), A activates the focused icon. Focused icon drawn inverted
     * (bright fill / dark text) so it's obvious which one A will trigger. */
    static const char* kLabel[nBtn] = { "PREV", "-10s", nullptr, "+10s", "NEXT", "?" };
    /* "PAUSE" is the widest label -- drop to 1x text if btnW is too small
     * to fit it at 2x (only realistic at the smallest Reduced resolution). */
    int btnScale = (textWidthScaled("PAUSE", 2) + 8 <= btnW) ? 2 : 1;
    for (int i = 0; i < nBtn; i++) {
        int bx0 = btnX0 + i * (btnW + btnGap);
        int bx1 = bx0 + btnW - 1;
        bool sel = !barFocused && (i == focusIdx); /* focus moved to the bar */
        uint8_t bg = sel ? 220 : 55;
        for (int y = btnY0; y <= btnY1; y++)
            for (int x = bx0; x <= bx1; x++)
                setYpix(Y, W, x, y, bg);
        const char* label = (i == 2) ? (paused ? "PLAY" : "PAUSE") : kLabel[i];
        int tw = textWidthScaled(label, btnScale);
        int tx = bx0 + (btnW - tw) / 2;
        int ty = btnY0 + (btnH - 7 * btnScale) / 2;
        uint8_t fg = sel ? 20 : 225;
        drawTextScaled(Y, W, tx, ty, label, fg, btnScale);
    }
}

/* Full-panel shortcut reference, opened via the "?" icon (focus index 5).
 * "Takes up most of the screen" per design: a large centred panel, one row
 * per control. */
static void drawHelpScreen(uint8_t* Y, int W, int H)
{
    struct Row { const char* btn; const char* action; };
    static const Row rows[] = {
        { "A",        "Confirm focused icon / Play-Pause" },
        { "B",        "Hide controls (again: exit)" },
        { "Dpad L/R", "Move focus between icons" },
        { "Dpad Down","Select the progress bar to scrub" },
        { "  L/R",    "Move pending seek target (hold: faster)" },
        { "  A/B",    "Confirm seek / cancel back to icons" },
        { "Dpad L/R", "Seek -10s/+10s (menu hidden)" },
        { "Dpad Up",  "Volume up" },
        { "L / R",    "Previous / Next episode" },
        { "X",        "Audio track" },
        { "Y",        "Subtitles" },
        { "HOME",     "Stop, return to library" },
    };
    const int nRows = (int)(sizeof(rows) / sizeof(rows[0]));

    /* Text scale and column positions both derive from W, not fixed pixel
     * constants -- W is no longer always 848 now that Settings can request
     * a reduced transcode resolution (see JellyfinClient::videoResMode).
     * Fixed columns sized for an 848-wide panel ran the longer action
     * strings well past a narrower texture's right edge, wrapping around
     * to the next row instead of being clipped. Only two W values are ever
     * realistic (848 or 480), so a simple threshold is enough here rather
     * than per-string fitting like drawNativeControls does for its buttons. */
    const int scale    = (W >= 700) ? 2 : 1;
    const int titleScale = scale + 1;
    const int rowH     = 7 * scale + 14; /* tightened from +18 to fit the scrub-bar rows added below */
    const int titleH   = 7 * titleScale;

    int panelW = (int)(W * 0.82f);
    int panelH = titleH + 24 + nRows * rowH + rowH;
    int maxH   = (int)(H * 0.92f);
    if (panelH > maxH) panelH = maxH;
    int panelX0 = (W - panelW) / 2;
    int panelY0 = (H - panelH) / 2;
    int panelX1 = panelX0 + panelW - 1;
    int panelY1 = panelY0 + panelH - 1;

    for (int y = panelY0; y <= panelY1; y++)
        for (int x = panelX0; x <= panelX1; x++)
            setYpix(Y, W, x, y, 18);

    const char* title = "Controls";
    int tw = textWidthScaled(title, titleScale);
    drawTextScaled(Y, W, panelX0 + (panelW - tw) / 2, panelY0 + 16, title, 235, titleScale);

    int rowY    = panelY0 + 16 + titleH + 24;
    int btnColX = panelX0 + (int)(panelW * 0.045f);
    int actColX = panelX0 + (int)(panelW * 0.28f);
    for (int i = 0; i < nRows && rowY + rowH < panelY1; i++) {
        drawTextScaled(Y, W, btnColX, rowY, rows[i].btn,    255, scale);
        drawTextScaled(Y, W, actColX, rowY, rows[i].action, 205, scale);
        rowY += rowH;
    }

    const char* footer = "B / A: Close";
    int fw = textWidthScaled(footer, scale);
    drawTextScaled(Y, W, panelX0 + (panelW - fw) / 2, panelY1 - rowH + 8, footer, 220, scale);
}

/* Transient top-right volume gauge (Android-style), drawn whenever
 * volIndicatorTimer > 0 -- see the member comment in PlayerOverlay.h for
 * why this is a relative WiiFin-tracked level rather than a true % of
 * MPlayer's actual mixer volume. Independent of nativeMode/state so it
 * shows for every session (including the Hidden-state power-user
 * shortcuts and real-Wiimote sessions, where nothing else provides any
 * volume feedback at all). */
/* Transient top-left bitrate/network toast, shown once at playback start
 * (see PlayerOverlay::showBitrateIndicator()) -- top-left rather than
 * bottom-right so it doesn't collide with the bottom control row/progress
 * bar, and top-left rather than top-right so it doesn't collide with the
 * volume gauge either. Margins mirror drawVolumeIndicator's (H/12, W/14) --
 * comfortably clear of CRT overscan, not hugging the raw edge. */
static void drawBitrateIndicator(uint8_t* Y, int W, int H, const char* text)
{
    const int scale = 2;
    const int pad   = 8;
    const int marginTop  = H / 12;
    const int marginLeft = W / 14;

    int tw = textWidthScaled(text, scale);
    int panelW = tw + pad * 2;
    int panelH = 7 * scale + pad * 2;

    int px0 = marginLeft;
    int px1 = px0 + panelW;
    int py0 = marginTop;
    int py1 = py0 + panelH;

    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++)
            setYpix(Y, W, x, y, 20);

    drawTextScaled(Y, W, px0 + pad, py0 + pad, text, 230, scale);
}

static void drawVolumeIndicator(uint8_t* Y, int W, int H, int level, int levelMax)
{
    const int scale   = 2;
    const int panelW  = 46;
    const int panelH  = 160;
    const int marginTop   = H / 12;
    const int marginRight = W / 14;

    int px1 = W - marginRight;
    int px0 = px1 - panelW;
    int py0 = marginTop;
    int py1 = py0 + panelH;

    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++)
            setYpix(Y, W, x, y, 20);

    const char* label = "VOL";
    int tw = textWidthScaled(label, scale);
    drawTextScaled(Y, W, px0 + (panelW - tw) / 2, py0 + 8, label, 230, scale);

    int barX0 = px0 + panelW / 2 - 8, barX1 = barX0 + 16;
    int barY0 = py0 + 8 + 7 * scale + 10;
    int barY1 = py1 - 10;
    for (int y = barY0; y <= barY1; y++)
        for (int x = barX0; x <= barX1; x++)
            setYpix(Y, W, x, y, 60);

    if (levelMax > 0) {
        int fillH = (int)((float)(barY1 - barY0) * level / levelMax);
        int fillY0 = barY1 - fillH;
        if (fillY0 < barY0) fillY0 = barY0;
        for (int y = fillY0; y <= barY1; y++)
            for (int x = barX0; x <= barX1; x++)
                setYpix(Y, W, x, y, 230);
    }
}

/* Native audio/subtitle track picker, drawn instead of relying on
 * MPlayer's generic OSD text (activateAudioPick/activateSubPick's showOSD
 * calls) -- that text has no background and was easy to miss/misread over
 * bright video, matching the same contrast problem the time readout had. */
static void drawTrackPicker(uint8_t* Y, int W, int H, const char* title,
                              const char* const* labels, int nLabels, int selIdx)
{
    /* Scale and column offsets derive from W, not fixed pixel constants --
     * see the matching comment in drawHelpScreen for why. Track names are
     * also arbitrary-length (server-supplied), unlike this file's other
     * fixed label sets, so they're truncated to the row's available width
     * regardless of W rather than just relying on a smaller scale. */
    const int scale  = (W >= 700) ? 2 : 1;
    const int rowH   = 7 * scale + 16;
    const int titleScale = scale + 1;
    const int titleH = 7 * titleScale;

    int panelW = (int)(W * 0.7f);
    int panelH = titleH + 24 + (nLabels > 0 ? nLabels : 1) * rowH + rowH;
    int maxH   = (int)(H * 0.75f);
    if (panelH > maxH) panelH = maxH;
    int panelX0 = (W - panelW) / 2;
    int panelY0 = (H - panelH) / 2;
    int panelX1 = panelX0 + panelW - 1;
    int panelY1 = panelY0 + panelH - 1;

    for (int y = panelY0; y <= panelY1; y++)
        for (int x = panelX0; x <= panelX1; x++)
            setYpix(Y, W, x, y, 18);

    int tw = textWidthScaled(title, titleScale);
    drawTextScaled(Y, W, panelX0 + (panelW - tw) / 2, panelY0 + 14, title, 235, titleScale);

    int labelX0    = panelX0 + (int)(panelW * 0.07f);
    int labelMaxX1 = panelX1 - (int)(panelW * 0.03f);
    int maxChars   = (labelMaxX1 - labelX0) / (6 * scale);
    if (maxChars < 1) maxChars = 1;

    int rowY = panelY0 + 14 + titleH + 20;
    for (int i = 0; i < nLabels && rowY + rowH < panelY1; i++) {
        bool sel = (i == selIdx);
        if (sel) {
            int hlY0 = rowY - 4, hlY1 = rowY + 7 * scale + 4;
            for (int y = hlY0; y <= hlY1; y++)
                for (int x = panelX0 + 16; x <= panelX1 - 16; x++)
                    setYpix(Y, W, x, y, 55);
        }
        char buf[64];
        int n = (int)strlen(labels[i]);
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
        if (n > maxChars) n = maxChars;
        memcpy(buf, labels[i], (size_t)n);
        buf[n] = '\0';
        drawTextScaled(Y, W, panelX0 + (int)(panelW * 0.03f), rowY, sel ? ">" : " ", sel ? 235 : 190, scale);
        drawTextScaled(Y, W, labelX0, rowY, buf, sel ? 235 : 190, scale);
        rowY += rowH;
    }

    const char* footer = "A: Select   B: Cancel";
    int fw = textWidthScaled(footer, scale);
    drawTextScaled(Y, W, panelX0 + (panelW - fw) / 2, panelY1 - rowH + 8, footer, 210, scale);
}

/* -----------------------------------------------------------------------
 * onFrame() — static, called from MPlayer main thread each rendered frame.
 * Must not call GRRLIB or change the GX state.
 * ----------------------------------------------------------------------- */
void PlayerOverlay::onFrame(uint8_t* Ytex, int Ywidth, int Yheight)
{
    PlayerOverlay* self = instance;
    if (!self) return;

    static int onFrameCount = 0;
    if (onFrameCount < 5)
        SYS_Report("[PlayerOverlay] onFrame #%d loading=%d %dx%d\n",
                   ++onFrameCount, (int)g_wiifin_loading_active, Ywidth, Yheight);

    /* Stop requested: black out the Y texture immediately so neither video
     * nor the ring spinner appears during the async exit sequence.       */
    if (g_wiifin_stopping) {
        memset(Ytex, 16, (size_t)Ywidth * (size_t)Yheight); /* BT.601 black luma */
        return;
    }

    /* While MPlayer is still buffering / seeking (loading_active == 1),
     * replace the raw frame with a dark loading screen + spinner so the
     * user never sees purple / garbage video data.                      */
    if (g_wiifin_loading_active) {
        drawLoadingScreen(Ytex, Ywidth, Yheight);
        return;
    }

    /* Volume HUD draws regardless of state/nativeMode -- it's the only
     * feedback for the Hidden-state GC power-user volume shortcuts, and
     * vo_gx.c doesn't show one for real-Wiimote sessions either. */
    if (self->volIndicatorTimer > 0)
        drawVolumeIndicator(Ytex, Ywidth, Yheight, self->volLevel, self->volLevelMax);

    /* Bitrate toast: same "regardless of state/nativeMode" treatment,
     * wall-clock timed so the ~2s is accurate regardless of actual tick
     * rate. Countdown starts here, on the first real frame reaching this
     * point (past the loading-screen early-return above), not back when
     * showBitrateIndicator() was called before playback even started --
     * otherwise buffering time eats into the 2s before anything's visible
     * to show it against. */
    if (self->bitrateIndicatorPending) {
        self->bitrateIndicatorPending = false;
        self->bitrateIndicatorExpireTicks = gettime() + secs_to_ticks(2);
    }
    if (gettime() < self->bitrateIndicatorExpireTicks)
        drawBitrateIndicator(Ytex, Ywidth, Yheight, self->bitrateIndicatorText);

    if (self->state == State::Hidden) return;

    if (self->nativeMode) {
        /* GC-pad session: vo_gx.c's own overlay never sees GC input and
         * so never draws for us -- own the drawing ourselves. */
        if (self->state == State::Main) {
            drawNativeControls(Ytex, Ywidth, Yheight, self->mainFocus,
                                self->barFocused, self->scrubPos);
        } else if (self->state == State::Help) {
            drawHelpScreen(Ytex, Ywidth, Yheight);
        } else if (self->state == State::AudioPick) {
            std::vector<std::string> labelStrs;
            for (size_t i = 0; i < self->ctx.audioStreams.size(); i++) {
                char buf[80];
                snprintf(buf, sizeof(buf), "[%d] %s", (int)i + 1,
                         self->ctx.audioStreams[i].displayTitle.c_str());
                labelStrs.push_back(buf);
            }
            std::vector<const char*> labels;
            for (auto& s : labelStrs) labels.push_back(s.c_str());
            drawTrackPicker(Ytex, Ywidth, Yheight, "Audio Track",
                             labels.data(), (int)labels.size(), self->audioPickSel);
        } else if (self->state == State::SubPick) {
            std::vector<std::string> labelStrs;
            labelStrs.push_back("Off");
            for (size_t i = 0; i < self->ctx.subStreams.size(); i++) {
                const MediaStream& s = self->ctx.subStreams[i];
                std::string label;
                if (!s.displayTitle.empty())            label = s.displayTitle;
                else if (!s.language.empty() && !s.codec.empty())
                    label = s.language + " (" + s.codec + ")";
                else if (!s.language.empty())           label = s.language;
                else if (!s.codec.empty())               label = s.codec;
                else { char b[16]; snprintf(b, sizeof(b), "Track %d", (int)i + 1); label = b; }
                char buf[96];
                snprintf(buf, sizeof(buf), "[%d] %s", (int)i + 1, label.c_str());
                labelStrs.push_back(buf);
            }
            std::vector<const char*> labels;
            for (auto& s : labelStrs) labels.push_back(s.c_str());
            drawTrackPicker(Ytex, Ywidth, Yheight, "Subtitles",
                             labels.data(), (int)labels.size(), self->subPickSel);
        }
        return;
    }

    /* Wiimote/IR session: all controls and the progress bar are drawn by
     * the GX overlay (ov_gx_draw in vo_gx.c).  Nothing to draw here. */
}
