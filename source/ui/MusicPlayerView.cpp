/*
 * MusicPlayerView.cpp — music player HUD + session management for WiiFin.
 *
 * GRRLIB stays active throughout audio playback.  MPlayer CE runs with
 * -vo null (audio-only) and never touches GX, so the bgThread can safely
 * render the animated music HUD via MusicOverlay::renderFrameGRRLIB()
 * at ~60 Hz.  Between tracks the bgThread has been joined; the main
 * thread pushes a static "Loading…" frame before doing network I/O.
 *
 * Controls (Wii Remote horizontal):
 *   A          — toggle play / pause
 *   ← / →      — seek −10 s / +10 s
 *   + (PLUS)   — next track
 *   − (MINUS)  — previous track
 *   HOME / B   — stop playback, return to library
 */

#include "MusicPlayerView.h"
#include "../player/WiiPlayer.h"
#include "../core/MusicBGM.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <unistd.h>
#include <ogcsys.h>
#include <ogc/lwp.h>           /* LWP_CreateThread / LWP_JoinThread */
#include <ogc/lwp_watchdog.h>  /* gettime(), ticks_to_millisecs()   */
#include <ogc/video.h>         /* VIDEO_WaitVSync                   */
#include <wiiuse/wpad.h>
#include <jpeglib.h>

/* Global power/reset flags (defined in App.cpp) */
extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

/* -----------------------------------------------------------------------
 * Deferred reportPlaybackStart — the callback g_stream_opened_cb fires
 * from MPlayer's main thread the instant the HTTP stream is opened.
 * Doing HTTP I/O there corrupts MPlayer's state, so we just set a flag
 * and let the bgThread (MusicOverlay::bgTick) send the report.
 * This way the Jellyfin transcode is already running when the report
 * arrives, so the dashboard correctly shows "Transcoding".
 * ----------------------------------------------------------------------- */
static volatile bool  s_reportNeeded = false;
static volatile bool  s_startReported = false;  /* true once reportPlaybackStart succeeded */
static JellyfinClient* s_reportClient   = nullptr;
static std::string     s_reportServer;
static JellyfinAuth    s_reportAuth;
static std::string     s_reportItemId;
static std::string     s_reportSessionId;
static uint32_t        s_lastProgressTick = 0;  /* for periodic progress reports */

/* -----------------------------------------------------------------------
 * Async reporter — runs HTTP progress/start reports on a dedicated LWP
 * thread so that the bgThread render loop is never blocked by network I/O.
 * ----------------------------------------------------------------------- */
static volatile bool      s_asyncStartPending = false; /* queue a reportPlaybackStart  */
static volatile bool      s_asyncProgPending  = false; /* queue a reportPlaybackProgress */
static volatile long long s_asyncProgTicks    = 0;
static volatile int       s_asyncProgPaused   = 0;
static volatile bool      s_asyncStop         = false;
static lwp_t              s_asyncThread       = LWP_THREAD_NULL;
/* httpsRequest alone uses ~8 KB of stack (mbedTLS contexts + buffers); give
 * enough room for the full call chain: asyncReporterFunc → report* → https. */
static uint8_t            s_asyncStack[32 * 1024] __attribute__((aligned(32)));
/* Hovered transport button: -1=Prev, 0=None, 1=Play/Pause, 2=Next.
 * Written by renderFrameGRRLIB (bgThread), read by bgTick (same thread). */
static volatile int       s_btnHovered = 0;

static void* asyncReporterFunc(void*)
{
    while (!s_asyncStop) {
        if (s_asyncStartPending && s_reportClient) {
            s_reportClient->reportPlaybackStart(s_reportServer, s_reportAuth,
                                                s_reportItemId, s_reportItemId,
                                                s_reportSessionId);
            s_asyncStartPending = false;
            s_startReported     = true;
            s_lastProgressTick  = (uint32_t)ticks_to_millisecs(gettime());
        }
        if (s_asyncProgPending && s_reportClient) {
            long long ticks  = s_asyncProgTicks;
            bool      isPaused = (s_asyncProgPaused != 0);
            s_asyncProgPending = false;   /* clear before HTTP so next can queue */
            s_reportClient->reportPlaybackProgress(
                s_reportServer, s_reportAuth,
                s_reportItemId, s_reportItemId,
                s_reportSessionId, ticks, isPaused);
        }
        usleep(16000); /* ~60 Hz poll — yields CPU between checks */
    }
    return nullptr;
}

/* Gate flag — prevents bgThread from calling GRRLIB while mplayer_main's
 * common init path is still touching GX.  Set to true once g_stream_opened_cb
 * fires (init is complete, stream is playing). */
static volatile bool s_renderEnabled = false;

static void onAudioStreamOpened()
{
    s_reportNeeded  = true;
    s_renderEnabled = true;
    /* mplayer_main's common init calls VIDEO_SetBlack(TRUE) to blank the
     * screen during startup.  With -vo gx the GX driver unblanks it, but
     * with -vo null nobody does.  Unblank here so our GRRLIB frames are
     * actually visible on screen. */
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
}

/* -----------------------------------------------------------------------
 * Singleton
 * ----------------------------------------------------------------------- */
