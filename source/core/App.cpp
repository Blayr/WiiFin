#include "App.h"
#include "Utils.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "../ui/ConnectView.h"
#include "../ui/MusicPlayerView.h"
#include "../ui/ProfileView.h"
#include "../ui/SettingsView.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <errno.h>
#include <stdio.h>
#include <gccore.h>
#include <sys/iosupport.h>

#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include "../player/WiiPlayer.h"
#include "../player/PlayerOverlay.h"

/* Ring spinner PNG embedded asset (declared here for use in runPlaySession) */
extern unsigned char data_ring_png[];
extern unsigned int  data_ring_png_len;

/* Assets used by runPlaySession's HOME suspend overlay and doShowHomeOverlay */
extern unsigned char data_wii_font_ttf[];
extern unsigned int  data_wii_font_ttf_len;
extern unsigned char data_button_start_png[];
extern unsigned int  data_button_start_png_len;
extern unsigned char data_cursors_PointerP1_64_png[];
extern unsigned int  data_cursors_PointerP1_64_png_len;

/* Forward declaration — defined later in this file, before App::loop() */
static bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                               GRRLIB_texImg* cursorPointerTex, bool musicEnabled);

/* --- GRRLIB loading spinner for the pre-GX phase ---
 * While MPlayer opens the stream, GRRLIB stays active and bgThread
 * calls grrlibSpinnerRender() every frame to draw ring.png rotating. */
static GRRLIB_texImg* s_loadingRingTex = nullptr;
static float          s_ringAngle      = 0.0f;

static void grrlibSpinnerRender(void)
{
    static int render_count = 0;
    render_count++;
    if (render_count <= 3 || (render_count % 60) == 0)
        SYS_Report("[GRRLIB_spinner] render #%d tex=%p angle=%.0f\n",
                   render_count, s_loadingRingTex, s_ringAngle);
    GRRLIB_FillScreen(0x0A1628FF);
    if (s_loadingRingTex) {
        GRRLIB_SetMidHandle(s_loadingRingTex, true);
        GRRLIB_DrawImg(320, 240, s_loadingRingTex, s_ringAngle,
                       1.0f, 1.0f, 0xFFFFFFFF);
        GRRLIB_SetMidHandle(s_loadingRingTex, false);
    }
    GRRLIB_Render();
    s_ringAngle += 4.0f;
    if (s_ringAngle >= 360.0f) s_ringAngle -= 360.0f;
}

static void grrlibSpinnerCleanup(void)
{
    if (s_loadingRingTex) {
        GRRLIB_FreeTexture(s_loadingRingTex);
        s_loadingRingTex = nullptr;
    }
    GRRLIB_Exit();
}

/* State captured before wii_player_play() so the mplayer callback can send
 * POST /Sessions/Playing after the stream is opened on the server. */
volatile bool g_app_powerOff = false;
volatile bool g_app_reset    = false;
static volatile bool s_restartApp = false;
static void onPower() { g_app_powerOff = true; }
static void onReset(u32, void*) { g_app_reset = true; }

struct {
    JellyfinClient* client;
    std::string serverUrl;
    JellyfinAuth   auth;
    std::string    itemId;
    std::string    mediaSourceId;
    std::string    playSessionId;
} s_pendingReport;

static void onStreamOpened() {
    s_pendingReport.client->reportPlaybackStart(
        s_pendingReport.serverUrl,
        s_pendingReport.auth,
        s_pendingReport.itemId,
        s_pendingReport.mediaSourceId,
        s_pendingReport.playSessionId);
}

/* -----------------------------------------------------------------------
 * sanitizeTrackLabel — copies src into dst (max dstSize bytes) converting
 * UTF-8 accented characters to their ASCII base letters so they render
 * correctly on the ASCII-only bitmap font used by vo_gx.c.
 * Any other non-ASCII byte sequence is silently skipped.
 * ----------------------------------------------------------------------- */
static void sanitizeTrackLabel(const char* src, char* dst, size_t dstSize)
{
    if (!dstSize) return;
    size_t di = 0;
    const unsigned char* s = (const unsigned char*)src;
    while (*s && di + 1 < dstSize) {
        unsigned char c = *s;
        if (c < 0x80) {
            dst[di++] = (char)c;
            s++;
        } else if (c == 0xC3) {
            /* U+00C0–U+00FF: Latin-1 Supplement (most French/Spanish/German) */
            s++;
            if (!*s) break;
            unsigned char c2 = *s++;
            char m = '\0';
            if      (c2 >= 0x80 && c2 <= 0x85) m = 'a'; /* à á â ã ä å */
            else if (c2 == 0x87)                m = 'c'; /* ç */
            else if (c2 >= 0x88 && c2 <= 0x8B) m = 'e'; /* è é ê ë */
            else if (c2 >= 0x8C && c2 <= 0x8F) m = 'i'; /* ì í î ï */
            else if (c2 == 0x91)                m = 'n'; /* ñ */
            else if (c2 >= 0x92 && c2 <= 0x96) m = 'o'; /* ò ó ô õ ö */
            else if (c2 == 0x98)                m = 'o'; /* ø */
            else if (c2 >= 0x99 && c2 <= 0x9C) m = 'u'; /* ù ú û ü */
            else if (c2 == 0x9D || c2 == 0x9F) m = 'y'; /* ý ÿ */
            else if (c2 >= 0xA0 && c2 <= 0xA5) m = 'A'; /* À Á Â Ã Ä Å */
            else if (c2 == 0xA7)                m = 'C'; /* Ç */
            else if (c2 >= 0xA8 && c2 <= 0xAB) m = 'E'; /* È É Ê Ë */
            else if (c2 >= 0xAC && c2 <= 0xAF) m = 'I'; /* Ì Í Î Ï */
            else if (c2 == 0xB1)                m = 'N'; /* Ñ */
            else if (c2 >= 0xB2 && c2 <= 0xB6) m = 'O'; /* Ò Ó Ô Õ Ö */
            else if (c2 == 0xB8)                m = 'O'; /* Ø */
            else if (c2 >= 0xB9 && c2 <= 0xBC) m = 'U'; /* Ù Ú Û Ü */
            else if (c2 == 0xBD || c2 == 0xBE) m = 'Y'; /* Ý Þ */
            if (m) dst[di++] = m;
        } else {
            /* Skip full multi-byte sequence (2–4 bytes) */
            int extra = (c < 0xE0) ? 1 : (c < 0xF0) ? 2 : 3;
            s++;
            for (int k = 0; k < extra && *s; ++k) s++;
        }
    }
    dst[di] = '\0';
}

/* -----------------------------------------------------------------------
 * runPlaySession — encapsulates the full play loop for one item.
 *
 * It handles:
 *   - building the PlayerOverlayContext
 *   - fetching intro timestamps (for TV episodes)
 *   - the next/prev/audio/sub restart loop
 *
 * Returns true if the user chose "Wii Menu" from the HOME overlay,
 * false for all other stop reasons (EOF, error, back to library).
 * GRRLIB must be active on entry; the function temporarily exits/re-inits
 * GRRLIB around each wii_player_play() call.
 * ----------------------------------------------------------------------- */
