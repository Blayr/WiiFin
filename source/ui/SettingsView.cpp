#include "SettingsView.h"
#include "../input/Input.h"
#include "../core/DebugLog.h"
#include "../version.h"
#include "../player/WiiPlayer.h" // AUDIO_DELAY_*_MS

static const int NUM_SETTINGS = 10; // extensible
static const int ROW_Y0 = 58;
static const int ROW_H  = 46; // actual drawn box height -- see draw*Row()
static const int ROW_DY = 48; // row-to-row spacing (includes the gap)
// Rows that fit between the title rule and the credits block (y=400); the
// list scrolls to keep the focused row in view once there are more.
static const int VISIBLE_ROWS = 7;

SettingsView::SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* f, JellyfinClient& c,
                           bool& music, bool& debugLogFile, bool& bgImageLoading,
                           int& audioDelay, int& autoplay)
    : btnTex(btn), font(f), client(c), musicEnabled(music), debugLogFileEnabled(debugLogFile),
      bgImageLoadingEnabled(bgImageLoading), audioDelayMs(audioDelay),
      autoplayLimit(autoplay) {}

// Screen Y of setting row i given the current scroll position. Only
// meaningful for visible rows (see isRowVisible).
#define rowY(i) (ROW_Y0 + ((i) - scrollTop) * ROW_DY)
#define isRowVisible(i) ((i) >= scrollTop && (i) < scrollTop + VISIBLE_ROWS)

// ---------------------------------------------------------------
void SettingsView::drawGradientBG() {
    const int r1=0x1a, g1=0x1a, b1=0x2e;
    const int r2=0x16, g2=0x21, b2=0x3e;
    const int bands=16, bh=480/bands;
    for (int i=0; i<bands; i++) {
        float t = i/(float)(bands-1);
        u32 col = (((int)(r1+(r2-r1)*t))<<24)|(((int)(g1+(g2-g1)*t))<<16)
                |(((int)(b1+(b2-b1)*t))<<8)|0xFF;
        GRRLIB_Rectangle(0, i*bh, 640, bh, col, 1);
    }
}

void SettingsView::drawToggleRow(int x, int y, int w,
                                  const char* label, const char* desc,
                                  bool value, bool focused) {
    // Background
    u32 bg = focused ? 0x1E3A5FCC : 0x111827AA;
    GRRLIB_Rectangle(x, y, w, ROW_H, bg, 1);
    u32 border = focused ? 0x4499FFFF : 0x2A3A4AFF;
    GRRLIB_Rectangle(x, y, w, 2,  border, 1);
    GRRLIB_Rectangle(x, y+ROW_H-2, w, 2, border, 1);

    // Label + description
    GRRLIB_PrintfTTF(x+14, y+5,  font, label, 16, 0xEEEEEEFF);
    GRRLIB_PrintfTTF(x+14, y+25, font, desc,  12, 0x778899FF);

    // Toggle pill on the right
    const char* valStr = value ? "ON" : "OFF";
    u32 pillCol = value ? 0x44BB66FF : 0x555566FF;
    int pw = GRRLIB_WidthTTF(font, valStr, 16) + 20;
    int px = x + w - pw - 14;
    int py = y + (ROW_H - 22) / 2;
    GRRLIB_Rectangle(px, py, pw, 22, pillCol, 1);
    GRRLIB_PrintfTTF(px + 10, py + 3, font, valStr, 16, 0xFFFFFFFF);
}

