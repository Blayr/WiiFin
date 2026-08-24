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
     * when the IR sensor is not visible (pointer off-screen). gcDown is the
     * subset of btnsDown that specifically originated from the GameCube pad
     * this tick (edge-detected) -- vo_gx.c's own overlay/pause/seek handling
     * never sees GC pad state, so those bits are always safe to act on
     * directly even in video mode, unlike real-Wiimote-sourced bits which
     * ov_tick may have already handled. */
    void tick(float timePos, int paused, uint32_t btnsDown, uint32_t btnsHeld,
              float irX, float irY, bool irValid, uint32_t gcDown = 0);

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
        Help,       /* GC-pad shortcut reference screen  */
    };
    State state       = State::Hidden;
    int   hideTimer   = 0;          /* frames remaining before auto-hide    */
    float lastTimePos = -1.0f;

    /* True when the current Main-state session was revealed via the GC pad
     * (vo_gx.c never sees GC input, so its own overlay/pause/seek handling
     * stays inactive) -- gates onFrame()'s native drawing and lets tick()
     * safely perform pause/seek itself instead of deferring to ov_tick. */
    bool nativeMode = false;

    /* nativeMode-only: index (0-5) of the focused icon in the native
     * control row -- Prev/-10s/Play-Pause/+10s/Next/Help -- moved with
     * Left/Right, activated with A. Non-native (Wiimote/IR) sessions don't
     * use this; their Left/Right stays a direct seek shortcut, matching
     * vo_gx.c's own hover+click model. */
    int mainFocus = 2; /* default to Play/Pause, the most-used action */

    /* nativeMode-only: progress-bar scrubbing, entered via Down from the
     * icon row. While true, Left/Right move a pending seek target
     * (scrubPos, absolute seconds) instead of cycling icon focus -- A
     * commits it with wii_player_seek_abs(), B/Up cancel back to the icon
     * row without seeking. Lets you land on an arbitrary position (e.g.
     * 16:30) instead of only ever moving 10s at a time. */
    bool  barFocused    = false;
    float scrubPos      = 0.0f;
    int   barHoldFrames = 0; /* consecutive ticks Left/Right has been held -- drives step acceleration */
    int   barMissFrames = 0; /* ticks since Left/Right last read as held -- grace period so a single
                               * flickery frame (GC pad diagonal-suppression noise) doesn't reset
                               * barHoldFrames back to the slow tier mid-hold */

    /* Sub-menu selection */
    int audioPickSel = 0;
    int subPickSel   = 0;

    /* Skip-intro prompt */
    bool introPromptShown = false;

    /* Transient top-right volume HUD (Android-style): bumped on every
     * adjustVolume() call, drawn by onFrame() while volIndicatorTimer > 0
     * regardless of state/nativeMode -- MPlayer's mixer level isn't
     * readable from WiiFin's side (would need patching the vendored
     * libmplayer.a), so this is a relative 0..volLevelMax gauge WiiFin
     * tracks itself from its own up/down presses, not a true % readout. */
    static const int volLevelMax = 20;
    int volLevel         = volLevelMax / 2;
    int volIndicatorTimer = 0;
    void adjustVolume(int delta);

    /* IR pointer state — written by tick(), read by onFrame() for rendering */
    float lastIrX    = -1.0f;
    float lastIrY    = -1.0f;
    int   irHover    = -1;   /* index 0-4 of hovered button, or -1 */

    /* Internal helpers */
    void activateMain();
    void activateAudioPick();
    void activateSubPick();
    void activateHelp();
    void hide();

    void showOSD(const char* text, int durationMs = 4000);
    void refreshTimeOSD(float timePos, int paused);
    void updateIntroPrompt(float timePos);

    /* Format seconds to MM:SS or HH:MM:SS */
    static const char* fmtTime(float secs, char* buf, int bufsz);
};