MusicOverlay* MusicOverlay::instance  = nullptr;
GRRLIB_ttfFont* MusicOverlay::renderFont = nullptr;
GRRLIB_texImg*  MusicOverlay::renderCursorTex = nullptr;
GRRLIB_texImg*  MusicOverlay::renderArtTex    = nullptr;

/* -----------------------------------------------------------------------
 * loadJPEGTexture — decode a JPEG image buffer into a GRRLIB texture.
 * Returns nullptr on any error (corrupt data, alloc failure, etc.)
 * Thread-safe: only called from the main thread in MusicPlayerView::run().
 * ----------------------------------------------------------------------- */
struct MpvJpegErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               buf;
};
static void mpvJpegErrExit(j_common_ptr cinfo) {
    longjmp(((MpvJpegErrMgr*)cinfo->err)->buf, 1);
}
static void mpvJpegNoOp(j_common_ptr) {}

static GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size)
{
    if (size < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
        return nullptr;

    struct jpeg_decompress_struct cinfo __attribute__((aligned(32)));
    MpvJpegErrMgr jerr __attribute__((aligned(32)));
    unsigned char* strip = nullptr;
    GRRLIB_texImg* tex   = nullptr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = mpvJpegErrExit;
    jerr.pub.output_message = mpvJpegNoOp;

    if (setjmp(jerr.buf)) {
        jpeg_destroy_decompress(&cinfo);
        free(strip);
        if (tex) { free(tex->data); free(tex); }
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    cinfo.progress = nullptr;
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    u32 w  = cinfo.output_width;
    u32 h  = cinfo.output_height;
    u32 nc = (u32)cinfo.output_components;
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || nc != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    tex = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!tex) { jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr; }

    u32 bufsize = GX_GetTexBufferSize(w, h, GX_TF_RGBA8, 0, 0);
    tex->data = memalign(32, bufsize);
    if (!tex->data) {
        free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr;
    }

    strip = (unsigned char*)malloc(w * 4 * nc);
    if (!strip) {
        free(tex->data); free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr;
    }

    u8* tileData = (u8*)tex->data;
    for (u32 by = 0; by < h; by += 4) {
        int nrows = (int)(h - by);
        if (nrows > 4) nrows = 4;
        JSAMPROW rp[4];
        for (int i = 0; i < 4; i++)
            rp[i] = strip + (u32)i * w * nc;
        int done = 0;
        while (done < nrows && cinfo.output_scanline < h)
            done += (int)jpeg_read_scanlines(&cinfo, rp + done, (JDIMENSION)(nrows - done));
        for (u32 bx = 0; bx < w; bx += 4) {
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8 red = (sx < w && r < (u8)nrows) ? strip[((u32)r * w + sx) * nc] : 0;
                    *tileData++ = 0xFF;
                    *tileData++ = red;
                }
            }
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8 g = 0, b = 0;
                    if (sx < w && r < (u8)nrows) {
                        g = strip[((u32)r * w + sx) * nc + 1];
                        b = strip[((u32)r * w + sx) * nc + 2];
                    }
                    *tileData++ = g;
                    *tileData++ = b;
                }
            }
        }
    }

    free(strip);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    tex->w      = w;
    tex->h      = h;
    tex->format = GX_TF_RGBA8;
    GRRLIB_SetHandle(tex, 0, 0);
    GRRLIB_FlushTex(tex);
    return tex;
}

/* (g_stream_opened_cb sets s_reportNeeded; bgTick queues to async reporter\n * thread which calls reportPlaybackStart without blocking the render loop.) */

/* -----------------------------------------------------------------------
 * Constructor
 * ----------------------------------------------------------------------- */
MusicOverlay::MusicOverlay(const std::vector<Track>& t, int idx)
    : tracks(t), currentIdx(idx < (int)t.size() ? idx : 0)
{
    for (int i = 0; i < VIZ_BARS; i++) { vizHeight[i] = 0.0f; vizTarget[i] = 0.0f; }
    instance = this;
}

/* -----------------------------------------------------------------------
 * Timing helpers
 * ----------------------------------------------------------------------- */
void MusicOverlay::resetTiming(float startSecs)
{
    startTick      = 0;
    seekBase       = startSecs;
    totalPauseSecs = 0.0f;
    wasPaused      = false;
    pausedAtTick   = 0;
    timingActive   = false;  /* defer: timer starts when g_mplayer_duration>0 */
}

void MusicOverlay::notifySeek(float targetSecs)
{
    startTick      = gettime();
    seekBase       = targetSecs;
    totalPauseSecs = 0.0f;
    timingActive   = true;  /* audio is already running when a seek happens */
    /* preserve pause state */
    if (wasPaused) pausedAtTick = startTick;
}

float MusicOverlay::getPosition() const
{
    /* Lazy-start: don't count time until MPlayer's main loop has begun
     * (g_mplayer_duration > 0), which is the first moment audio is actually
     * being decoded/played — avoids counting the ~3 s demuxer init phase. */
    if (!timingActive) {
        if (g_mplayer_duration <= 0.0f)
            return seekBase;
        /* First call after audio begins — arm the wall-clock now. */
        const_cast<MusicOverlay*>(this)->startTick    = gettime();
        const_cast<MusicOverlay*>(this)->timingActive = true;
    }
    uint64_t now   = gettime();
    float elapsed  = (float)ticks_to_millisecs(now - startTick) / 1000.0f;
    float paused   = totalPauseSecs;
    if (wasPaused && pausedAtTick >= startTick)
        paused += (float)ticks_to_millisecs(now - pausedAtTick) / 1000.0f;
    float pos = seekBase + elapsed - paused;
    if (pos < 0.0f) pos = 0.0f;
    return pos;
}