static bool runPlaySession(JellyfinClient& client,
                           const JellyfinAuth& auth,
                           const std::string& serverUrl,
                           LibraryView& lv)
{
    /* Working copies of play parameters — updated on next/prev/track change */
    std::string           itemId        = lv.pendingPlayItemId;
    std::string           mediaSourceId = lv.pendingPlayMediaSourceId;
    std::string           playSessionId = lv.pendingPlaySessionId;
    std::string           url           = lv.pendingPlayUrl;
    std::vector<JellyfinEpisode> episodes    = lv.pendingPlayEpisodes;
    int                          episodeIdx  = lv.pendingPlayEpisodeIdx;
    std::vector<MediaStream>     audioStreams = lv.pendingPlayAudioStreams;
    std::vector<MediaStream>     subStreams   = lv.pendingPlaySubStreams;
    int audioIdx = lv.pendingPlayAudioIdx;
    int subIdx   = lv.pendingPlaySubIdx;

    /* The seek offset (100-ns ticks) passed to the Jellyfin transcoder.
     * Preserved across retries so a premature-EOF retry resumes from the
     * same position rather than restarting from the beginning. */
    long long startTimeTicks = lv.pendingPlayStartTimeTicks;
    /* Known duration from Jellyfin metadata — used as g_wiifin_known_duration
     * to drive the progress bar when the MPEG-TS demuxer returns 0 for length. */
    long long runtimeTicks   = lv.pendingPlayRuntimeTicks;

    /* Capture BGM state ONCE before any play session in this call.
     * Must not be re-captured inside the outer loop: after a track switch
     * (PLAYER_STOP_AUDIO/SUB) the inner loop skips MusicBGM::resume(), so on
     * the next outer iteration isRunning() would return false even though BGM
     * was originally active — causing resume() to be skipped again when the
     * user finally presses B, leaving ASND dead and all sounds silent. */
    bool musicWasRunning = MusicBGM::isRunning();
    SYS_Report("[runPlay] start music=%d itemId=%s ep=%d\n",
               (int)musicWasRunning, lv.pendingPlayItemId.c_str(),
               (int)lv.pendingPlayEpisodes.size());

    for (;;) {
        /* Pause BGM at the start of every outer iteration so ao_gekko can take
         * over the AI DMA.  On the first iteration this is the initial pause;
         * on NEXT/PREV iterations it re-pauses BGM that was resumed at the end
         * of the previous inner loop.  On AUDIO/SUB iterations it is a no-op
         * (BGM was never resumed because isTrackSwitch==true). */
        MusicBGM::pause();
        SYS_Report("[runPlay] music paused\n");

        /* Build overlay context for this play */
        PlayerOverlayContext ctx;
        ctx.episodes     = episodes;
        ctx.episodeIdx   = episodeIdx;
        ctx.audioStreams  = audioStreams;
        ctx.subStreams    = subStreams;
        ctx.currentAudio = audioIdx;
        ctx.currentSub   = subIdx;
        ctx.selectedAudio = audioIdx;
        ctx.selectedSub   = subIdx;
        if (!episodes.empty() && episodeIdx < (int)episodes.size())
            ctx.episodeTitle = episodes[episodeIdx].name;

        /* Try to fetch intro timestamps (for TV episodes) */
        if (!episodes.empty()) {
            SYS_Report("[runPlay] getIntroTimestamps start\n");
            client.getIntroTimestamps(serverUrl, auth, itemId, ctx.intro);
            SYS_Report("[runPlay] getIntroTimestamps done\n");
        }

        PlayerOverlay overlay(ctx);
        wii_player_set_overlay(&overlay);

        /* Wire up playback-start reporting */
        s_pendingReport.client        = &client;
        s_pendingReport.serverUrl     = serverUrl;
        s_pendingReport.auth          = auth;
        s_pendingReport.itemId        = itemId;
        s_pendingReport.mediaSourceId = mediaSourceId;
        g_stream_opened_cb = onStreamOpened;

        /* ---- Inner play-retry loop ------------------------------------------
         * On PLAYER_STOP_ERROR (premature EOF detected in WiiPlayer.cpp), the
         * Jellyfin transcoder session has most likely expired during the TLS
         * connection's 504-retry cycle.  Re-acquire a fresh PlaybackInfo session
         * at the same seek position and retry, up to 3 times total.
         * -------------------------------------------------------------------- */
        int reason     = PLAYER_STOP_EOF;
        int playRetries = 0;
        for (;;) {
            /* Update the session id for the playback-start callback */
            s_pendingReport.playSessionId = playSessionId;

            /* ---- Populate GX overlay track lists --------------------------------
             * Fill audio / sub track arrays so vo_gx.c can draw the pickers.     */
            {
                g_wiifin_audio_count   = 0;
                g_wiifin_current_audio = audioIdx;
                g_wiifin_selected_audio = audioIdx;
                int n = (int)audioStreams.size();
                if (n > WIIFIN_TRACK_MAX) n = WIIFIN_TRACK_MAX;
                for (int ti = 0; ti < n; ++ti) {
                    sanitizeTrackLabel(audioStreams[ti].displayTitle.c_str(),
                                       g_wiifin_audio_tracks[ti].label,
                                       sizeof(g_wiifin_audio_tracks[ti].label));
                    g_wiifin_audio_tracks[ti].index = audioStreams[ti].index;
                }
                g_wiifin_audio_count = n;

                g_wiifin_sub_count     = 0;
                g_wiifin_current_sub   = subIdx;
                g_wiifin_selected_sub  = subIdx;
                /* Entry 0 is always "Off" */
                snprintf(g_wiifin_sub_tracks[0].label,
                         sizeof(g_wiifin_sub_tracks[0].label), "Off");
                g_wiifin_sub_tracks[0].index = -1;
                int ns = (int)subStreams.size();
                if (ns > WIIFIN_TRACK_MAX - 1) ns = WIIFIN_TRACK_MAX - 1;
                for (int ti = 0; ti < ns; ++ti) {
                    sanitizeTrackLabel(subStreams[ti].displayTitle.c_str(),
                                       g_wiifin_sub_tracks[ti + 1].label,
                                       sizeof(g_wiifin_sub_tracks[ti + 1].label));
                    g_wiifin_sub_tracks[ti + 1].index = subStreams[ti].index;
                }
                g_wiifin_sub_count = ns + 1; /* includes "Off" entry */
            }

            /* Set the known duration so the progress bar works when the
             * MPEG-TS demuxer cannot determine stream length itself. */
            g_wiifin_known_duration = (runtimeTicks > 0)
                                      ? (float)(runtimeTicks / 10000000LL)
                                      : 0.0f;

            /* Load ring.png and set up GRRLIB spinner callbacks.
             * GRRLIB stays active — bgThread renders ring.png via
             * grrlibSpinnerRender() until mpgxInit() calls
             * grrlibSpinnerCleanup() → GRRLIB_Exit(). */
            s_loadingRingTex = GRRLIB_LoadTexture(data_ring_png);
            s_ringAngle = 0.0f;
            SYS_Report("[App] GRRLIB spinner: tex=%p ringLen=%u\n",
                       s_loadingRingTex, data_ring_png_len);
            g_wiifin_grrlib_render_cb  = grrlibSpinnerRender;
            g_wiifin_grrlib_cleanup_cb = grrlibSpinnerCleanup;

            /* Render one frame immediately so the ring is visible right away */
            grrlibSpinnerRender();

            /* Tell MPlayer to skip the RESUME_PAD (3 s) at demuxer level when
             * the Jellyfin URL was back-shifted by that amount — eliminates the
             * initial 6-second A/V desync produced by Jellyfin's live transcoder
             * on track switches and position resumes.  Fresh playback from the
             * start (startTimeTicks == 0) never has a pad, so skip is 0. */
            g_wiifin_ss_secs = (startTimeTicks > 30000000LL) ? 3.0f : 0.0f;
            SYS_Report("[App] wii_player_play: startTicks=%lld ss=%.1f\n",
                       startTimeTicks, (double)g_wiifin_ss_secs);
            reason = wii_player_play(url.c_str());
            SYS_Report("[DBG] wii_player_play RETURNED reason=%d loading=%d\n",
                       reason, (int)g_wiifin_loading_active);

            /* After play returns, GRRLIB was exited by mpgxInit's cleanup.
             * Re-init it for the app UI. */
            g_wiifin_grrlib_render_cb  = nullptr;
            g_wiifin_grrlib_cleanup_cb = nullptr;
            s_loadingRingTex = nullptr;
            WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
            WPAD_SetVRes(WPAD_CHAN_0, 640, 480);
            SYS_Report("[DBG] GRRLIB_Init() CALLING @ runPlaySession return\\n");
            GRRLIB_Init();
            SYS_Report("[DBG] GRRLIB_Init() DONE\\n");
            /* GRRLIB_Init internally calls VIDEO_SetBlack(false).  Re-blank
             * immediately so the uninitialised XFBs are never visible. */
            VIDEO_SetBlack(true);
            VIDEO_Flush();
            /* Show the loading spinner in both GX framebuffers so there is
             * no flash while the app is tearing down the play session. */
            {
                GRRLIB_texImg* tmpRing = GRRLIB_LoadTexture(data_ring_png);
                for (int _fi = 0; _fi < 2; ++_fi) {
                    GRRLIB_FillScreen(0x0A1628FF);
                    if (tmpRing) {
                        GRRLIB_SetMidHandle(tmpRing, true);
                        GRRLIB_DrawImg(320, 240, tmpRing, 0, 1.0f, 1.0f, 0xFFFFFFFF);
                        GRRLIB_SetMidHandle(tmpRing, false);
                    }
                    GRRLIB_Render();
                }
                GRRLIB_FreeTexture(tmpRing);
            }
            /* Both framebuffers now contain the spinner — safe to unblank. */
            VIDEO_SetBlack(false);
            VIDEO_Flush();

            long long positionTicks = (long long)(g_mplayer_time_pos * 10000000.0f);

            /* Retry on premature EOF (transcoder session expired mid-connect).
             * Skip reportPlaybackStopped on retry attempts: position is 0,
             * nothing meaningful to report, and the call blocks ~3 minutes
             * on a busy server. We always delete the stale encoding first so
             * the server reclaims the transcoder slot immediately. */
            bool willRetry = (reason == PLAYER_STOP_ERROR && playRetries < 3);

            /* For track switches we immediately restart at the same position:
             * skip reportPlaybackStopped (saves one full TLS+HTTP round-trip,
             * ~2-4 s) and skip BGM resume (we'll pause it again on the next
             * loop iteration anyway). */
            bool isTrackSwitch = (reason == PLAYER_STOP_AUDIO || reason == PLAYER_STOP_SUB);
            bool isHomeSuspend = (reason == PLAYER_STOP_HOME);

            /* Restart BGM now — before the blocking network calls — so the
             * user hears music while session cleanup (report + delete) runs.
             * Only done when we are NOT about to retry (which calls
             * wii_player_play again, requiring ASND to stay ended), and NOT
             * for track switches (immediately restarted, BGM paused again).
             * When BGM was not running, still reinitAudio() so that ASND
             * (and therefore SoundFX) is alive after AESND_Reset().
             * Also skip for HOME suspend: we will either resume MPlayer
             * immediately (BGM stays paused) or exit (BGM not needed). */
            if (!willRetry && !isTrackSwitch && !isHomeSuspend) {
                SYS_Report("[DBG] BGM restore: musicWasRunning=%d\n", (int)musicWasRunning);
                MusicBGM::stop();
                MusicBGM::init(musicWasRunning);
                SYS_Report("[DBG] BGM restore DONE\n");
            }

            /* Only report stopped if something actually played (position > 0).
             * When positionTicks == 0, nothing was played — skip the call to
             * avoid blocking for minutes on a slow/busy server.
             * Also skip for track switches: playback resumes immediately at
             * the same position so the "stopped" report is both misleading
             * and a source of unnecessary multi-second loader freeze. */
            if (!willRetry && !isTrackSwitch && !isHomeSuspend && positionTicks > 0) {
                client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                             playSessionId, positionTicks);
            }
            if (!isHomeSuspend)
                client.deleteActiveEncoding(serverUrl, auth, playSessionId);

            if (willRetry) {
                long long retryTicks = startTimeTicks + positionTicks;
                std::string retryUrl, retrySession;
                if (client.getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId,
                                             audioIdx, subIdx, retryTicks,
                                             retryUrl, retrySession)) {
                    ++playRetries;
                    SYS_Report("[App] premature EOF retry %d/3 from tick=%lld\n",
                               playRetries, retryTicks);
                    url           = retryUrl;
                    playSessionId = retrySession;
                    continue;
                }
                /* URL re-acquisition failed; fall through as final stop */
                reason = PLAYER_STOP_EOF;
            }
            break;
        }

        wii_player_set_overlay(nullptr);

        /* --- Handle stop reason --- */
        if (reason == PLAYER_STOP_HOME) {
            /* HOME was pressed during playback.  Show the clean GRRLIB HOME
             * overlay (same as the rest of the app).  B = resume playback;
             * Wii Menu / Reset = exit as usual. */
            long long suspendTicks = startTimeTicks +
                                     (long long)(g_mplayer_time_pos * 10000000.0f);

            /* Load minimal assets — GRRLIB was reinit'd above. */
            GRRLIB_ttfFont* hmFont   = GRRLIB_LoadTTF(data_wii_font_ttf, data_wii_font_ttf_len);
            GRRLIB_texImg*  hmBtn    = GRRLIB_LoadTexture(data_button_start_png);
            GRRLIB_texImg*  hmCursor = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);

            /* musicEnabled = false: BGM is already paused for this play session
             * and should not auto-resume when the user presses B (resume). */
            bool wantsExit = doShowHomeOverlay(hmFont, hmBtn, hmCursor, false);

            GRRLIB_FreeTTF(hmFont);
            GRRLIB_FreeTexture(hmBtn);
            GRRLIB_FreeTexture(hmCursor);

            if (!wantsExit) {
                /* User pressed B (resume): delete old session, restart from
                 * suspended position.  getTranscodingUrl adds a RESUME_PAD
                 * back-off automatically. */
                client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                std::string newUrl, newSession;
                if (client.getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId,
                                             audioIdx, subIdx, suspendTicks,
                                             newUrl, newSession)) {
                    url            = newUrl;
                    playSessionId  = newSession;
                    startTimeTicks = suspendTicks;
                    continue;
                }
                /* Re-acquisition failed — report and fall through to EOF. */
                if (suspendTicks > 0)
                    client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                                 playSessionId, suspendTicks);
                return false;
            } else {
                /* User chose Wii Menu or Reset — report and clean up. */
                if (suspendTicks > 0)
                    client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                                 playSessionId, suspendTicks);
                client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                return true;
            }
        }

        if (reason == PLAYER_STOP_NEXT || reason == PLAYER_STOP_PREV) {
            int nextIdx = (reason == PLAYER_STOP_NEXT) ? episodeIdx + 1 : episodeIdx - 1;
            if (nextIdx < 0 || nextIdx >= (int)episodes.size()) return false;

            /* Fetch item detail for the next episode to get its stream list */
            JellyfinItemDetail nextDetail;
            if (!client.getItemDetail(serverUrl, auth, episodes[nextIdx].id, nextDetail)) return false;

            /* Get the transcoding URL for the next episode */
            std::string nextUrl, nextSession;
            if (!client.getTranscodingUrl(serverUrl, auth,
                                          episodes[nextIdx].id, episodes[nextIdx].id,
                                          0, -1, 0, nextUrl, nextSession)) return false;

            episodeIdx    = nextIdx;
            itemId        = episodes[nextIdx].id;
            mediaSourceId = itemId;
            playSessionId = nextSession;
            url           = nextUrl;
            audioStreams   = nextDetail.audioStreams;
            subStreams     = nextDetail.subtitleStreams;
            audioIdx       = 0;
            subIdx         = -1;
            startTimeTicks = 0;  /* new episode: start from beginning */
            runtimeTicks   = nextDetail.runtimeTicks;  /* update known duration */
            continue;
        }

        if (reason == PLAYER_STOP_AUDIO || reason == PLAYER_STOP_SUB) {
            /* Re-transcode same episode with new audio/sub track, resuming
             * from current playback position (g_mplayer_time_pos is the tick
             * at which the user triggered the switch). */
            /* Read back selection from GX overlay globals (set by vo_gx.c picker) */
            int newAudio = (int)g_wiifin_selected_audio;
            int newSub   = (int)g_wiifin_selected_sub;
            /* Also sync back to PlayerOverlayContext for consistency */
            ctx.selectedAudio = newAudio;
            ctx.selectedSub   = newSub;
            long long switchTicks = (long long)(g_mplayer_time_pos * 10000000.0f);

            std::string newUrl, newSession;
            if (!client.getTranscodingUrl(serverUrl, auth,
                                          itemId, mediaSourceId,
                                          newAudio, newSub, switchTicks, newUrl, newSession)) return false;
            url            = newUrl;
            playSessionId  = newSession;
            audioIdx       = newAudio;
            subIdx         = newSub;
            startTimeTicks = switchTicks;  /* for premature-EOF retry on new session */
            g_wiifin_track_switch = 1;     /* suppress loading overlay on restart */
            continue;
        }

        /* Return true if user chose Wii Menu or Reset (both exit playback) */
        if (reason == PLAYER_STOP_RESET) s_restartApp = true;
        return (reason == PLAYER_STOP_WIIMENU || reason == PLAYER_STOP_RESET);
    }
}

