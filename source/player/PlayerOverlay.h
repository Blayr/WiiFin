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

    /* HUD title (movie name, or "Series\nS1E3 - Episode") -- drawn in the
     * top-left of the GC-pad native controls. */
    std::string title;

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

    /* Autoplay. In: App sets autoplayNext when the next episode should
     * autoplay. Out: autoplayGo is true while the end-of-episode countdown
     * is running (cleared if the user cancels); userInteracted is set by any
     * button press this session. */
    bool autoplayNext   = false;
    bool autoplayGo     = false;
    bool userInteracted = false;
};

/* Countdown shown before autoplaying the next episode. */
static const float AUTOPLAY_LEAD_SECS = 10.0f;

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
    /* gcCStickX: Input::gcCStickHeldX(), drives the live A/V delay nudge. */
    void tick(float timePos, int paused, uint32_t btnsDown, uint32_t btnsHeld,
              float irX, float irY, bool irValid, uint32_t gcDown = 0,
              int gcCStickX = 0);

    /* Call once, right after construction and before wii_player_play() --
     * shows a bottom-right toast for ~2s with the transcode bitrate and
     * whether it was picked as the fast/wired or slow/wireless target, so
     * you can tell at a glance what this session actually requested. Wall-
     * clock timed (not frame-count), unlike the volume HUD's timer, since
     * the bgThread tick rate isn't reliably ~30Hz (see the scrub-bar
     * accel fix earlier). */
    void showBitrateIndicator(int kbps, int resW, int resH);

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

    /* ctx.title folded to printable ASCII (the 5x7 pixel font has no
     * glyphs past 0x7E) -- computed once in the constructor rather than
     * per frame. */
    std::string titleAscii;

    /* nativeMode-only: index (0-3) of the focused icon in the native
     * control row -- Prev/Play-Pause/Next/Help -- moved with
     * Left/Right, activated with A. Non-native (Wiimote/IR) sessions don't
     * use this; their Left/Right stays a direct seek shortcut, matching
     * vo_gx.c's own hover+click model. */
    int mainFocus = 1; /* default to Play/Pause, the most-used action */

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

    /* End-of-episode autoplay countdown. B cancels it; returns true when it
     * consumed a B press (so it doesn't also exit playback). */
    bool  updateAutoplay(float timePos, bool anyPress, bool bPress);
    bool  autoplayCancelled = false;
    bool  autoplayFired     = false;
    float autoplayFrac      = 0.0f; /* countdown bar fill, 0..1 */

    /* Live A/V delay nudge (C-stick). The toast updates per notch, but the
     * MPlayer command is only sent once the stick idles (sending per notch
     * was slow and janky). Clamped to +/-2 s. */
    void adjustAudioDelayNotch(int dir); /* dir: -1 or +1 */
    void flushPendingAudioDelay();
    float    delayPendingDeltaSecs = 0.0f; /* unsent delta, added to g_wiifin_audio_delay_secs */
    uint32_t delayNextNotchMs      = 0;    /* gettime()-ms; next notch allowed once passed while held */
    uint32_t delayApplyDeadlineMs  = 0;    /* gettime()-ms; flush pending once passed with no new notch */

    /* A/V-delay toast, drawn in every state like the volume gauge. */
    char delayIndicatorText[32] = "";
    int  delayIndicatorTimer    = 0; /* frames remaining, mirrors volIndicatorTimer */

    /* Transient bottom-right bitrate/network toast -- see
     * showBitrateIndicator() above. Drawn by onFrame() while gettime() is
     * still under bitrateIndicatorExpireTicks, same "regardless of state/
     * nativeMode" treatment as the volume HUD. */
    char     bitrateIndicatorText[64] = "";
    // showBitrateIndicator() is called before wii_player_play() even starts
    // -- loading/buffering can take several seconds, so the 2s countdown
    // must not start until the first real (non-loading-screen) frame is
    // actually drawn, or most/all of it burns away invisibly before
    // anything's on screen to show it against.
    bool     bitrateIndicatorPending = false;
    uint64_t bitrateIndicatorExpireTicks = 0;

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