float MusicOverlay::getDuration() const
{
    float d = (float)g_mplayer_duration;
    if (d <= 0.0f && !tracks.empty() && currentIdx < (int)tracks.size())
        d = (float)(tracks[currentIdx].runtimeTicks / 10000000LL);
    return d;
}

/* -----------------------------------------------------------------------
 * bgTick — called ~60 Hz from the background LWP thread.
 * Handles button input, pause-time tracking, periodic progress reports,
 * and power/reset detection.
 * ----------------------------------------------------------------------- */
void MusicOverlay::bgTick(int paused, uint32_t btnsDown, uint32_t btnsHeld)
{
    MusicOverlay* mo = instance;
    if (!mo) return;

    /* ---- Power / reset detection: stop playback so cleanup runs ------- */
    if (g_app_powerOff || g_app_reset) {
        mo->requestStop = true;
        wii_player_stop();
    }

    /* ---- Deferred playback-start report ---------------------------------
     * Hand off to the async reporter thread to avoid blocking the render loop. */
    if (s_reportNeeded && s_reportClient) {
        s_asyncStartPending = true;
        s_reportNeeded      = false;
    }

    /* ---- Periodic progress report (~every 10 s) -----------------------
     * Queue to async reporter thread; do NOT call HTTP here. */
    if (s_startReported && s_reportClient && !s_asyncProgPending) {
        uint32_t now = (uint32_t)ticks_to_millisecs(gettime());
        if (now - s_lastProgressTick >= 10000) {
            s_lastProgressTick  = now;
            s_asyncProgTicks    = (long long)(mo->getPosition() * 10000000.0f);
            s_asyncProgPaused   = paused;
            s_asyncProgPending  = true;
        }
    }

    /* ---- Pause-time tracking ------------------------------------------ */
    bool nowPaused = (paused != 0);
    if (nowPaused && !mo->wasPaused) {
        mo->pausedAtTick = gettime();
        mo->wasPaused    = true;
    } else if (!nowPaused && mo->wasPaused) {
        uint64_t now = gettime();
        mo->totalPauseSecs += (float)ticks_to_millisecs(now - mo->pausedAtTick) / 1000.0f;
        mo->wasPaused        = false;
        mo->pausedAtTick     = 0;
    }

    /* ---- Auto-advance at end of track ---------------------------------- */
    /* MPlayer uses -demuxer audio for non-seekable HTTP streams and can't
     * determine duration from the stream headers (reports -5.4 "unknown").
     * When the HTTP response ends MPlayer's cache empties and it hangs for
     * up to ~60 s waiting for more data.  Detect EOF proactively via
     * g_mplayer_time_pos vs. the Jellyfin-known duration (runtimeTicks)
     * and stop MPlayer immediately so the next track starts without delay. */
    if (!nowPaused && !mo->requestStop && mo->pendingNextIdx < 0) {
        float dur = mo->getDuration();
        if (dur > 0.0f && g_mplayer_time_pos > 2.0f && g_mplayer_time_pos >= dur - 1.0f) {
            int n = (int)mo->tracks.size();
            if (mo->currentIdx + 1 < n)
                mo->pendingNextIdx = mo->currentIdx + 1;
            else
                mo->requestStop = true;
            wii_player_stop();
        }
    }

    /* ---- Button handling ---------------------------------------------- */
    /* A: action depends on which transport button the IR is hovering over. */
    if (btnsDown & WPAD_BUTTON_A) {
        int hov = s_btnHovered;
        if (hov == -1) {
            /* Prev button */
            if (mo->currentIdx > 0)
                mo->pendingNextIdx = mo->currentIdx - 1;
            else
                mo->pendingNextIdx = mo->currentIdx;
            wii_player_stop();
        } else if (hov == 2) {
            /* Next button */
            int n = (int)mo->tracks.size();
            if (n > 0 && mo->currentIdx + 1 < n) {
                mo->pendingNextIdx = mo->currentIdx + 1;
                wii_player_stop();
            }
        } else {
            /* Play/Pause button (hov==1) or no button hovered */
            wii_player_pause_toggle();
        }
    }

    /* LEFT / RIGHT: seek -10s / +10s */
    if (btnsDown & WPAD_BUTTON_LEFT) {
        float pos = mo->getPosition() - 10.0f;
        if (pos < 0.0f) pos = 0.0f;
        wii_player_seek_abs(pos);
        mo->notifySeek(pos);
    }
    if (btnsDown & WPAD_BUTTON_RIGHT) {
        float dur = mo->getDuration();
        float pos = mo->getPosition() + 10.0f;
        if (dur > 0.0f && pos > dur) pos = dur;
        wii_player_seek_abs(pos);
        mo->notifySeek(pos);
    }

    /* + : next track */
    if (btnsDown & WPAD_BUTTON_PLUS) {
        int n = (int)mo->tracks.size();
        if (n > 0 && mo->currentIdx + 1 < n) {
            mo->pendingNextIdx = mo->currentIdx + 1;
            wii_player_stop();
        }
    }

    /* - : previous track */
    if (btnsDown & WPAD_BUTTON_MINUS) {
        if (mo->currentIdx > 0) {
            mo->pendingNextIdx = mo->currentIdx - 1;
        } else {
            /* restart current track */
            mo->pendingNextIdx = mo->currentIdx;
        }
        wii_player_stop();
    }

    /* HOME or B: stop and return to library */
    if ((btnsDown & WPAD_BUTTON_HOME) || (btnsDown & WPAD_BUTTON_B)) {
        mo->requestStop = true;
        wii_player_stop();
    }

    (void)btnsHeld;
}

