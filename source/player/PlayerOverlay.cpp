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

    /* Show overlay automatically for the first ~5 s so the user sees it. */
    activateMain();
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
    state     = State::Main;
    hideTimer = 150; /* 5 s at ~30 Hz */
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
    state     = State::Hidden;
    hideTimer = 0;
    /* Clear the OSD text */
    showOSD(" ", 1);
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
                         float irX, float irY, bool irValid)
{
    (void)btnsHeld; /* reserved for hold-to-seek in the future */

    /* If the GX overlay (vo_gx.c) already handled buttons this frame,
     * drop them from btnsDown so we don't double-fire actions.
     * g_wiifin_gx_btn_consumed is a WPAD bitmask of consumed buttons. */
    if (g_wiifin_gx_btn_consumed) {
        btnsDown &= ~(uint32_t)g_wiifin_gx_btn_consumed;
        g_wiifin_gx_btn_consumed = 0;
    }

    /* ---- IR pointer: reveal overlay on movement, track hover ---- */
    if (irValid) {
        bool moved = (lastIrX >= 0.0f) &&
                     ((irX - lastIrX) * (irX - lastIrX) +
                      (irY - lastIrY) * (irY - lastIrY)) > 16.0f; /* >4 px */
        if (moved) {
            if (state == State::Hidden) {
                activateMain();
                refreshTimeOSD(timePos, paused);
            } else if (state == State::Main) {
                hideTimer = 150; /* reset auto-hide on pointer movement */
            }
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

    /* Any button reveals the overlay */
    if (btnsDown && state == State::Hidden) {
        activateMain();
        refreshTimeOSD(timePos, paused);
        return; /* consume the press as "show" */
    }

    /* HOME: stop player; runPlaySession will show the clean GRRLIB HOME overlay. */
    if (btnsDown & WPAD_BUTTON_HOME) {
        g_wiifin_stopping    = 1;
        g_player_stop_reason = PLAYER_STOP_HOME;
        wii_player_stop();
        return;
    }

    /* AUTO-HIDE timer — disabled while video is paused
     * (keep the overlay visible so the user can click play to resume). */
    if (state != State::Hidden) {
        if (g_mplayer_paused) {
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
             * by the video-mode check. */
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
            g_wiifin_stopping     = 1;
            g_player_stop_reason  = PLAYER_STOP_EOF;
            wii_player_stop();
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
        if (btnsDown & WPAD_BUTTON_UP)   { wii_player_vol_up();   }
        if (btnsDown & WPAD_BUTTON_DOWN) { wii_player_vol_down(); }

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

    if (self->state == State::Hidden) return;

    /* All controls and the progress bar are drawn by the GX overlay
     * (ov_gx_draw in vo_gx.c).  Nothing to draw here for Main state. */
}
