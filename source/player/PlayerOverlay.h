#pragma once

#include <string>
#include <vector>
#include <stdint.h>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * PlayerOverlayContext — provided by App before calling wii_player_play()
 * ----------------------------------------------------------------------- */
struct PlayerOverlayContext {
    /* Episode navigation (set for TV shows; empty for movies) */
    std::vector<JellyfinEpisode> episodes;   /* all eps in current season  */
    int                          episodeIdx; /* index of the currently-playing ep */
    std::string                  episodeTitle;

    /* Media streams (audio + subtitle tracks available for the item) */
    std::vector<MediaStream> audioStreams;
    std::vector<MediaStream> subStreams;
    int currentAudio = 0;   /* index into audioStreams matching current transcode */
    int currentSub   = -1;  /* index into subStreams; -1 = none                  */

    /* User-selected track after a track-switch request (read by App) */
    int selectedAudio = 0;
    int selectedSub   = -1;

    /* Intro skip info */
    IntroInfo intro;
};

/* -----------------------------------------------------------------------
 * PlayerOverlay — in-player control HUD.
 *
 * Lifecycle:
 *   1. Create before calling wii_player_play().
 *   2. Pass to wii_player_set_overlay(&overlay).
 *   3. wii_player_play() starts a background thread that calls tick() at
 *      ~30 Hz, and registers onFrame() as g_wiifin_overlay_cb so it fires
 *      each video frame from the rendering thread.
 *   4. After wii_player_play() returns, check ctx.selectedAudio/selectedSub
 *      if the stop reason is PLAYER_STOP_AUDIO / PLAYER_STOP_SUB.
 * ----------------------------------------------------------------------- */
class PlayerOverlay {
public:
    explicit PlayerOverlay(PlayerOverlayContext& ctx);

    /* Called ~30 Hz from the background LWP thread.
     * irX/irY are WPAD IR coordinates in [0,640)×[0,480); irValid is false
     * when the IR sensor is not visible (pointer off-screen). */
    void tick(float timePos, int paused, uint32_t btnsDown, uint32_t btnsHeld,
              float irX, float irY, bool irValid);

    /* Static trampoline: registered as g_wiifin_overlay_cb.
     * Called from the MPlayer main thread (rendering loop), safe to write
     * directly to the Y texture.  Must NOT call GRRLIB or change GX state. */
    static void onFrame(uint8_t* Ytex, int Ywidth, int Yheight);

    /* Singleton pointer set in constructor — onFrame uses it. */
    static PlayerOverlay* instance;

private:
    PlayerOverlayContext& ctx;

    /* Overlay visibility */
    enum class State {
        Hidden,
        Main,       /* seek bar + time + basic controls */
        AudioPick,  /* audio track picker               */
        SubPick,    /* subtitle track picker             */
    };
    State state       = State::Hidden;
    int   hideTimer   = 0;          /* frames remaining before auto-hide    */
    float lastTimePos = -1.0f;

    /* Sub-menu selection */
    int audioPickSel = 0;
    int subPickSel   = 0;

    /* Skip-intro prompt */
    bool introPromptShown = false;

    /* IR pointer state — written by tick(), read by onFrame() for rendering */
    float lastIrX    = -1.0f;
    float lastIrY    = -1.0f;
    int   irHover    = -1;   /* index 0-4 of hovered button, or -1 */

    /* Internal helpers */
    void activateMain();
    void activateAudioPick();
    void activateSubPick();
    void hide();

    void showOSD(const char* text, int durationMs = 4000);
    void refreshTimeOSD(float timePos, int paused);
    void updateIntroPrompt(float timePos);

    /* Format seconds to MM:SS or HH:MM:SS */
    static const char* fmtTime(float secs, char* buf, int bufsz);
};