/* -----------------------------------------------------------------------
 * updateVisualizerFrame — called each GX frame from onFrame().
 * dt: time since last frame in seconds.
 * When paused, bars gently fall toward 0.
 * When playing, targets are updated using sine waves with per-bar phases.
 * ----------------------------------------------------------------------- */
void MusicOverlay::updateVisualizerFrame(float dt)
{
    vizPhase += dt * 3.5f;   /* global phase advance rate          */
    /* Keep vizPhase bounded — prevents sinf() slow range reduction on long
     * sessions (large argument → many extra FP ops → periodic micro-freeze). */
    if (vizPhase > 1000.0f) vizPhase -= 1000.0f;

    bool playing = !(g_mplayer_paused);

    for (int i = 0; i < VIZ_BARS; i++) {
        if (playing) {
            /* Each bar has a unique frequency and phase offset */
            float freq  = 0.8f + (float)i * 0.18f;
            float phase = (float)i * 0.42f;
            float raw   = sinf(vizPhase * freq + phase) * 0.5f + 0.5f;
            /* Secondary harmonic adds texture */
            raw += sinf(vizPhase * freq * 1.7f + phase * 2.1f) * 0.2f;
            if (raw < 0.0f) raw = 0.0f;
            if (raw > 1.0f) raw = 1.0f;
            /* Bars near centre are generally taller (music "energy" shape) */
            float centre = 1.0f - fabsf((float)i - (float)(VIZ_BARS / 2)) / (float)(VIZ_BARS / 2);
            vizTarget[i] = raw * (0.3f + centre * 0.7f);
        } else {
            /* Paused: slowly drop to 0 */
            vizTarget[i] = 0.0f;
        }

        /* Smooth interpolation toward target */
        float rate = playing ? 8.0f : 3.0f;
        vizHeight[i] += (vizTarget[i] - vizHeight[i]) * rate * dt;
        if (vizHeight[i] < 0.0f) vizHeight[i] = 0.0f;
        if (vizHeight[i] > 1.0f) vizHeight[i] = 1.0f;
    }
}

/* -----------------------------------------------------------------------
 * renderFrameGRRLIB — static, called ~60 Hz from bgThread during
 * audio-only playback.  Renders the full music HUD using GRRLIB and
 * calls GRRLIB_Render() (which waits for vsync) at the end.
 *
 * GRRLIB must still be active (GRRLIB_Exit() must NOT have been called).
 * With -vo null, MPlayer's main thread never touches GX, so there is no
 * contention.
 * ----------------------------------------------------------------------- */
