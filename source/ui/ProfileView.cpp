#include "ProfileView.h"
#include "../input/Input.h"
#include <stdio.h>
#include <string.h>

ProfileView::ProfileView(GRRLIB_ttfFont* f, GRRLIB_texImg* cursor,
                         const std::vector<SavedProfile>& p)
    : font(f), cursorTex(cursor), profiles(p)
{
    focusedRow = 0;
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
void ProfileView::drawGradientBG() {
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

/* -----------------------------------------------------------------------
 * update()
 * ----------------------------------------------------------------------- */
ProfileResult ProfileView::update(ir_t& ir) {
    if (ir.valid) irMode = true;

    int totalRows = (int)profiles.size() + 1; /* profiles + "Add New" row */

    /* D-pad navigation */
    if (Input::isUpPressed()) {
        focusedRow = (focusedRow - 1 + totalRows) % totalRows;
        irMode = false;
        confirmDelete = false;
    }
    if (Input::isDownPressed()) {
        focusedRow = (focusedRow + 1) % totalRows;
        irMode = false;
        confirmDelete = false;
    }

    /* IR hover */
    bool irHoveredRow = false;
    if (ir.valid) {
        int vis = (totalRows < MAX_VIS) ? totalRows : MAX_VIS;
        for (int i = 0; i < vis; i++) {
            int ry = ROW_Y0 + i * ROW_H;
            if (ir.x >= ROW_X && ir.x <= ROW_X + ROW_W &&
                ir.y >= ry    && ir.y <= ry + ROW_H - 4) {
                if (focusedRow != i) confirmDelete = false;
                focusedRow = i;
                irMode = true;
                irHoveredRow = true;
            }
        }
    }

    /* B = back / cancel confirm */
    if (Input::isBackPressed()) {
        if (confirmDelete) { confirmDelete = false; return ProfileResult::None; }
        return ProfileResult::Back;
    }

    /* MINUS (isLPressed) = toggle delete confirm for a profile row */
    if (Input::isLPressed() && focusedRow < (int)profiles.size()) {
        confirmDelete = !confirmDelete;
        return ProfileResult::None;
    }

    /* A = confirm */
    if (Input::isAJustPressed() && (irHoveredRow || (!ir.valid && !irMode))) {
        if (confirmDelete) {
            confirmDelete = false;
            selectedIdx = focusedRow;
            return ProfileResult::DeleteOne;
        }
        if (focusedRow == (int)profiles.size()) {
            return ProfileResult::AddNew;
        }
        selectedIdx = focusedRow;
        return ProfileResult::Selected;
    }

    return ProfileResult::None;
}

/* -----------------------------------------------------------------------
 * render()
 * ----------------------------------------------------------------------- */
void ProfileView::render(ir_t& ir) {
    drawGradientBG();

    /* Title */
    const char* title = profiles.empty() ? "No Profiles — Add One" : "Select Profile";
    int tw = GRRLIB_WidthTTF(font, title, 26);
    GRRLIB_PrintfTTF((640 - tw) / 2, 14, font, title, 26, 0xFFFFFFFF);
    GRRLIB_Rectangle(ROW_X, 55, ROW_W, 1, 0x4499FF88, 1);

    int totalRows = (int)profiles.size() + 1;
    int vis = (totalRows < MAX_VIS) ? totalRows : MAX_VIS;

    for (int i = 0; i < vis; i++) {
        int ry  = ROW_Y0 + i * ROW_H;
        bool sel   = (i == focusedRow);
        bool isAdd = (i == (int)profiles.size());

        u32 bg     = sel ? 0x1E3A5FCC : 0x111827AA;
        u32 border = sel ? 0x4499FFFF : 0x2A3A4AFF;

        GRRLIB_Rectangle(ROW_X, ry,          ROW_W, ROW_H - 4, bg,     1);
        GRRLIB_Rectangle(ROW_X, ry,          ROW_W, 2,          border, 1);
        GRRLIB_Rectangle(ROW_X, ry+ROW_H-6,  ROW_W, 2,          border, 1);

        if (isAdd) {
            u32 col = sel ? 0x4499FFFF : 0x778899FF;
            GRRLIB_PrintfTTF(ROW_X + 14, ry + 18, font, "+ Add New Profile", 20, col);
        } else {
            const SavedProfile& p = profiles[i];

            /* Line 1: "Username @ Server Name" */
            char line1[96];
            if (!p.username.empty() && !p.serverName.empty())
                snprintf(line1, sizeof(line1), "%s @ %s", p.username.c_str(), p.serverName.c_str());
            else if (!p.serverName.empty())
                snprintf(line1, sizeof(line1), "%s", p.serverName.c_str());
            else
                snprintf(line1, sizeof(line1), "%s", p.serverUrl.c_str());

            /* Line 2: server URL */
            char line2[80];
            snprintf(line2, sizeof(line2), "%s", p.serverUrl.c_str());

            GRRLIB_PrintfTTF(ROW_X + 14, ry + 8,  font, line1, 18, 0xEEEEEEFF);
            GRRLIB_PrintfTTF(ROW_X + 14, ry + 32, font, line2, 13, 0x778899FF);

            /* Delete hint when selected and not in confirm mode */
            if (sel && !confirmDelete) {
                const char* hint = "-: Delete";
                int hw = GRRLIB_WidthTTF(font, hint, 13);
                GRRLIB_PrintfTTF(ROW_X + ROW_W - hw - 10, ry + 22, font, hint, 13, 0xFF6666AA);
            }
        }
    }

    /* Delete confirmation overlay */
    if (confirmDelete) {
        GRRLIB_Rectangle(100, 185, 440, 110, 0x000000DD, 1);
        GRRLIB_Rectangle(100, 185, 440,   2, 0xFF4444FF, 1);
        const char* q = "Delete this profile?";
        int qw = GRRLIB_WidthTTF(font, q, 20);
        GRRLIB_PrintfTTF((640-qw)/2, 210, font, q, 20, 0xFF6666FF);
        const char* hint2 = "A: Yes, delete   B: Cancel";
        int h2w = GRRLIB_WidthTTF(font, hint2, 16);
        GRRLIB_PrintfTTF((640-h2w)/2, 252, font, hint2, 16, 0xCCCCCCFF);
    }

    /* Footer */
    const char* footer = confirmDelete
        ? "A: Confirm Delete   B: Cancel"
        : "A: Connect   -: Delete   B: Back";
    int fw = GRRLIB_WidthTTF(font, footer, 14);
    GRRLIB_PrintfTTF((640 - fw) / 2, 458, font, footer, 14, 0x778899FF);

    /* IR cursor */
    if (ir.valid && cursorTex) {
        orient_t orient;
        WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4,
                       cursorTex, orient.roll, 1, 1, 0xFFFFFFFF);
    }
}