// A one-shot action row (as opposed to an ON/OFF toggle) -- e.g. "Clear
// Image Cache". Pressing A fires the action immediately; there's no
// persistent value to show, just a "GO" pill and a description that the
// caller can update afterward to reflect the result. Once fired (`done`),
// the row stops reacting to focus (no highlight box) and the pill greys out
// and reads "DONE" so it reads as inert/unclickable -- matches update()'s
// case 2, which stops re-firing the action once cacheClearedCount is set.
// Nothing re-arms it until Settings is reopened (cacheClearedCount resets).
void SettingsView::drawActionRow(int x, int y, int w,
                                  const char* label, const char* desc,
                                  bool focused, bool done) {
    bool highlight = focused && !done;
    u32 bg = highlight ? 0x1E3A5FCC : 0x111827AA;
    GRRLIB_Rectangle(x, y, w, ROW_H, bg, 1);
    u32 border = highlight ? 0x4499FFFF : 0x2A3A4AFF;
    GRRLIB_Rectangle(x, y, w, 2,  border, 1);
    GRRLIB_Rectangle(x, y+ROW_H-2, w, 2, border, 1);

    u32 labelCol = done ? 0x99A3AEFF : 0xEEEEEEFF;
    GRRLIB_PrintfTTF(x+14, y+5,  font, label, 16, labelCol);
    GRRLIB_PrintfTTF(x+14, y+25, font, desc,  12, 0x778899FF);

    const char* actStr = done ? "DONE" : "GO";
    u32 pillCol = done ? 0x3A3F4AFF : 0x3388CCFF;
    int pw = GRRLIB_WidthTTF(font, actStr, 16) + 20;
    int px = x + w - pw - 14;
    int py = y + (ROW_H - 22) / 2;
    GRRLIB_Rectangle(px, py, pw, 22, pillCol, 1);
    GRRLIB_PrintfTTF(px + 10, py + 3, font, actStr, 16, done ? 0x8891A0FF : 0xFFFFFFFF);
}

// Like drawToggleRow, but the pill shows an arbitrary value string instead
// of ON/OFF -- for a setting with more than two options (A cycles through
// them at the caller's fireAction()).
void SettingsView::drawCycleRow(int x, int y, int w,
                                 const char* label, const char* desc,
                                 const char* valueText, bool focused) {
    u32 bg = focused ? 0x1E3A5FCC : 0x111827AA;
    GRRLIB_Rectangle(x, y, w, ROW_H, bg, 1);
    u32 border = focused ? 0x4499FFFF : 0x2A3A4AFF;
    GRRLIB_Rectangle(x, y, w, 2,  border, 1);
    GRRLIB_Rectangle(x, y+ROW_H-2, w, 2, border, 1);

    GRRLIB_PrintfTTF(x+14, y+5,  font, label, 16, 0xEEEEEEFF);
    GRRLIB_PrintfTTF(x+14, y+25, font, desc,  12, 0x778899FF);

    u32 pillCol = 0x3A5F8CFF;
    int pw = GRRLIB_WidthTTF(font, valueText, 15) + 20;
    int px = x + w - pw - 14;
    int py = y + (ROW_H - 22) / 2;
    GRRLIB_Rectangle(px, py, pw, 22, pillCol, 1);
    GRRLIB_PrintfTTF(px + 10, py + 3, font, valueText, 15, 0xFFFFFFFF);
}

