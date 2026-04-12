#pragma once
#include <vector>
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include "../core/SavedProfile.h"

enum class ProfileResult {
    None,
    Selected,   /* selectedIdx is set — load that profile                */
    DeleteOne,  /* selectedIdx is set — caller removes it then re-loops  */
    AddNew,     /* user wants to connect a new account                   */
    Back,       /* user pressed B                                        */
};

class ProfileView {
public:
    ProfileView(GRRLIB_ttfFont* font, GRRLIB_texImg* cursor,
                const std::vector<SavedProfile>& profiles);

    /* Call every frame; returns None while the screen is active. */
    ProfileResult update(ir_t& ir);
    void          render(ir_t& ir);

    int selectedIdx = 0; /* valid when result is Selected or DeleteOne */

private:
    GRRLIB_ttfFont* font;
    GRRLIB_texImg*  cursorTex;
    const std::vector<SavedProfile>& profiles;

    int  focusedRow    = 0;
    bool irMode        = false;
    bool confirmDelete = false;

    static const int ROW_H   = 64;
    static const int ROW_Y0  = 90;
    static const int ROW_X   = 60;
    static const int ROW_W   = 520;
    static const int MAX_VIS = 6; /* max visible rows (profiles + Add New) */

    void drawGradientBG();
};
