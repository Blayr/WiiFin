#include "SettingsView.h"
#include "../input/Input.h"
#include "../version.h"

static const int NUM_SETTINGS = 2; // extensible

SettingsView::SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* f, JellyfinClient& c, bool& music)
    : btnTex(btn), font(f), client(c), musicEnabled(music) {}

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
    GRRLIB_Rectangle(x, y, w, 52, bg, 1);
    u32 border = focused ? 0x4499FFFF : 0x2A3A4AFF;
    GRRLIB_Rectangle(x, y, w, 2,  border, 1);
    GRRLIB_Rectangle(x, y+50, w, 2, border, 1);

    // Label + description
    GRRLIB_PrintfTTF(x+14, y+7,  font, label, 18, 0xEEEEEEFF);
    GRRLIB_PrintfTTF(x+14, y+29, font, desc,  13, 0x778899FF);

    // Toggle pill on the right
    const char* valStr = value ? "ON" : "OFF";
    u32 pillCol = value ? 0x44BB66FF : 0x555566FF;
    int pw = GRRLIB_WidthTTF(font, valStr, 16) + 20;
    int px = x + w - pw - 14;
    int py = y + 14;
    GRRLIB_Rectangle(px, py, pw, 24, pillCol, 1);
    GRRLIB_PrintfTTF(px + 10, py + 4, font, valStr, 16, 0xFFFFFFFF);
}

// ---------------------------------------------------------------
bool SettingsView::update(ir_t& ir) {
    bool aPressed = Input::isAJustPressed();

    if (ir.valid) irMode = true;

    if (Input::isBackPressed()) return true;

    if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + NUM_SETTINGS) % NUM_SETTINGS; irMode = false; }
    if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % NUM_SETTINGS; irMode = false; }

    // IR hover
    if (ir.valid) {
        for (int i = 0; i < NUM_SETTINGS; i++) {
            int ry = 120 + i * 66;
            if (ir.x >= 60 && ir.x <= 580 && ir.y >= ry && ir.y <= ry + 52) {
                selectedIndex = i;
                irMode = true;
            }
        }
    }

    if (aPressed && !ir.valid && !irMode) {
        switch (selectedIndex) {
            case 0: client.sslVerify  = !client.sslVerify;  break;
            case 1: musicEnabled      = !musicEnabled;       break;
        }
    }
    // IR click
    if (ir.valid && aPressed) {
        for (int i = 0; i < NUM_SETTINGS; i++) {
            int ry = 120 + i * 66;
            if (ir.x >= 60 && ir.x <= 580 && ir.y >= ry && ir.y <= ry + 52) {
                switch (i) {
                    case 0: client.sslVerify  = !client.sslVerify;  break;
                    case 1: musicEnabled      = !musicEnabled;       break;
                }
            }
        }
    }

    return false;
}

void SettingsView::render(ir_t& ir) {
    (void)ir;
    drawGradientBG();

    // Title
    GRRLIB_PrintfTTF(0, 14, font, "Settings", 24, 0xFFFFFFFF);
    GRRLIB_Rectangle(60, 50, 520, 1, 0x4499FF88, 1);

    // Settings rows
    drawToggleRow(60, 120, 520,
        "SSL Verification",
        "Validate the server HTTPS certificate (disable for self-signed certs)",
        client.sslVerify, selectedIndex == 0);

    drawToggleRow(60, 186, 520,
        "Background Music",
        "Play background music in menus",
        musicEnabled, selectedIndex == 1);

    // Credits block
    GRRLIB_Rectangle(60, 390, 520, 1, 0x4499FF44, 1);
    const char* version = "WiiFin v" WIIFIN_VERSION;
    const char* credits = "Made with \xe2\x9d\xa4 for Jellyfin  \xe2\x80\xa2  github.com/fabienmillet/WiiFin";
    int vw = GRRLIB_WidthTTF(font, version, 15);
    int cw = GRRLIB_WidthTTF(font, credits, 12);
    GRRLIB_PrintfTTF((640-vw)/2, 400, font, version, 15, 0xAADDFFFF);
    GRRLIB_PrintfTTF((640-cw)/2, 422, font, credits, 12, 0x556677FF);

    // Footer
    const char* footer = "A: Toggle   B: Back";
    int fw = GRRLIB_WidthTTF(font, footer, 14);
    GRRLIB_PrintfTTF((640-fw)/2, 458, font, footer, 14, 0x778899FF);
}
