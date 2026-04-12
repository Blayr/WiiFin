#pragma once
#include <ogc/conf.h>

// 16:9 detection: the Wii EFB is 640x480 (4:3). When displayed on a 16:9 TV
// every pixel is stretched by 4/3 horizontally. Drawing textures at scaleX=0.75
// pre-squishes them so they appear at the correct aspect ratio after stretch.
namespace WiiUtils {
    extern bool widescreen;
    inline void detectAspect() {
        widescreen = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);
    }
    // Horizontal scale to apply to drawn textures when in 16:9 mode
    inline float wsScaleX() { return widescreen ? 0.75f : 1.0f; }
}