// ---------------------------------------------------------------
bool SettingsView::update(ir_t& ir) {
    if (viewingLog) { updateLogViewer(); return false; }

    bool aPressed = Input::isAJustPressed();

    if (ir.valid) irMode = true;

    if (Input::isBackPressed()) return true;

    if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + NUM_SETTINGS) % NUM_SETTINGS; irMode = false; }
    if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % NUM_SETTINGS; irMode = false; }
    // Keep the focused row on screen (also handles wrap-around).
    if (selectedIndex < scrollTop) scrollTop = selectedIndex;
    if (selectedIndex >= scrollTop + VISIBLE_ROWS) scrollTop = selectedIndex - VISIBLE_ROWS + 1;

    // IR hover
    if (ir.valid) {
        for (int i = 0; i < NUM_SETTINGS; i++) {
            if (!isRowVisible(i)) continue;
            int ry = rowY(i);
            if (ir.x >= 60 && ir.x <= 580 && ir.y >= ry && ir.y <= ry + ROW_H) {
                selectedIndex = i;
                irMode = true;
            }
        }
    }

    auto fireAction = [&](int i) {
        switch (i) {
            case 0: client.sslVerify   = !client.sslVerify;   break;
            case 1: {
                using BM = JellyfinClient::BitrateMode;
                client.bitrateMode = (BM)(((int)client.bitrateMode + 1) % 4);
                break;
            }
            case 2: {
                using VRM = JellyfinClient::VideoResMode;
                client.videoResMode = (VRM)(((int)client.videoResMode + 1) % 3);
                break;
            }
            // A steps up, wrapping to the minimum; Left/Right step either way.
            case 3:
                audioDelayMs += AUDIO_DELAY_STEP_MS;
                if (audioDelayMs > AUDIO_DELAY_MAX_MS) audioDelayMs = AUDIO_DELAY_MIN_MS;
                break;
            case 4: autoplayLimit = (autoplayLimit + 1) % (AUTOPLAY_NO_LIMIT + 1); break;
            case 5: musicEnabled       = !musicEnabled;        break;
            // Read by LibraryView's constructor, so it applies the next time
            // a profile's library is opened, not to the one already open.
            case 6: bgImageLoadingEnabled = !bgImageLoadingEnabled; break;
            // One-shot: only fires while cacheClearedCount is still -1 (unset
            // this time Settings was opened). Re-pressing afterward is a no-op
            // rather than clearing an already-empty cache and overwriting the
            // "Cleared N" message with "Cleared 0".
            case 7: if (cacheClearedCount < 0) cacheClearedCount = client.clearImageCache(); break;
            case 8:
                debugLogFileEnabled = !debugLogFileEnabled;
                DebugLog::SetFileLoggingEnabled(debugLogFileEnabled);
                break;
            case 9:
                viewingLog  = true;
                logScrollTop = 0;
                break;
        }
    };

    if (selectedIndex == 3) {
        int step = Input::isRightPressed() ? AUDIO_DELAY_STEP_MS
                 : Input::isLeftPressed()  ? -AUDIO_DELAY_STEP_MS : 0;
        if (step) {
            audioDelayMs += step;
            if (audioDelayMs < AUDIO_DELAY_MIN_MS) audioDelayMs = AUDIO_DELAY_MIN_MS;
            if (audioDelayMs > AUDIO_DELAY_MAX_MS) audioDelayMs = AUDIO_DELAY_MAX_MS;
        }
    }
    if (selectedIndex == 4) {
        if (Input::isRightPressed() && autoplayLimit < AUTOPLAY_NO_LIMIT) autoplayLimit++;
        if (Input::isLeftPressed()  && autoplayLimit > 0)                 autoplayLimit--;
    }

    if (aPressed && !ir.valid && !irMode) fireAction(selectedIndex);
    // IR click
    if (ir.valid && aPressed) {
        for (int i = 0; i < NUM_SETTINGS; i++) {
            if (!isRowVisible(i)) continue;
            int ry = rowY(i);
            if (ir.x >= 60 && ir.x <= 580 && ir.y >= ry && ir.y <= ry + ROW_H)
                fireAction(i);
        }
    }

    return false;
}

// ---------------------------------------------------------------
void SettingsView::updateLogViewer() {
    const int visibleLines = 20;
    int total = (int)DebugLog::History().size();

    if (Input::isBackPressed()) { viewingLog = false; return; }
    if (Input::isUpPressed())   { if (logScrollTop > 0) logScrollTop--; }
    if (Input::isDownPressed()) { if (logScrollTop < total - visibleLines) logScrollTop++; }
    if (logScrollTop > total - visibleLines) logScrollTop = total - visibleLines;
    if (logScrollTop < 0) logScrollTop = 0;
}