extern unsigned char data_logo_wiifin_png[];
extern unsigned int data_logo_wiifin_png_len;
extern unsigned char data_button_start_png[];
extern unsigned int data_button_start_png_len;
extern unsigned char data_cursors_PointerP1_64_png[];
extern unsigned int data_cursors_PointerP1_64_png_len;
extern unsigned char data_cursors_HandOpenP1_64_png[];
extern unsigned int data_cursors_HandOpenP1_64_png_len;
extern unsigned char data_cursors_HandClosedP1_64_png[];
extern unsigned int data_cursors_HandClosedP1_64_png_len;
extern unsigned char data_wii_font_ttf[];
extern unsigned int data_wii_font_ttf_len;
extern unsigned char data_jp_font_ttf[];
extern unsigned int data_jp_font_ttf_len;

#define logo_wiifin_png      data_logo_wiifin_png
#define logo_wiifin_png_len  data_logo_wiifin_png_len
#define button_start_png      data_button_start_png
#define button_start_png_len  data_button_start_png_len
#define wii_font_ttf      data_wii_font_ttf
#define wii_font_ttf_len  data_wii_font_ttf_len
#define jp_font_ttf       data_jp_font_ttf
#define jp_font_ttf_len   data_jp_font_ttf_len

// --- Button layout constants ---
// btnTex is 512x128 (power-of-2 required by GX/GRRLIB).
// Display at 280x70 (4:1 ratio preserved, sx=sy=0.547).
static const int BX        = 180;
static const int BW        = 280;
static const int BH        = 70;
static const int BY_START  = 125;
static const int B_SPACING = 78;

