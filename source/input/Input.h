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
};