void SettingsView::render(ir_t& ir) {
    (void)ir;
    if (viewingLog) { renderLogViewer(); return; }

    drawGradientBG();

    // Title
    GRRLIB_PrintfTTF(0, 14, font, "Settings", 24, 0xFFFFFFFF);
    GRRLIB_Rectangle(60, 50, 520, 1, 0x4499FF88, 1);

    // Settings rows -- only those inside the scroll window are drawn.
    if (isRowVisible(0))
    drawToggleRow(60, rowY(0), 520,
        "SSL Verification",
        "Validate the server HTTPS certificate (disable for self-signed certs)",
        client.sslVerify, selectedIndex == 0);

    if (isRowVisible(1)) {
        using BM = JellyfinClient::BitrateMode;
        char bitrateDesc[96];
        const char* bitrateVal;
        switch (client.bitrateMode) {
            case BM::Fast:
                bitrateVal = "FAST";
                snprintf(bitrateDesc, sizeof(bitrateDesc), "Always %.0f kbps", (double)client.NETWORK_BITRATE_CEILING_KBPS);
                break;
            case BM::Medium:
                bitrateVal = "MEDIUM";
                snprintf(bitrateDesc, sizeof(bitrateDesc), "Always %.0f kbps", (double)client.NETWORK_BITRATE_MEDIUM_KBPS);
                break;
            case BM::Slow:
                bitrateVal = "SLOW";
                snprintf(bitrateDesc, sizeof(bitrateDesc), "Always %.0f kbps", (double)client.NETWORK_BITRATE_FLOOR_KBPS);
                break;
            default:
                bitrateVal = "AUTO";
                if (client.networkKbps > 0.0f)
                    snprintf(bitrateDesc, sizeof(bitrateDesc), "Measured ~%.0f kbps -> %.0f kbps target",
                        (double)client.networkKbps, (double)client.effectiveBitrateKbps());
                else
                    snprintf(bitrateDesc, sizeof(bitrateDesc), "Not measured yet -- checked before next playback");
                break;
        }
        drawCycleRow(60, rowY(1), 520, "Bitrate", bitrateDesc, bitrateVal, selectedIndex == 1);
    }

    if (isRowVisible(2)) {
        using VRM = JellyfinClient::VideoResMode;
        const char* resDesc;
        const char* resVal;
        switch (client.videoResMode) {
            case VRM::Full:
                resVal  = "FULL";
                resDesc = "Always 848x480";
                break;
            case VRM::Reduced:
                resVal  = "REDUCED";
                resDesc = "Always 480x480 -- matches composite/CRT's real resolvable detail";
                break;
            default:
                resVal  = client.isProgressiveVideo ? "AUTO (Full)" : "AUTO (Reduced)";
                resDesc = "848x480 on component/HDMI adapter, 480x480 on composite/S-Video";
                break;
        }
        drawCycleRow(60, rowY(2), 520, "Video Resolution", resDesc, resVal, selectedIndex == 2);
    }

    if (isRowVisible(3)) {
        char delayVal[16];
        snprintf(delayVal, sizeof(delayVal), "%+d ms", audioDelayMs);
        if (audioDelayMs == 0) snprintf(delayVal, sizeof(delayVal), "0 ms");
        drawCycleRow(60, rowY(3), 520, "Audio Delay",
            "Left/Right: 50 ms steps. + delays audio. C-stick adjusts live in playback",
            delayVal, selectedIndex == 3);
    }

    if (isRowVisible(4)) {
        char apVal[16];
        if (autoplayLimit == 0)                      snprintf(apVal, sizeof(apVal), "OFF");
        else if (autoplayLimit == AUTOPLAY_NO_LIMIT) snprintf(apVal, sizeof(apVal), "NO LIMIT");
        else                                         snprintf(apVal, sizeof(apVal), "%d", autoplayLimit);
        drawCycleRow(60, rowY(4), 520, "Autoplay Next Episode",
            "Episodes to autoplay in a row (B cancels the countdown)",
            apVal, selectedIndex == 4);
    }

    if (isRowVisible(5))
    drawToggleRow(60, rowY(5), 520,
        "Background Music",
        "Play background music in menus",
        musicEnabled, selectedIndex == 5);

    if (isRowVisible(6))
    drawToggleRow(60, rowY(6), 520,
        "Background Image Loading",
        "Load posters in the background (applies next time a profile is opened)",
        bgImageLoadingEnabled, selectedIndex == 6);

    char cacheDesc[64];
    if (cacheClearedCount >= 0)
        snprintf(cacheDesc, sizeof(cacheDesc), "Cleared %d cached image(s)", cacheClearedCount);
    else
        snprintf(cacheDesc, sizeof(cacheDesc), "Frees SD card space by deleting cached posters/backdrops");
    if (isRowVisible(7))
    drawActionRow(60, rowY(7), 520,
        "Clear Image Cache", cacheDesc, selectedIndex == 7, cacheClearedCount >= 0);

    if (isRowVisible(8))
    drawToggleRow(60, rowY(8), 520,
        "Debug Log File",
        "Write the debug log to sd:/apps/WiiFin/wiifin_log.txt (off by default)",
        debugLogFileEnabled, selectedIndex == 8);

    if (isRowVisible(9))
    drawActionRow(60, rowY(9), 520,
        "View Debug Log", "Browse this session's log on-screen", selectedIndex == 9, false);

    // Scrollbar to the right of the rows, only when the list overflows.
    if (NUM_SETTINGS > VISIBLE_ROWS) {
        const int sbX = 588, sbY = ROW_Y0, sbH = VISIBLE_ROWS * ROW_DY - (ROW_DY - ROW_H);
        int thumbH = sbH * VISIBLE_ROWS / NUM_SETTINGS;
        int thumbY = sbY + (sbH - thumbH) * scrollTop / (NUM_SETTINGS - VISIBLE_ROWS);
        GRRLIB_Rectangle(sbX, sbY, 4, sbH, 0x2A3A4AFF, 1);
        GRRLIB_Rectangle(sbX, thumbY, 4, thumbH, 0x4499FFFF, 1);
    }

    // Credits block
    GRRLIB_Rectangle(60, 400, 520, 1, 0x4499FF44, 1);
    const char* version = "WiiFin v" WIIFIN_VERSION;
    const char* credits = "Made with \xe2\x9d\xa4 for Jellyfin  \xe2\x80\xa2  github.com/fabienmillet/WiiFin";
    int vw = GRRLIB_WidthTTF(font, version, 14);
    int cw = GRRLIB_WidthTTF(font, credits, 11);
    GRRLIB_PrintfTTF((640-vw)/2, 407, font, version, 14, 0xAADDFFFF);
    GRRLIB_PrintfTTF((640-cw)/2, 425, font, credits, 11, 0x556677FF);

    // Footer
    const char* footer = "A: Toggle/Select   B: Back";
    int fw = GRRLIB_WidthTTF(font, footer, 14);
    GRRLIB_PrintfTTF((640-fw)/2, 458, font, footer, 14, 0x778899FF);
}

