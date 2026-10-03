#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include "../jellyfin/JellyfinClient.h"

class SettingsView {
public:
    SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* font, JellyfinClient& client,
                bool& musicEnabled, bool& debugLogFileEnabled,
                bool& bgImageLoadingEnabled, int& audioDelayMs);

    // Returns true when the user exits (B or back)
    bool update(ir_t& ir);
    void render(ir_t& ir);

private:
    GRRLIB_texImg*  btnTex;
    GRRLIB_ttfFont* font;
    JellyfinClient& client;
    bool&           musicEnabled;
    bool&           debugLogFileEnabled;
    bool&           bgImageLoadingEnabled;
    int&            audioDelayMs; // playback start A/V delay, see g_wiifin_audio_delay_default_ms

    int  selectedIndex = 0;
    bool irMode        = false;
    int  scrollTop     = 0; // first visible row -- the list scrolls, see VISIBLE_ROWS

    // -1 = cache hasn't been cleared this time Settings was opened;
    // otherwise the number of files removed by the last clear.
    int cacheClearedCount = -1;

    // Full-screen log viewer, entered via the "View Debug Log" row.
    bool viewingLog  = false;
    int  logScrollTop = 0; // index into DebugLog::History() of the top visible line

    void drawGradientBG();
    void drawToggleRow(int x, int y, int w, const char* label,
                       const char* desc, bool value, bool focused);
    void drawActionRow(int x, int y, int w, const char* label,
                       const char* desc, bool focused, bool done);
    // Like drawToggleRow, but the pill shows an arbitrary current-value
    // string instead of ON/OFF -- A cycles through a fixed set of options.
    void drawCycleRow(int x, int y, int w, const char* label,
                      const char* desc, const char* valueText, bool focused);
    void updateLogViewer();
    void renderLogViewer();
};