void App::reloadAssets() {
    SYS_Report("[DBG] reloadAssets ENTER\n");
    GRRLIB_FreeTexture(logoTex);   logoTex   = GRRLIB_LoadTexture(logo_wiifin_png);
    GRRLIB_FreeTexture(btnTex);    btnTex    = GRRLIB_LoadTexture(button_start_png);
    GRRLIB_FreeTexture(cursorPointerTex);    cursorPointerTex    = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);
    if (cursorPointerTex)
        wii_player_set_cursor_tex(cursorPointerTex->data,
                                  (u16)cursorPointerTex->w, (u16)cursorPointerTex->h,
                                  (u8)cursorPointerTex->format);
    GRRLIB_FreeTexture(ringTex);   ringTex   = GRRLIB_LoadTexture(data_ring_png);
    // FreeType was wiped by GRRLIB_Exit() — reload fonts without FreeTTF
    font   = GRRLIB_LoadTTF(wii_font_ttf, wii_font_ttf_len);
    jpFont = GRRLIB_LoadTTF(jp_font_ttf, jp_font_ttf_len);
    SYS_Report("[DBG] reloadAssets EXIT logo=%p btn=%p cursor=%p ring=%p font=%p jpFont=%p\n",
               logoTex, btnTex, cursorPointerTex, ringTex, font, jpFont);
}

void App::init(const char* argv0) {
    SYS_SetPowerCallback(onPower);
    SYS_SetResetCallback(onReset);

    SYS_Report("[WiiFin] init start\n");

    {
        VIDEO_Init();
        SYS_Report("[WiiFin] VIDEO_Init done\n");
        GXRModeObj* m = VIDEO_GetPreferredMode(NULL);
        void* xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(m));
        VIDEO_ClearFrameBuffer(m, xfb, 0x00800080); // YCbCr black — prevents green garbage frame
        VIDEO_Configure(m);
        VIDEO_SetNextFramebuffer(xfb);
        VIDEO_SetBlack(false);  // unblank so Dolphin's renderer connects to VI output
        VIDEO_Flush();
        SYS_Report("[WiiFin] VIDEO_Flush done, waiting 50ms\n");
        usleep(50000);
    }

    {
        static u8 s_pre_fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
        SYS_Report("[WiiFin] GX_Init start\n");
        GX_Init(s_pre_fifo, sizeof(s_pre_fifo));
        SYS_Report("[WiiFin] GX_AbortFrame start\n");
        GX_AbortFrame();
        GX_Flush();
        SYS_Report("[WiiFin] GX pre-init done\n");
    }

    SYS_Report("[WiiFin] GRRLIB_Init start\n");
    GRRLIB_Init();
    SYS_Report("[WiiFin] GRRLIB_Init done\n");

    // Clear both framebuffers to black immediately to avoid green garbage frame
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();

    WiiUtils::detectAspect();

    // Load textures and fonts from embedded data (no file I/O, always fast)
    logoTex   = GRRLIB_LoadTexture(logo_wiifin_png);
    btnTex    = GRRLIB_LoadTexture(button_start_png);
    cursorPointerTex    = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);
    // Register cursor PNG with WiiPlayer so vo_gx.c can draw it as the IR cursor
    if (cursorPointerTex)
        wii_player_set_cursor_tex(cursorPointerTex->data,
                                  (u16)cursorPointerTex->w, (u16)cursorPointerTex->h,
                                  (u8)cursorPointerTex->format);
    font   = GRRLIB_LoadTTF(wii_font_ttf, wii_font_ttf_len);
    jpFont = GRRLIB_LoadTTF(jp_font_ttf, jp_font_ttf_len);
    ringTex = GRRLIB_LoadTexture(data_ring_png);

    // Show a splash frame immediately so the user sees something during init.
    if (logoTex) {
        GRRLIB_FillScreen(0x0A1628FF);   // dark navy (app brand colour, clearly NOT pure black)
        float ls = 0.60f;
        int lw = (int)(logoTex->w * ls);
        GRRLIB_DrawImg((640 - lw) / 2, (480 - (int)(logoTex->h * ls)) / 2,
                       logoTex, 0, ls, ls, 0xFFFFFFFF);
        GRRLIB_Render();
    } else {
        // Logo failed to load — show a plain coloured screen so we know init reached this point
        GRRLIB_FillScreen(0x1E3A5FFF);  // steel-blue diagnostic fallback
        GRRLIB_Render();
    }

    // Now init filesystem and input (may take a moment on first call)
    fatInitDefault();
    // fatMountSimple was removed: calling it after fatInitDefault() overwrites
    // the devoptab entry for "sd" with a stub that has open_r=NULL → errno=88.
    // fatInitDefault() alone correctly registers "sd" with a working FAT driver.

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    // Try argv0, all known prefixes, and NO-prefix (default libfat device)
    if (argv0 && argv0[0]) {
        std::string p(argv0);
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
            argvPath = p.substr(0, slash + 1) + "wiifin.cfg";
    }
    const char* probes[] = {
        argvPath.empty() ? nullptr : argvPath.c_str(),
        "/apps/WiiFin/wiifin.cfg",        // no prefix = default device
        "sd:/apps/WiiFin/wiifin.cfg",
        "fat:/apps/WiiFin/wiifin.cfg",
        "fat0:/apps/WiiFin/wiifin.cfg",
        "fat1:/apps/WiiFin/wiifin.cfg",
        "usb:/apps/WiiFin/wiifin.cfg",
    };
    // mkdirp: create each component of `dir` (e.g. "sd:/apps/WiiFin") only
    // if the device supports mkdir_r.  mkdir() only creates one level, so we
    // walk forward from the first '/' after the device prefix.
    auto mkdirp = [](const std::string& dir) {
        const char* colon = strchr(dir.c_str(), ':');
        if (!colon) return;
        std::string devname(dir.c_str(), colon - dir.c_str());
        bool devOk = false;
        for (int j = 0; j < STD_MAX; j++) {
            if (!devoptab_list[j] || !devoptab_list[j]->name) continue;
            if (devname == devoptab_list[j]->name && devoptab_list[j]->mkdir_r) {
                devOk = true; break;
            }
        }
        if (!devOk) return;
        // Walk each path component and mkdir incrementally
        std::string cur;
        const char* p = dir.c_str();
        while (*p) {
            const char* slash = strchr(p + 1, '/');
            if (slash) {
                cur.assign(dir.c_str(), slash);
                mkdir(cur.c_str(), 0777); /* ignore errors (EEXIST ok) */
                p = slash;
            } else {
                mkdir(dir.c_str(), 0777);
                break;
            }
        }
    };

    settingsPath = "";
    for (int i = 0; i < 7; i++) {
        if (!probes[i]) continue;
        // Create parent directory tree before probing
        {
            std::string p(probes[i]);
            size_t slash = p.rfind('/');
            if (slash != std::string::npos && slash > 0)
                mkdirp(p.substr(0, slash));
        }
        errno = 0;
        FILE* f = fopen(probes[i], "a");
        if (f) { fclose(f); settingsPath = probes[i]; break; }
    }

    loadSettings();
    MusicBGM::init(musicEnabled);
    SoundFX::init();
}