// ---------------------------------------------------------------
void SettingsView::renderLogViewer() {
    drawGradientBG();

    GRRLIB_PrintfTTF(0, 14, font, "Debug Log", 24, 0xFFFFFFFF);
    GRRLIB_Rectangle(15, 50, 610, 1, 0x4499FF88, 1);

    const auto& history = DebugLog::History();
    const int visibleLines = 20;
    const int lineH = 18;
    const int listY0 = 62;

    if (history.empty()) {
        GRRLIB_PrintfTTF(15, listY0, font, "Nothing logged yet this session.", 14, 0x778899FF);
    } else {
        int shown = (int)history.size() - logScrollTop;
        if (shown > visibleLines) shown = visibleLines;
        for (int i = 0; i < shown; i++) {
            const std::string& line = history[logScrollTop + i];
            GRRLIB_PrintfTTF(15, listY0 + i * lineH, font, line.c_str(), 13, 0xCCEEFFEE);
        }
    }

    char scroll[32];
    snprintf(scroll, sizeof(scroll), "%d / %d", logScrollTop + 1, (int)history.size());
    int sw = GRRLIB_WidthTTF(font, scroll, 13);
    GRRLIB_PrintfTTF(610 - sw, 14, font, scroll, 13, 0x778899FF);

    const char* footer = DebugLog::IsFileLoggingEnabled()
        ? "Up/Down: Scroll   B: Back   (also writing to SD)"
        : "Up/Down: Scroll   B: Back   (enable the toggle above to save to SD)";
    int fw = GRRLIB_WidthTTF(font, footer, 13);
    GRRLIB_PrintfTTF((640-fw)/2, 458, font, footer, 13, 0x778899FF);
}
