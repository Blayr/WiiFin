#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * wii_player_play - Play a URL using the integrated MPlayer CE engine.
 *
 * Calling convention:
 *  1. If g_wiifin_grrlib_render_cb is set, GRRLIB may still be active
 *     (bgThread renders ring.png).  mpgxInit() will call
 *     g_wiifin_grrlib_cleanup_cb → GRRLIB_Exit() before taking over GX.
 *  2. This blocks until playback ends.
 *  3. After return, the caller MUST call GRRLIB_Init() to reclaim the GX.
 *
 * @param url  Null-terminated HTTP(S) URL to stream.
 * @return  PLAYER_STOP_* constant describing why playback ended.
 */
int wii_player_play(const char* url);

/* ------------------------------------------------------------------
 * GRRLIB loading spinner callbacks — set by App.cpp before
 * wii_player_play() so bgThread renders ring.png via GRRLIB during
 * the open_stream() → reinit_video_chain() window.
 *
 *  g_wiifin_grrlib_render_cb  — called every frame from bgThread;
 *     must do GRRLIB_FillScreen + DrawImg + Render.
 *  g_wiifin_grrlib_cleanup_cb — called once by mpgxInit() to do
 *     GRRLIB_FreeTexture + GRRLIB_Exit().
 * ------------------------------------------------------------------ */
extern void (*g_wiifin_grrlib_render_cb)(void);
extern void (*g_wiifin_grrlib_cleanup_cb)(void);

/**
 * wii_player_stop - Asynchronously request the active stream to stop.
 * Safe to call from any context while wii_player_play() is running.
 */
void wii_player_stop(void);

/**
 * wii_player_set_cursor_tex - Register the PointerP1 cursor texture.
 * Call with the GRRLIB_texImg data pointer, width, height, and GX texture
 * format before wii_player_play(); the GXTexObj is initialised internally.
 * Pass data=NULL to revert to the built-in crosshair fallback.
 */
#include <ogc/gx.h>
void wii_player_set_cursor_tex(void* data, u16 w, u16 h, u8 fmt);

/* ------------------------------------------------------------------
 * Return / stop-reason codes from wii_player_play()
 * ------------------------------------------------------------------ */
#define PLAYER_STOP_EOF     0   /* Normal end-of-file or user quit via HOME */
#define PLAYER_STOP_ERROR   1   /* Hard error from MPlayer                  */
#define PLAYER_STOP_NEXT    2   /* Overlay requested: play next episode      */
#define PLAYER_STOP_PREV    3   /* Overlay requested: play previous episode  */
#define PLAYER_STOP_AUDIO   4   /* Overlay requested: re-transcode new audio */
#define PLAYER_STOP_SUB     5   /* Overlay requested: re-transcode new sub   */
#define PLAYER_STOP_WIIMENU 6   /* User chose "Wii Menu" from HOME overlay   */
#define PLAYER_STOP_RESET   7   /* User chose "Reset" from HOME overlay      */
#define PLAYER_STOP_HOME    8   /* HOME pressed: exit to GRRLIB overlay, may resume */

/* Set before wii_player_play(); read back after it returns to know the reason. */
extern volatile int g_player_stop_reason;

/* Known duration (seconds) fetched from Jellyfin API — written by App before
 * wii_player_play(); used as a fallback when the MPEG-TS demuxer cannot
 * determine the stream duration (common for transcoded live-style streams). */
extern volatile float g_wiifin_known_duration;

/* Set to 1 in wii_player_play() before mplayer_main(); cleared by the
 * patched mplayer.c main-loop hook once playback has progressed > 1 s
 * from the initial decoded position.  onFrame() shows a loading screen
 * while set.
 * Stays 0 for track-switch restarts (g_wiifin_track_switch was 1) so
 * the loading overlay is skipped and the video resumes seamlessly. */
extern volatile int g_wiifin_loading_active;

/* Set to 1 by App.cpp before calling wii_player_play() for an
 * audio/subtitle track-switch restart.  Consumed (reset to 0) at the
 * start of wii_player_play() to suppress the loading screen for that
 * session only. */
extern volatile int g_wiifin_track_switch;

/* Tracks the PTS of the first decoded frame so loading_active is cleared
 * based on progress, not absolute PTS (handles mid-stream resume). */
extern float g_wiifin_loading_start_pos;

/* ------------------------------------------------------------------
 * MPlayer state — written by the patched mplayer.c main loop,
 * read by WiiFin's background thread / overlay drawing code.
 * ------------------------------------------------------------------ */
extern volatile float g_mplayer_time_pos;  /* current video PTS (seconds)    */
extern volatile float g_mplayer_duration;  /* total stream duration (seconds) */
extern volatile int   g_mplayer_paused;    /* 1 while MPlayer is paused        */

/* ------------------------------------------------------------------
 * MPlayer control requests — written by background thread,
 * consumed once per main-loop iteration by the patched mplayer.c.
 * ------------------------------------------------------------------ */
extern volatile float g_wiifin_seek_secs;  /* set >=0 to seek absolute       */
extern volatile int   g_wiifin_vol_delta;  /* set non-zero to adjust volume   */