void MusicOverlay::renderFrameGRRLIB()
{
    MusicOverlay* mo = instance;
    if (!mo) return;

    /* Do NOT touch GRRLIB/GX until mplayer_main's init phase is done.
     * mplayer_main's common init path touches GX even with -vo null;
     * calling GRRLIB_Render() concurrently corrupts the GX FIFO and
     * produces a black screen on the second track onwards.
     * The main thread renders a static "Now Playing" screen before
     * calling wii_player_play_audio(); the Wii VI holds that frame
     * until we take over here. */
    if (!s_renderEnabled) {
        VIDEO_WaitVSync();   /* pacing (~16 ms) without touching GX */
        return;
    }

    /* Compute dt */
    uint32_t now = (uint32_t)ticks_to_millisecs(gettime());
    float dt = (mo->lastFrameTick == 0) ? 0.016f
             : (float)(now - mo->lastFrameTick) / 1000.0f;
    if (dt > 0.2f) dt = 0.033f;
    mo->lastFrameTick = now;

    mo->updateVisualizerFrame(dt);

    /* ---- Layout constants (GRRLIB framebuffer = 640×480) ----------- */
    const int W = 640, H = 480;

    /* Two-column layout:
     *   Left  — album art square (or placeholder)
     *   Right — track dots, title, artist, visualizer bars
     *   Full width — seek bar + time labels + controls hint          */
    const int ART_X   = 18;
    const int ART_Y   = 38;
    const int ART_SZ  = 210;   /* displayed as ART_SZ × ART_SZ          */
    const int RIGHT_X = ART_X + ART_SZ + 12;   /* = 240                 */
    const int RIGHT_W = W - RIGHT_X - 18;       /* = 382                 */

    /* ---- Background ----------------------------------------------- */
    GRRLIB_FillScreen(0x000000FF);

    /* ---- Track dots (top of right column, centred within it) ------- */
    {
        int n = (int)mo->tracks.size();
        if (n > 1 && n <= 20) {
            const int DOT_W = 8, DOT_GAP = 4;
            int total = n * (DOT_W + DOT_GAP) - DOT_GAP;
            int dx = RIGHT_X + (RIGHT_W - total) / 2;
            for (int i = 0; i < n; i++) {
                u32 c = (i == mo->currentIdx) ? 0xFFFFFFFF : 0x3A3A3AFF;
                GRRLIB_Rectangle((f32)(dx + i * (DOT_W + DOT_GAP)), 14.0f,
                                 (f32)DOT_W, (f32)DOT_W, c, true);
            }
        }
    }

    /* ---- Album art (left column) or dark placeholder --------------- */
    if (renderArtTex) {
        float sx = (float)ART_SZ / (float)renderArtTex->w;
        float sy = (float)ART_SZ / (float)renderArtTex->h;
        GRRLIB_DrawImg(ART_X, ART_Y, renderArtTex, 0.0f, sx, sy, 0xFFFFFFFF);
    } else {
        GRRLIB_Rectangle((f32)ART_X, (f32)ART_Y,
                         (f32)ART_SZ, (f32)ART_SZ, 0x1A1A1AFF, true);
    }

    /* ---- Title and artist (right column) --------------------------- */
    if (renderFont && !mo->tracks.empty() &&
        mo->currentIdx < (int)mo->tracks.size()) {
        const Track& tr = mo->tracks[mo->currentIdx];

        const char* title = tr.title.c_str();
        GRRLIB_PrintfTTF(RIGHT_X, ART_Y + 2, renderFont, title, 20, 0xFFFFFFFF);

        if (!tr.artist.empty()) {
            const char* artist = tr.artist.c_str();
            GRRLIB_PrintfTTF(RIGHT_X, ART_Y + 28, renderFont, artist, 14, 0x999999FF);
        }
    }

    /* ---- Visualizer bars (right column, below artist) -------------- */
    const int VIZ_BASE_Y  = ART_Y + ART_SZ;            /* = 248         */
    const int VIZ_MAX_H   = ART_SZ - 58;              /* = 152, top≈96  */
    const int BAR_W       = RIGHT_W / VIZ_BARS - 2;   /* ≈14 px @ 382  */
    const int BAR_GAP     = 2;
    const int VIZ_TOTAL_W = (BAR_W + BAR_GAP) * VIZ_BARS - BAR_GAP;
    const int VIZ_X0      = RIGHT_X + (RIGHT_W - VIZ_TOTAL_W) / 2;

    for (int i = 0; i < VIZ_BARS; i++) {
        int bx = VIZ_X0 + i * (BAR_W + BAR_GAP);
        int bh = (int)(mo->vizHeight[i] * (float)VIZ_MAX_H);
        if (bh < 2) bh = 2;
        int by = VIZ_BASE_Y - bh;

        /* Colour gradient: dark blue at bottom → cyan at top */
        float t = (float)bh / (float)VIZ_MAX_H;
        uint8_t r = (uint8_t)(t * 60);
        uint8_t g = (uint8_t)(100 + t * 155);
        uint8_t b = 255;
        u32 color = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
        GRRLIB_Rectangle((f32)bx, (f32)by, (f32)BAR_W, (f32)bh, color, true);

        /* 2-px bright peak cap */
        if (bh > 4)
            GRRLIB_Rectangle((f32)bx, (f32)(by - 3), (f32)BAR_W, 2.0f,
                             0xFFFFFFFF, true);
    }

    /* ---- Seek bar (full width, below art + viz) -------------------- */
    const int SEEK_Y  = VIZ_BASE_Y + 14;   /* = 262                     */
    const int SEEK_X0 = ART_X;
    const int SEEK_X1 = W - 18;            /* = 622                     */
    const int SEEK_H  = 5;

    GRRLIB_Rectangle((f32)SEEK_X0, (f32)SEEK_Y,
                     (f32)(SEEK_X1 - SEEK_X0), (f32)SEEK_H,
                     0x2A2A2AFF, true);

    float pos = mo->getPosition();
    const float dur = mo->getDuration();
    /* Cap displayed position at duration — wall-clock timer can overrun
     * during cache-stall at EOF before mplayer detects end-of-stream. */
    if (dur > 0.0f && pos > dur) pos = dur;

    if (dur > 0.0f) {
        float frac = pos / dur;
        if (frac > 1.0f) frac = 1.0f;
        int filled = (int)(frac * (float)(SEEK_X1 - SEEK_X0));
        if (filled > 0)
            GRRLIB_Rectangle((f32)SEEK_X0, (f32)SEEK_Y,
                             (f32)filled, (f32)SEEK_H, 0x0099CCFF, true);
        /* Scrubber knob */
        GRRLIB_Rectangle((f32)(SEEK_X0 + filled - 5), (f32)(SEEK_Y - 4),
                         10.0f, (f32)(SEEK_H + 8), 0xFFFFFFFF, true);
    }

    /* ---- Time labels ----------------------------------------------- */
    if (renderFont) {
        const int TIME_Y = SEEK_Y + SEEK_H + 5;
        char buf[16];
        int s = (int)pos;
        snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
        GRRLIB_PrintfTTF(SEEK_X0, TIME_Y, renderFont, buf, 15, 0xCCCCCCFF);

        if (dur > 0.0f) {
            s = (int)dur;
            snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
            u32 dw = GRRLIB_WidthTTF(renderFont, buf, 15);
            GRRLIB_PrintfTTF(SEEK_X1 - (int)dw, TIME_Y, renderFont, buf, 15, 0x666666FF);
        }

        /* Hint — reduced now that play/pause and track change are buttons */
        const char* hint = "</>: Seek     B: Back";
        u32 hw = GRRLIB_WidthTTF(renderFont, hint, 12);
        GRRLIB_PrintfTTF((W - (int)hw) / 2, H - 14, renderFont, hint, 12, 0x333333FF);
    }

    /* ---- Transport buttons (IR-clickable) --------------------------------
     * Three buttons: |< (Prev)  ||/>  (Play/Pause)  >| (Next)
     * Total: 3×80 + 2×20 = 280 px → starts at (640-280)/2 = 180.
     * Do NOT call WPAD_ScanPads() — see original cursor comment.
     * ----------------------------------------------------------------------- */
    {
        const int BTN_Y   = 304;
        const int BTN_H   = 52;
        const int BTN_W   = 80;
        const int BTN_GAP = 20;
        const int BTN_X0  = (W - (BTN_W * 3 + BTN_GAP * 2)) / 2;  /* = 180 */

        /* Read IR once — shared for hover detection and cursor drawing. */
        ir_t ir;
        WPAD_IR(WPAD_CHAN_0, &ir);

        int newHov = 0;
        if (ir.valid) {
            int ix = (int)ir.x, iy = (int)ir.y;
            if (iy >= BTN_Y && iy < BTN_Y + BTN_H) {
                if      (ix >= BTN_X0                        && ix < BTN_X0 + BTN_W)                       newHov = -1;
                else if (ix >= BTN_X0 + BTN_W + BTN_GAP     && ix < BTN_X0 + BTN_W * 2 + BTN_GAP)        newHov =  1;
                else if (ix >= BTN_X0 + (BTN_W + BTN_GAP)*2 && ix < BTN_X0 + BTN_W * 3 + BTN_GAP * 2)   newHov =  2;
            }
        }
        s_btnHovered = newHov;

        const bool playingNow  = !(bool)g_mplayer_paused;
        const char* btnLabels[3] = { "|<", playingNow ? "||" : ">", ">|" };
        const int   btnVals[3]   = { -1, 1, 2 };
        const int   btnX[3]      = { BTN_X0,
                                     BTN_X0 + BTN_W + BTN_GAP,
                                     BTN_X0 + (BTN_W + BTN_GAP) * 2 };

        for (int b = 0; b < 3; b++) {
            bool hov = (newHov == btnVals[b]);
            /* Border */
            GRRLIB_Rectangle((f32)(btnX[b] - 1), (f32)(BTN_Y - 1),
                             (f32)(BTN_W + 2), (f32)(BTN_H + 2),
                             hov ? 0x0099CCFF : 0x2A2A4AFF, true);
            /* Fill */
            GRRLIB_Rectangle((f32)btnX[b], (f32)BTN_Y,
                             (f32)BTN_W, (f32)BTN_H,
                             hov ? 0x003C6EFF : 0x0D0D1EFF, true);
            /* Label */
            if (renderFont) {
                u32 tw = GRRLIB_WidthTTF(renderFont, btnLabels[b], 19);
                int tx = btnX[b] + ((int)BTN_W - (int)tw) / 2;
                int ty = BTN_Y + (BTN_H - 20) / 2;
                GRRLIB_PrintfTTF(tx, ty, renderFont, btnLabels[b], 19, 0xFFFFFFFF);
            }
        }

        /* Cursor drawn on top of buttons */
        if (renderCursorTex && ir.valid) {
            orient_t orient;
            WPAD_Orientation(WPAD_CHAN_0, &orient);
            GRRLIB_DrawImg((int)ir.x - 8, (int)ir.y - 4,
                           renderCursorTex, orient.roll, 1.0f, 1.0f, 0xFFFFFFFF);
        }
    }

    VIDEO_SetBlack(FALSE);
    GRRLIB_Render();
}