/* -----------------------------------------------------------------------
 * doShowHomeOverlay — full-screen Wii-style HOME menu with confirmation popup.
 * Can be called from App::loop() (normal UI) or from runPlaySession() (after
 * the player stops with PLAYER_STOP_HOME) so both contexts share identical UX.
 * Returns true if the user wants to exit (Wii Menu or Reset).
 * Sets s_restartApp = true when "Reset" is chosen.
 * Pauses BGM on entry; resumes only if musicEnabled is true (pass false
 * when calling from the player, since BGM is already paused for playback).
 * ----------------------------------------------------------------------- */
static bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                               GRRLIB_texImg* cursorPointerTex, bool musicEnabled)
{
    ir_t ir;
    ir.valid = false;
        int   hmSel     = 0;    /* 0 = Wii Menu, 1 = Reset */
        int   state     = 0;    /* 0 = home menu, 1 = confirm popup */
        int   confirmFor = 0;   /* which button triggered the popup */

        /* Main button layout */
        const int BTN_W = 230, BTN_H = 80;
        const int BTN_Y = 195;
        const int B0X   =  65;
        const int B1X   = 345;
        float bcx[2] = { B0X + BTN_W * 0.5f, B1X + BTN_W * 0.5f };
        float bcy    = BTN_Y + BTN_H * 0.5f;

        /* Confirmation dialog layout — centred on 640×480 */
        const int DW = 370, DH = 218;
        const int DX = (640 - DW) / 2;       /* 135 */
        const int DY = (480 - DH) / 2;       /* 131 */
        const int PBW = 150, PBH = 56;
        const int PBY = DY + DH - PBH - 20;
        const int PB0X = DX + 24;
        const int PB1X = DX + DW - PBW - 24;
        float pbcx[2] = { PB0X + PBW * 0.5f, PB1X + PBW * 0.5f };
        float pbcy    = PBY + PBH * 0.5f;

        /* Animation state */
        float openAnim      = 0.0f;
        float hoverSc[2]    = { 1.0f, 1.0f };
        float popAnim       = 0.0f;
        int   popSel        = 1;             /* default: "No" */
        float popHoverSc[2] = { 1.0f, 1.0f };

        /* Hover-change trackers for Select sound */
        int prevHoverMain = -1;
        int prevHoverPop  = -1;

        MusicBGM::pause();
        SoundFX::play(SoundFX::FX::MenuEnter);

        while (true) {
            Input::update();
            WPAD_IR(WPAD_CHAN_0, &ir);
            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
            if (g_app_powerOff || g_app_reset) return true;

            float irX = ir.valid ? ir.x : -1.f;
            float irY = ir.valid ? ir.y : -1.f;

            /* Per-frame hover results (reset each frame) */
            int hover    = -1;
            int popHover = -1;

            /* ---- Input ---- */
            if (state == 0) {
                if (Input::isBPressed()) { if (musicEnabled) MusicBGM::resume(); return false; }

                if (ir.valid) {
                    for (int i = 0; i < 2; i++) {
                        float hw = BTN_W * hoverSc[i] * 0.5f;
                        float hh = BTN_H * hoverSc[i] * 0.5f;
                        if (irX >= bcx[i] - hw && irX <= bcx[i] + hw &&
                            irY >= bcy    - hh && irY <= bcy    + hh)
                            hover = i;
                    }
                }
                /* Select sound: fire once when IR cursor first enters a button */
                if (hover >= 0 && hover != prevHoverMain)
                    SoundFX::play(SoundFX::FX::Select);
                prevHoverMain = hover;

                if (Input::isAJustPressed()) {
                    SoundFX::play(SoundFX::FX::Start); /* clicking Wii Menu / Reset */
                    confirmFor = (hover >= 0) ? hover : hmSel;
                    state      = 1;
                    popAnim    = 0.0f;
                    popSel     = 1;
                }
                if (Input::isLeftPressed()  || Input::isUpPressed())   hmSel = 0;
                if (Input::isRightPressed() || Input::isDownPressed())  hmSel = 1;
                for (int i = 0; i < 2; i++) {
                    float t = (hover == i) ? 1.10f : 1.0f;
                    hoverSc[i] += (t - hoverSc[i]) * 0.18f;
                }
            } else {
                /* popup state */
                if (Input::isBPressed()) { state = 0; }
                if (ir.valid) {
                    for (int i = 0; i < 2; i++) {
                        if (irX >= pbcx[i] - PBW * 0.5f && irX <= pbcx[i] + PBW * 0.5f &&
                            irY >= pbcy    - PBH * 0.5f && irY <= pbcy    + PBH * 0.5f)
                            popHover = i;
                    }
                }
                /* Select sound: fire once when IR cursor first enters a popup button */
                if (popHover >= 0 && popHover != prevHoverPop)
                    SoundFX::play(SoundFX::FX::Select);
                prevHoverPop = popHover;

                if (Input::isAJustPressed()) {
                    int sel = (popHover >= 0) ? popHover : popSel;
                    if (sel == 0) { /* Yes */
                        SoundFX::play(SoundFX::FX::MenuExit);
                        SoundFX::waitDone(SoundFX::FX::MenuExit);
                        if (confirmFor == 1) s_restartApp = true;
                        return true;
                    } else {        /* No */
                        SoundFX::play(SoundFX::FX::Back);
                        state = 0;
                    }
                }
                if (Input::isLeftPressed()  || Input::isUpPressed())   popSel = 0;
                if (Input::isRightPressed() || Input::isDownPressed())  popSel = 1;
                for (int i = 0; i < 2; i++) {
                    float t = (popHover == i) ? 1.08f : 1.0f;
                    popHoverSc[i] += (t - popHoverSc[i]) * 0.18f;
                }
            }

            /* ---- Update animations ---- */
            if (openAnim < 1.0f) openAnim += 0.075f;
            if (openAnim > 1.0f) openAnim = 1.0f;
            float oc = openAnim * openAnim * (3.0f - 2.0f * openAnim); /* smoothstep */

            if (state == 1) {
                if (popAnim < 1.0f) popAnim += 0.12f;
                if (popAnim > 1.0f) popAnim = 1.0f;
            }
            float pc = popAnim * popAnim * (3.0f - 2.0f * popAnim);

            u8  fa     = (u8)(oc * 255);
            int slideH = (int)((1.0f - oc) * 50);
            int slideF = (int)((1.0f - oc) * 50);

            auto CA = [&](u32 col) -> u32 {
                return (col & 0xFFFFFF00u) | (u8)((col & 0xFF) * oc);
            };

            /* ---- Draw HOME menu ---- */
            {
                const int BANDS = 20, BH = 480 / BANDS;
                for (int i = 0; i < BANDS; i++) {
                    float t  = i / (float)(BANDS - 1);
                    int rc = (int)(0x08 + (0x04 - 0x08) * t);
                    int gc = (int)(0x0D + (0x06 - 0x0D) * t);
                    int bc = (int)(0x28 + (0x18 - 0x28) * t);
                    GRRLIB_Rectangle(0, i * BH, 640, BH + 1,
                        ((u32)rc<<24)|((u32)gc<<16)|((u32)bc<<8)|fa, 1);
                }
            }
            for (int y = 0; y < 480; y += 4)
                GRRLIB_Rectangle(0, y, 640, 1, CA(0x00000020), 1);

            GRRLIB_Rectangle(0,  0 - slideH, 640, 62, CA(0x0A1840FF), 1);
            GRRLIB_Rectangle(0, 61 - slideH, 640,  2, CA(0x2255AAFF), 1);
            GRRLIB_Rectangle(0, 438 + slideF, 640,  2, CA(0x2255AAFF), 1);
            GRRLIB_Rectangle(0, 440 + slideF, 640, 40, CA(0x0A1840FF), 1);

            if (font && fa > 8) {
                int ty0 = 18 - slideH;
                if (ty0 >= 0) {
                    GRRLIB_PrintfTTF(28, ty0, font, "HOME Menu", 26,
                                     0xFFFFFF00u | fa);
                    const char* close = "B: Close";
                    int cw = GRRLIB_WidthTTF(font, close, 15);
                    GRRLIB_PrintfTTF(640 - cw - 22, 23 - slideH, font, close, 15,
                                     0x8899BB00u | fa);
                }
                int ty1 = 453 + slideF;
                if (ty1 < 480) {
                    const char* hint = "A: Confirm     B: Close";
                    int hw = GRRLIB_WidthTTF(font, hint, 13);
                    GRRLIB_PrintfTTF((640 - hw) / 2, ty1, font, hint, 13,
                                     0x6677AA00u | fa);
                }
            }

            if (btnTex) {
                for (int i = 0; i < 2; i++) {
                    float sc  = hoverSc[i];
                    float dw  = BTN_W * sc;
                    float dh  = BTN_H * sc;
                    float tsx = dw / btnTex->w;
                    float tsy = dh / btnTex->h;
                    int   dx  = (int)(bcx[i] - dw * 0.5f);
                    int   dy  = (int)(bcy    - dh * 0.5f);
                    bool  sel = (hover == i || hmSel == i);

                    u8 shA = (u8)(fa * 0.45f);
                    GRRLIB_DrawImg(dx + 7, dy + 7, btnTex, 0, tsx, tsy,
                                   0x00000000u | shA);
                    u32 tint = sel ? 0xFFFFFFu : 0xE8F0F8u;
                    GRRLIB_DrawImg(dx, dy, btnTex, 0, tsx, tsy, (tint << 8) | fa);

                    if (font && fa > 8) {
                        const char* lbl = (i == 0) ? "Wii Menu" : "Reset";
                        int fs = (int)(22 * sc);
                        if (fs < 12) fs = 12;
                        int tw = GRRLIB_WidthTTF(font, lbl, fs);
                        int tx = (int)(bcx[i]) - tw / 2;
                        int ty = (int)(bcy)    - fs / 2 - 1;
                        u32 lc = sel ? 0x0D1B3Eu : 0x2A4070u;
                        GRRLIB_PrintfTTF(tx, ty, font, lbl, fs, (lc << 8) | fa);
                    }
                }
            }

            /* ---- Draw confirmation popup (state == 1) ---- */
            if (state == 1 && pc > 0.01f) {
                u8 da = (u8)(pc * 255);

                /* Dark scrim */
                GRRLIB_Rectangle(0, 0, 640, 480,
                                 0x00000000u | (u8)(pc * 150), 1);

                /* Pop-in: scale from 0.82 → 1.0 around screen centre */
                float psc = 0.82f + 0.18f * pc;
                int adw = (int)(DW * psc), adh = (int)(DH * psc);
                int adx = 320 - adw / 2,   ady = 240 - adh / 2;

                /* Border then white fill */
                GRRLIB_Rectangle(adx - 2, ady - 2, adw + 4, adh + 4,
                                 (0xAABBCCu << 8) | da, 1);
                GRRLIB_Rectangle(adx, ady, adw, adh,
                                 (0xF2F4F8u << 8) | da, 1);

                /* Question text */
                if (font && da > 8) {
                    const char* line1 = (confirmFor == 0)
                        ? "Return to the Wii Menu?"
                        : "Reset the application?";
                    const char* line2 = "(Anything not saved will be lost.)";
                    int l1w = GRRLIB_WidthTTF(font, line1, 20);
                    int l2w = GRRLIB_WidthTTF(font, line2, 14);
                    GRRLIB_PrintfTTF(320 - l1w / 2, ady + 32, font, line1, 20,
                                     (0x222244u << 8) | da);
                    GRRLIB_PrintfTTF(320 - l2w / 2, ady + 60, font, line2, 14,
                                     (0x667799u << 8) | da);
                }

                /* Yes / No buttons */
                if (btnTex) {
                    for (int i = 0; i < 2; i++) {
                        float bsc = popHoverSc[i] * psc;
                        float bdw = PBW * bsc, bdh = PBH * bsc;
                        float tsx = bdw / btnTex->w;
                        float tsy = bdh / btnTex->h;
                        /* Scale positions relative to screen centre */
                        float relX = pbcx[i] - 320.0f;
                        float relY = pbcy    - 240.0f;
                        int   bdx  = 320 + (int)(relX * psc) - (int)(bdw * 0.5f);
                        int   bdy  = 240 + (int)(relY * psc) - (int)(bdh * 0.5f);
                        bool  bsel = (popHover == i || popSel == i);

                        u8 shA = (u8)(da * 0.38f);
                        GRRLIB_DrawImg(bdx + 5, bdy + 5, btnTex, 0, tsx, tsy,
                                       0x00000000u | shA);
                        u32 tint = bsel ? 0xFFFFFFu : 0xDDEEF8u;
                        GRRLIB_DrawImg(bdx, bdy, btnTex, 0, tsx, tsy,
                                       (tint << 8) | da);

                        if (font && da > 8) {
                            const char* lbl = (i == 0) ? "Yes" : "No";
                            int fs = (int)(19 * psc);
                            if (fs < 10) fs = 10;
                            int tw = GRRLIB_WidthTTF(font, lbl, fs);
                            int lx = bdx + ((int)(PBW * psc) - tw) / 2;
                            int ly = bdy + ((int)(PBH * psc) - fs) / 2 - 1;
                            u32 lc = bsel ? 0x0D1B3Eu : 0x2A4070u;
                            GRRLIB_PrintfTTF(lx, ly, font, lbl, fs, (lc << 8) | da);
                        }
                    }
                }
            }

            /* IR cursor — always on top */
            if (ir.valid && cursorPointerTex)
                GRRLIB_DrawImg((int)irX - 10, (int)irY - 4,
                               cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);

            GRRLIB_Render();
        }
}

