#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include "../jellyfin/JellyfinClient.h"

class SettingsView {
public:
    SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* font, JellyfinClient& client, bool& musicEnabled);

    // Returns true when the user exits (B or back)
    bool update(ir_t& ir);
    void render(ir_t& ir);

private:
    GRRLIB_texImg*  btnTex;
    GRRLIB_ttfFont* font;
    JellyfinClient& client;
    bool&           musicEnabled;

    int  selectedIndex = 0;
    bool irMode        = false;

    void drawGradientBG();
    void drawToggleRow(int x, int y, int w, const char* label,
                       const char* desc, bool value, bool focused);
};