/* ======================================================================= */
/*  MusicPlayerView                                                          */
/* ======================================================================= */

MusicPlayerView::MusicPlayerView(GRRLIB_ttfFont* f,
                                  JellyfinClient& c,
                                  const JellyfinAuth& a,
                                  const std::string& srv)
    : font(f), client(c), auth(a), serverUrl(srv)
{}

void MusicPlayerView::setCursorTex(GRRLIB_texImg* tex)
{
    MusicOverlay::renderCursorTex = tex;
}

void MusicPlayerView::setTracks(const std::vector<MusicOverlay::Track>& t, int idx)
{
    tracks     = t;
    currentIdx = (idx >= 0 && idx < (int)t.size()) ? idx : 0;
}

/* -----------------------------------------------------------------------
 * run — main session loop.
 *
 * GRRLIB stays active throughout.  The bgThread calls renderFrameGRRLIB()
 * ~60 Hz during playback.  Between tracks the main thread renders a
 * static "Loading…" screen so the user never sees a black gap.
 * ----------------------------------------------------------------------- */
bool MusicPlayerView::run()
{
    if (tracks.empty()) return false;

    int idx = currentIdx;

    /* Pre-fetch URL for the first track before entering the loop. */
    std::string url, sessionId;
    {
        const MusicOverlay::Track& first = tracks[idx];
        if (!client.getAudioStreamUrl(serverUrl, auth, first.id, 0, url, sessionId)) {
            SYS_Report("[MusicPlayerView] getAudioStreamUrl failed (first): %s\n",
                       client.lastError().c_str());
            return false;
        }
    }

    /* Pause background music once before entering the playback loop. */
    MusicBGM::pause();

    /* Make the font available to the bgThread render callback. */
    MusicOverlay::renderFont = font;

    for (;;) {
        if (idx < 0 || idx >= (int)tracks.size()) break;

        const MusicOverlay::Track& tr = tracks[idx];

        /* Arm overlay (handles button input + HUD rendering from bgThread) */
        MusicOverlay overlay(tracks, idx);
        overlay.resetTiming();

        /* ---- Prepare deferred playback-start report -------------------
         * g_stream_opened_cb fires from MPlayer's main thread when the
         * HTTP stream opens.  We just set a flag; bgTick (running on the
         * bgThread) will send the actual HTTP report so Jellyfin sees the
         * transcode session as already active — dashboard shows "Transcoding". */
        s_reportNeeded      = false;
        s_startReported     = false;
        s_lastProgressTick  = 0;
        s_reportClient    = &client;
        s_reportServer    = serverUrl;
        s_reportAuth      = auth;
        s_reportItemId    = tr.id;
        s_reportSessionId = sessionId;
        g_stream_opened_cb  = onAudioStreamOpened;
        s_renderEnabled     = false;

        /* ---- Load album art for this track (main thread, blocking) ---- */
        {
            if (MusicOverlay::renderArtTex) {
                GRRLIB_FreeTexture(MusicOverlay::renderArtTex);
                MusicOverlay::renderArtTex = nullptr;
            }
            std::string imgBytes;
            if (client.getItemImageBytes(serverUrl, auth, tr.id, 210, 210, imgBytes)
                    && !imgBytes.empty()) {
                MusicOverlay::renderArtTex = loadJPEGTexture(
                    (const u8*)imgBytes.data(), (u32)imgBytes.size());
            }
        }

        /* ---- Render static "Now Playing" into both XFBs ---------------
         * The bgThread will NOT touch GRRLIB until g_stream_opened_cb sets
         * s_renderEnabled (after mplayer init completes).  The Wii VI holds
         * this frame in the meantime. */
        {
            const int W = 640, H = 480;
            const int ART_X  = 18,  ART_Y  = 38,  ART_SZ = 210;
            const int RIGHT_X = ART_X + ART_SZ + 12;  /* 240 */
            const int RIGHT_W = W - RIGHT_X - 18;       /* 382 */
            for (int _fi = 0; _fi < 2; ++_fi) {
                GRRLIB_FillScreen(0x000000FF);

                /* Track dots in right column */
                if (font) {
                    int n = (int)tracks.size();
                    if (n > 1 && n <= 20) {
                        const int DOT_W = 8, DOT_GAP = 4;
                        int total = n * (DOT_W + DOT_GAP) - DOT_GAP;
                        int dx = RIGHT_X + (RIGHT_W - total) / 2;
                        for (int i = 0; i < n; i++) {
                            u32 c = (i == idx) ? 0xFFFFFFFF : 0x3A3A3AFF;
                            GRRLIB_Rectangle((f32)(dx + i * (DOT_W + DOT_GAP)), 14.0f,
                                             (f32)DOT_W, (f32)DOT_W, c, true);
                        }
                    }
                }

                /* Album art or placeholder */
                if (MusicOverlay::renderArtTex) {
                    float sx = (float)ART_SZ / (float)MusicOverlay::renderArtTex->w;
                    float sy = (float)ART_SZ / (float)MusicOverlay::renderArtTex->h;
                    GRRLIB_DrawImg(ART_X, ART_Y, MusicOverlay::renderArtTex,
                                   0.0f, sx, sy, 0xFFFFFFFF);
                } else {
                    GRRLIB_Rectangle((f32)ART_X, (f32)ART_Y,
                                     (f32)ART_SZ, (f32)ART_SZ, 0x1A1A1AFF, true);
                }

                /* Title and artist in right column */
                if (font) {
                    GRRLIB_PrintfTTF(RIGHT_X, ART_Y + 2, font,
                                     tr.title.c_str(), 20, 0xFFFFFFFF);
                    if (!tr.artist.empty())
                        GRRLIB_PrintfTTF(RIGHT_X, ART_Y + 28, font,
                                         tr.artist.c_str(), 14, 0x999999FF);

                    const char* hint = "A:Play/Pause  </> Seek  +/-:Track  B:Back";
                    u32 hw = GRRLIB_WidthTTF(font, hint, 13);
                    GRRLIB_PrintfTTF((W - (int)hw) / 2, H - 16, font, hint, 13, 0x444444FF);
                }
                GRRLIB_Render();
            }
        }

        /* ---- Set bgThread callbacks: GRRLIB HUD + button input -------- */
        wii_player_set_audio_render_cb(MusicOverlay::renderFrameGRRLIB);
        wii_player_set_music_tick(MusicOverlay::bgTick);

        /* Start async reporter thread before mplayer so it’s ready to handle
         * the deferred reportPlaybackStart without blocking the render loop. */
        s_asyncStop         = false;
        s_asyncStartPending = false;
        s_asyncProgPending  = false;
        /* Priority 30 < bgThread (40) so HTTPS never preempts the render loop.
         * CPU is yielded during vsync waits, giving the reporter enough runtime. */
        LWP_CreateThread(&s_asyncThread, asyncReporterFunc, nullptr,
                        s_asyncStack, sizeof(s_asyncStack), 30);

        /* Run playback (blocks until EOF / user stop).
         * bgThread waits (VIDEO_WaitVSync) during mplayer init, then
         * switches to animated HUD once s_renderEnabled is set.
         * Set g_wiifin_known_duration so MPlayer's main-loop hook can assign
         * g_mplayer_duration from the first iteration — that's the signal
         * getPosition() uses to start counting, avoiding the ~3 s init delay. */
        g_wiifin_known_duration = overlay.getDuration();
        wii_player_play_audio(url.c_str());
        g_wiifin_known_duration = 0.0f;

        /* --- bgThread is now joined; stop async reporter thread. --- */
        s_asyncStop = true;
        if (s_asyncThread != LWP_THREAD_NULL) {
            LWP_JoinThread(s_asyncThread, nullptr);
            s_asyncThread = LWP_THREAD_NULL;
        }
        /* Blank the display while we fill both GRRLIB framebuffers with the
         * transition spinner.  Without this, one of the XFBs may still
         * contain a stale frame from a prior video session and briefly
         * flashes through before the spinner render completes.
         * Mirror the same blank→fill→unblank pattern used by App.cpp after
         * wii_player_play() so the transition is always clean. */
        VIDEO_SetBlack(TRUE);
        VIDEO_Flush();

        /* Render a transition frame (navy bg + ring spinner) matching the
         * post-play spinner used by App.cpp so there is no abrupt colour
         * jump between the music player and the library view. */
        {
            extern unsigned char data_ring_png[];
            GRRLIB_texImg* ringTex = GRRLIB_LoadTexture(data_ring_png);
            for (int _fi = 0; _fi < 2; ++_fi) {
                GRRLIB_FillScreen(0x0A1628FF);
                if (ringTex) {
                    GRRLIB_SetMidHandle(ringTex, true);
                    GRRLIB_DrawImg(320, 240, ringTex, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF);
                    GRRLIB_SetMidHandle(ringTex, false);
                }
                GRRLIB_Render();
            }
            GRRLIB_FreeTexture(ringTex);
        }
        /* Both XFBs now contain the spinner — safe to unblank. */
        VIDEO_SetBlack(FALSE);
        VIDEO_Flush();

        /* If the deferred start report was queued but never executed
         * (e.g. track was very short), send it synchronously now. */
        if ((s_asyncStartPending || !s_startReported) && s_reportClient) {
            s_reportClient->reportPlaybackStart(s_reportServer, s_reportAuth,
                                                s_reportItemId, s_reportItemId,
                                                s_reportSessionId);
            s_asyncStartPending = false;
        }

        /* Report stopped and clean up encoding session. */
        {
            long long posTicks = (long long)(overlay.getPosition() * 10000000.0f);
            client.reportPlaybackStopped(serverUrl, auth, tr.id, tr.id, sessionId, posTicks);
            if (!sessionId.empty())
                client.deleteActiveEncoding(serverUrl, auth, sessionId);
        }

        /* Determine next track index */
        bool doStop  = overlay.requestStop;
        int  nextIdx = (overlay.pendingNextIdx >= 0) ? overlay.pendingNextIdx : idx + 1;

        /* Pre-fetch URL for the next track */
        std::string nextUrl, nextSessionId;
        bool hasNext = false;
        if (!doStop && nextIdx >= 0 && nextIdx < (int)tracks.size()) {
            hasNext = client.getAudioStreamUrl(serverUrl, auth,
                                               tracks[nextIdx].id, 0,
                                               nextUrl, nextSessionId);
            if (!hasNext)
                SYS_Report("[MusicPlayerView] getAudioStreamUrl failed (next): %s\n",
                           client.lastError().c_str());
        }

        if (doStop || !hasNext)
            break;

        /* Advance to next track using pre-fetched URL */
        idx       = nextIdx;
        url       = nextUrl;
        sessionId = nextSessionId;
    }

    g_stream_opened_cb     = nullptr;
    s_reportClient         = nullptr;
    MusicOverlay::instance = nullptr;
    wii_player_set_music_tick(nullptr);

    if (MusicOverlay::renderArtTex) {
        GRRLIB_FreeTexture(MusicOverlay::renderArtTex);
        MusicOverlay::renderArtTex = nullptr;
    }

    MusicBGM::resume();

    return false;
}