void App::loop() {
    ir_t ir;
    int  selectedIndex = 0;
    bool irMode        = false;
    int  prevIrBtn     = -1;  /* last button index hovered via IR; -1 = none */
    const int MENU_COUNT = 3;
    const std::string menuItems[] = {
        "Connect To Jellyfin",
        "Settings",
        "Exit"
    };

    /* Thin wrapper so existing call sites don't need to change. */
    auto showHomeOverlay = [&]() -> bool {
        return doShowHomeOverlay(font, btnTex, cursorPointerTex, musicEnabled);
    };

    /* ---- Helper: launch LibraryView for a saved profile ---- */
    auto runLibraryWithProfile = [&](const SavedProfile& p) {
        JellyfinAuth auth;
        auth.userId      = p.userId;
        auth.accessToken = p.accessToken;
        auth.serverName  = p.serverName;
        if (!jellyfinClient.initNetwork()) return; /* DNS won't work without this */
        LibraryView lv(font, jpFont, cursorPointerTex, ringTex,
                       jellyfinClient, auth, p.serverUrl);
        for (;;) {
            while (true) {
                Input::update();
                WPAD_IR(WPAD_CHAN_0, &ir);
                if (g_app_powerOff || g_app_reset) { running = false; break; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                if (lv.update(ir)) break;
                lv.render(ir);
                GRRLIB_Render();
            }
            if (!running) return;
            if (lv.pendingPlayIsMusic) {
                lv.pendingPlayIsMusic = false;
                SoundFX::play(SoundFX::FX::Start);
                MusicPlayerView mpv(font, jellyfinClient, auth, p.serverUrl);
                mpv.setCursorTex(cursorPointerTex);
                mpv.setTracks(lv.pendingMusicTracks, lv.pendingMusicTrackIdx);
                bool wantsExit = mpv.run();
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                SoundFX::play(SoundFX::FX::Back);
                {
                    GRRLIB_texImg* tmpRing = GRRLIB_LoadTexture(data_ring_png);
                    for (int _fi = 0; _fi < 2; ++_fi) {
                        GRRLIB_FillScreen(0x0A1628FF);
                        if (tmpRing) {
                            GRRLIB_SetMidHandle(tmpRing, true);
                            GRRLIB_DrawImg(320, 240, tmpRing, 0, 1.0f, 1.0f, 0xFFFFFFFF);
                            GRRLIB_SetMidHandle(tmpRing, false);
                        }
                        GRRLIB_Render();
                    }
                    GRRLIB_FreeTexture(tmpRing);
                }
                reloadAssets();
                if (wantsExit) { running = false; return; }
                lv.reinitAfterPlayback(font, jpFont, cursorPointerTex, ringTex);
            } else if (!lv.pendingPlayUrl.empty()) {
                SYS_Report("[DBG] runLibrary: entering runPlaySession\n");
                lv.releaseForPlayback();
                bool wantsExit = runPlaySession(jellyfinClient, auth, p.serverUrl, lv);
                SYS_Report("[DBG] runLibrary: runPlaySession returned wantsExit=%d\n", (int)wantsExit);
                reloadAssets();
                /* Ensure ASND is alive after returning from the player.
                 * Full stop + init from scratch — avoids stale audio state
                 * left behind by ao_gekko / AESND after MPlayer exits. */
                if (!MusicBGM::isRunning()) {
                    SYS_Report("[DBG] runLibrary: stop+init (BGM not running)\n");
                    MusicBGM::stop();
                    MusicBGM::init(false);
                } else {
                    SYS_Report("[DBG] runLibrary: skip reinit (BGM already running)\n");
                }
                SYS_Report("[DBG] runLibrary: reloadAssets done\n");
                if (wantsExit) { running = false; return; }
                lv.reinitAfterPlayback(font, jpFont, cursorPointerTex, ringTex);
            } else {
                break; // user navigated back (B from libraries grid) — no play requested
            }
        }
    };

    /* ---- Helper: open ConnectView, on success add/update profile + library ---- */
    auto runConnect = [&]() {
        ConnectView cv(btnTex, cursorPointerTex, font, jellyfinClient);
        ConnectResult res = ConnectResult::None;
        while (res == ConnectResult::None && running) {
            Input::update();
            WPAD_IR(WPAD_CHAN_0, &ir);
            if (g_app_powerOff || g_app_reset) { running = false; break; }
            if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
            res = cv.update(ir);
            cv.render(ir);
            GRRLIB_Render();
        }
        if (res == ConnectResult::Success) {
            SavedProfile p;
            p.serverUrl   = cv.serverUrl;
            p.username    = cv.username;
            p.serverName  = cv.auth.serverName;
            p.userId      = cv.auth.userId;
            p.accessToken = cv.auth.accessToken;
            /* Update existing profile if same userId (token refresh/re-auth), otherwise append.
             * Only deduplicate when both sides have a known userId — if userId is empty
             * (should never happen) always append to avoid silently overwriting profiles. */
            bool found = false;
            if (!p.userId.empty()) {
                for (auto& existing : profiles) {
                    if (!existing.userId.empty() && existing.userId == p.userId) {
                        existing = p; found = true; break;
                    }
                }
            }
            if (!found) profiles.push_back(p);
            saveSettings();
            runLibraryWithProfile(p);
        }
    };

    /* ---- Helper: profile picker loop ---- */
    auto runProfilePicker = [&]() {
        while (running) {
            if (profiles.empty()) {
                runConnect();
                return;
            }
            ProfileView pv(font, cursorPointerTex, profiles);
            ProfileResult res = ProfileResult::None;
            while (res == ProfileResult::None && running) {
                Input::update();
                WPAD_IR(WPAD_CHAN_0, &ir);
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; return; }
                res = pv.update(ir);
                pv.render(ir);
                GRRLIB_Render();
            }
            if (!running || res == ProfileResult::Back) return;
            if (res == ProfileResult::DeleteOne) {
                int idx = pv.selectedIdx;
                if (idx >= 0 && idx < (int)profiles.size()) {
                    profiles.erase(profiles.begin() + idx);
                    saveSettings();
                }
                continue;
            }
            if (res == ProfileResult::AddNew) { runConnect(); continue; }
            if (res == ProfileResult::Selected) {
                runLibraryWithProfile(profiles[pv.selectedIdx]);
                continue; /* re-show picker on return */
            }
        }
    };

    while (running) {
        Input::update();   // calls WPAD_ScanPads() internally
        WPAD_IR(WPAD_CHAN_0, &ir);

        // --- Input: D-pad navigation ---
        if (g_app_reset) running = false;
        else if (Input::isHomePressed() && showHomeOverlay()) running = false;
        if (g_app_powerOff) running = false;
        if (ir.valid) irMode = true;
        if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + MENU_COUNT) % MENU_COUNT; irMode = false; }
        if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % MENU_COUNT; irMode = false; }

        // --- IR hover updates selection (no action yet) ---
        bool irHovered = false;
        if (ir.valid) {
            for (int i = 0; i < MENU_COUNT; ++i) {
                int by = BY_START + i * B_SPACING;
                if (ir.x >= BX && ir.x <= BX + BW &&
                    ir.y >= by && ir.y <= by + BH) {
                    selectedIndex = i;
                    irHovered = true;
                    irMode = true;
                    break;
                }
            }
        }

        // --- Select sound: fire once when IR cursor first enters a button ---
        {
            int curIrBtn = irHovered ? selectedIndex : -1;
            if (curIrBtn >= 0 && curIrBtn != prevIrBtn)
                SoundFX::play(SoundFX::FX::Select);
            prevIrBtn = curIrBtn;
        }

        // --- Single action dispatch on A press ---
        if (Input::isAJustPressed() && (irHovered || (!ir.valid && !irMode))) {
            SoundFX::play(SoundFX::FX::Start);
            switch (selectedIndex) {
                case 0: {
                    runProfilePicker();
                    break;
                }
                case 1: {
                    // Launch Settings view
                    SettingsView sv(btnTex, font, jellyfinClient, musicEnabled);
                    while (true) {
                        Input::update();
                        WPAD_IR(WPAD_CHAN_0, &ir);
                        if (g_app_powerOff || g_app_reset) { running = false; break; }
                        if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                        if (sv.update(ir)) break;
                        sv.render(ir);
                        if (ir.valid && cursorPointerTex) {
                            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
                            GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
                        }
                        GRRLIB_Render();
                    }
                    saveSettings();
                    MusicBGM::setEnabled(musicEnabled);
                    break;
                }
                case 2: running = false; break;
            }
        }

        // ===================== RENDER =====================

        // --- Background: vertical gradient #0E2440 -> #1A3A60 (clearly dark navy, not black) ---
        {
            const int r1 = 0x0E, g1 = 0x24, b1 = 0x40;
            const int r2 = 0x1A, g2 = 0x3A, b2 = 0x60;
            const int bands = 16;
            const int bh    = 480 / bands;  // 30px per band
            for (int i = 0; i < bands; i++) {
                float t  = i / (float)(bands - 1);
                int rc   = r1 + (int)((r2 - r1) * t);
                int gc   = g1 + (int)((g2 - g1) * t);
                int bc   = b1 + (int)((b2 - b1) * t);
                u32 col  = ((u32)rc << 24) | ((u32)gc << 16) | ((u32)bc << 8) | 0xFF;
                GRRLIB_Rectangle(0, i * bh, 640, bh, col, 1);
            }
        }

        // --- Logo: centered horizontally, y=18, scaled to 60% ---
        if (logoTex) {
            float ls = 0.60f;
            int lw = (int)(logoTex->w * ls);
            int lx = (640 - lw) / 2;
            GRRLIB_DrawImg(lx, 18, logoTex, 0, ls, ls, 0xFFFFFFFF);
        }

        // --- Menu buttons ---
        for (int i = 0; i < MENU_COUNT; ++i) {
            int byCtr = BY_START + i * B_SPACING + BH / 2;  // vertical center

            bool isSelected = (i == selectedIndex);
            bool isHover    = ir.valid &&
                              ir.x >= BX && ir.x <= BX + BW &&
                              ir.y >= (BY_START + i * B_SPACING) &&
                              ir.y <= (BY_START + i * B_SPACING + BH);

            // Zoom only on IR hover, no tint change ever
            float zoom = isHover ? 1.06f : 1.0f;
            int drawW  = (int)(BW * zoom);
            int drawH  = (int)(BH * zoom);
            int drawX  = (640 / 2) - drawW / 2;
            int drawY  = byCtr - drawH / 2;

            if (btnTex) {
                float sx = (float)drawW / (float)btnTex->w;
                float sy = (float)drawH / (float)btnTex->h;
                GRRLIB_DrawImg(drawX, drawY, btnTex, 0, sx, sy, 0xFFFFFFFF);
            } else {
                // Fallback button: filled rect (dark navy tint) with bright border
                u32 fillCol   = isSelected ? 0x1A4A8AFF : 0x0D2A50FF;
                u32 borderCol = isSelected ? 0x4A90D9FF : 0x2A5A90FF;
                GRRLIB_Rectangle(drawX,     drawY,     drawW,     drawH,     fillCol,   1);
                GRRLIB_Rectangle(drawX,     drawY,     drawW,     2,         borderCol, 1);
                GRRLIB_Rectangle(drawX,     drawY+drawH-2, drawW, 2,         borderCol, 1);
                GRRLIB_Rectangle(drawX,     drawY,     2,         drawH,     borderCol, 1);
                GRRLIB_Rectangle(drawX+drawW-2, drawY, 2,         drawH,     borderCol, 1);
            }

            // Text: white on dark button bg (readable on both texture and fallback rect)
            const char* label    = menuItems[i].c_str();
            const int   fontSize = 20;
            int tw = GRRLIB_WidthTTF(font, label, fontSize);
            int tx = (640 / 2) - tw / 2;
            int ty = byCtr - fontSize / 2;
            // Dark text on white button texture; bright text on fallback dark rect
            u32 textColor = btnTex
                ? (isSelected ? (u32)0x003A80FF : (u32)0x1A3A5AFF)
                : (isSelected ? (u32)0xFFFFFFFF : (u32)0xCCDDEEFF);
            GRRLIB_PrintfTTF(tx, ty, font, label, fontSize, textColor);
        }

        // --- Footer ---
        {
            const char* footer   = "A Select  B Back  HOME Menu";
            const int   fSize    = 16;
            int fw = GRRLIB_WidthTTF(font, footer, fSize);
            int fx = (640 - fw) / 2;
            GRRLIB_PrintfTTF(fx, 455, font, footer, fSize, 0xAAAAAAFF);
        }

        // --- IR Cursor: rendered last ---
        if (ir.valid && cursorPointerTex) {
            orient_t orient;
            WPAD_Orientation(WPAD_CHAN_0, &orient);
            GRRLIB_DrawImg(
                (int)ir.x - 20,
                (int)ir.y - 4,
                cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
        }

        GRRLIB_Render();
    }

    // --- Cleanup ---
    saveSettings();
    MusicBGM::pause();   // stop audio thread/ASND callbacks before tearing down GX
    // Blank the VI output before GX teardown to avoid purple/pink artefact frame
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    GRRLIB_FreeTexture(logoTex);
    GRRLIB_FreeTexture(btnTex);
    GRRLIB_FreeTexture(cursorPointerTex);
    GRRLIB_FreeTexture(ringTex);
    GRRLIB_FreeTTF(font);
    GRRLIB_FreeTTF(jpFont);
    GRRLIB_Exit();
    WPAD_Shutdown();
    if (g_app_powerOff)        SYS_ResetSystem(SYS_POWEROFF,    0, 0);
    else if (s_restartApp) exit(0);  // HBC catches exit(0) and reloads the app
    else {
        /* Restore the NAND-loader stub bytes at their installed VA (0x80804000)
         * before returning to the System Menu.  The stub was zeroed at startup
         * by the DOL BSS initialiser (BSS spans 0x806f7e0c–0x80ae64dc, which
         * includes the stub zone) and may have been overwritten again by
         * MPlayer's stream-cache buffer during playback.  Without this restore
         * SYS_RETURNTOMENU results in a DSI exception in the return trampoline. */
        {
            extern char __stub_zone_start[], __stub_zone_end[];
            size_t sz = (size_t)(__stub_zone_end - __stub_zone_start);
            if (sz > 0) {
                memcpy((void*)0x80804000u, __stub_zone_start, sz);
                DCFlushRange((void*)0x80804000u, sz);
            }
        }
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }
}