/* ------------------------------------------------------------------
 * Texture overlay callback — set before wii_player_play().
 * Called from gx_supp.c::mpgxRunOverlay() (from vo_gx.c flip_page)
 * each frame, with a pointer to the Y-plane texture buffer, so the
 * overlay can draw directly into the luma channel (no GX state change).
 *
 * IMPORTANT: the callback must NOT call GRRLIB or change the GX state.
 * ------------------------------------------------------------------ */
extern void (*g_wiifin_overlay_cb)(uint8_t* Ytex, int Ywidth, int Yheight);

/* ------------------------------------------------------------------
 * g_stream_opened_cb — one-shot callback fired immediately after the
 * stream URL is opened (Jellyfin transcode job has started).
 * ------------------------------------------------------------------ */
extern void (*g_stream_opened_cb)(void);

/* ------------------------------------------------------------------
 * Convenience functions used by the PlayerOverlay background thread.
 * All are thread-safe (just set volatiles / call mp_input_queue_cmd).
 * ------------------------------------------------------------------ */
/* Show OSD text on top of the video (calls MPlayer's osd_show_text command).
 * durationMs: display duration in milliseconds.
 * Safe to call from the background thread. */
void wii_player_show_osd(const char* text, int durationMs);

void wii_player_pause_toggle(void);
void wii_player_seek_abs(float seconds);
void wii_player_seek_rel(float delta);
void wii_player_vol_up(void);
void wii_player_vol_down(void);
void wii_player_request_next(void);
void wii_player_request_prev(void);
void wii_player_request_audio_switch(void);
void wii_player_request_sub_switch(void);

/* ------------------------------------------------------------------
 * Track selector data — set before wii_player_play() so the GX overlay
 * can show a clickable audio / subtitle picker.
 * ------------------------------------------------------------------ */
#define WIIFIN_TRACK_MAX  12   /* max tracks per type */

typedef struct {
    char label[64];  /* display string shown in the picker              */
    int  index;      /* stream index passed to getTranscodingUrl()      */
} WiiTrackInfo;

/* Audio tracks: fill g_wiifin_audio_tracks[0..n-1], set g_wiifin_audio_count.
 * Subtitle tracks: same for g_wiifin_sub_tracks / g_wiifin_sub_count;
 *   entry 0 is always "Off" with index=-1.
 * g_wiifin_current_audio / g_wiifin_current_sub: index of the active track.
 * After wii_player_play() returns PLAYER_STOP_AUDIO / PLAYER_STOP_SUB,
 * read g_wiifin_selected_audio / g_wiifin_selected_sub for the new choice. */
extern WiiTrackInfo   g_wiifin_audio_tracks[WIIFIN_TRACK_MAX];
extern int            g_wiifin_audio_count;
extern int            g_wiifin_current_audio;   /* index value, not array pos */
extern volatile int   g_wiifin_selected_audio;  /* written by GX overlay      */

extern WiiTrackInfo   g_wiifin_sub_tracks[WIIFIN_TRACK_MAX];
extern int            g_wiifin_sub_count;
extern int            g_wiifin_current_sub;     /* -1 = off                   */
extern volatile int   g_wiifin_selected_sub;    /* written by GX overlay      */
extern volatile int   g_wiifin_stopping;         /* set before wii_player_stop */

/* When > 0, wii_player_play() adds "-ss <g_wiifin_ss_secs>" to MPlayer's
 * argv so it discards stream output until that many seconds have elapsed.
 * Use with RESUME_PAD_TICKS: set to 3.0f whenever startTimeTicks > 3 s so
 * MPlayer skips the back-off portion and starts at the exact target position,
 * eliminating the 6-second initial A/V desync on track switches / resumes. */
extern volatile float g_wiifin_ss_secs;

#ifdef __cplusplus
}

/* Attach a PlayerOverlay before wii_player_play().
 * Pass nullptr to detach (no overlay). */
struct PlayerOverlay;
void wii_player_set_overlay(PlayerOverlay* overlay);

/* -----------------------------------------------------------------------
 * Music (audio-only) playback path.
 *
 * wii_player_play_audio() uses MPlayer with audio-optimised args
 * (lavf demuxer, no video codec, smaller cache).  The frame callback
 * (g_wiifin_overlay_cb) must be set before calling this function;
 * it will NOT be overwritten the way wii_player_play() does.
 *
 * wii_player_set_music_tick() registers a callback that is invoked
 * ~60 Hz by the background input-polling thread, receiving current
 * pause state and button events — used by MusicOverlay::tick().
 * ----------------------------------------------------------------------- */

/* Play audio-only URL; same return codes as wii_player_play(). */
int wii_player_play_audio(const char* url);

/* Register a tick callback for music overlay (nullptr to clear). */
void wii_player_set_music_tick(
    void (*cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld));

/* Register a GRRLIB render callback for audio-only playback.
 * Called ~60 Hz from the background thread; the callback must call
 * GRRLIB_Render() itself.  Pass nullptr to clear. */
void wii_player_set_audio_render_cb(void (*cb)());

#endif