void App::run() {
    loop();
}

void App::loadSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "r");
    if (!f) return;

    profiles.clear();
    int profileCount = 0;
    SavedProfile legacyProfile;
    bool hasLegacy = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "ssl_verify") == 0) {
            jellyfinClient.sslVerify = atoi(val) != 0;
        } else if (strcmp(key, "music_enabled") == 0) {
            musicEnabled = atoi(val) != 0;
        } else if (strcmp(key, "profile_count") == 0) {
            profileCount = atoi(val);
            if (profileCount > 0 && profileCount <= 32) profiles.resize((size_t)profileCount);
        } else if (strncmp(key, "profile.", 8) == 0) {
            /* profile.N.field=value */
            int idx = atoi(key + 8);
            if (idx < 0 || idx >= (int)profiles.size()) continue;
            const char* dot = strchr(key + 8, '.');
            if (!dot) continue;
            const char* field = dot + 1;
            if (strcmp(field, "server_url")   == 0) {
                profiles[idx].serverUrl = val;
                while (profiles[idx].serverUrl.size() > 1 && profiles[idx].serverUrl.back() == '/')
                    profiles[idx].serverUrl.pop_back();
            }
            if (strcmp(field, "username")      == 0) profiles[idx].username    = val;
            if (strcmp(field, "server_name")   == 0) profiles[idx].serverName  = val;
            if (strcmp(field, "user_id")       == 0) profiles[idx].userId      = val;
            if (strcmp(field, "access_token")  == 0) profiles[idx].accessToken = val;
        } else {
            /* Legacy single-profile keys — migrate on first load */
            if (strcmp(key, "server_url")   == 0) {
                legacyProfile.serverUrl = val;
                while (legacyProfile.serverUrl.size() > 1 && legacyProfile.serverUrl.back() == '/')
                    legacyProfile.serverUrl.pop_back();
                hasLegacy = true;
            }
            if (strcmp(key, "username")      == 0) { legacyProfile.username    = val; }
            if (strcmp(key, "user_id")       == 0) { legacyProfile.userId      = val; }
            if (strcmp(key, "access_token")  == 0) { legacyProfile.accessToken = val; }
            if (strcmp(key, "server_name")   == 0) { legacyProfile.serverName  = val; }
        }
    }
    fclose(f);

    /* Migrate legacy single-profile format (no profile_count key present) */
    if (hasLegacy && profiles.empty() && !legacyProfile.accessToken.empty())
        profiles.push_back(legacyProfile);
}

static std::string sanitizeConfigValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\n' && c != '\r') out += c;
    return out;
}

void App::saveSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "w");
    if (!f) return;
    fprintf(f, "ssl_verify=%d\n",      jellyfinClient.sslVerify ? 1 : 0);
    fprintf(f, "music_enabled=%d\n",    musicEnabled ? 1 : 0);
    fprintf(f, "profile_count=%d\n",   (int)profiles.size());
    for (int i = 0; i < (int)profiles.size(); i++) {
        const SavedProfile& p = profiles[i];
        fprintf(f, "profile.%d.server_url=%s\n",  i, sanitizeConfigValue(p.serverUrl).c_str());
        fprintf(f, "profile.%d.username=%s\n",     i, sanitizeConfigValue(p.username).c_str());
        fprintf(f, "profile.%d.server_name=%s\n",  i, sanitizeConfigValue(p.serverName).c_str());
        fprintf(f, "profile.%d.user_id=%s\n",      i, sanitizeConfigValue(p.userId).c_str());
        fprintf(f, "profile.%d.access_token=%s\n", i, sanitizeConfigValue(p.accessToken).c_str());
    }
    fflush(f);
    fclose(f);
}
