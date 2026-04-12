/*
 * mplayer_stubs.c — Weak stub symbols for MPlayer CE externals.
 *
 * Compiled only when libs/mplayer-ce-build/libmplayer.a is NOT present
 * (see Makefile).  Allows WiiFin to build and link without the full
 * MPlayer CE static library; playback functions will be no-ops.
 */

#include <ogc/video.h>

/* gx_supp.c globals */
GXRModeObj *vmode = NULL;
int screenwidth  = 640;
int screenheight = 480;
void (*g_wiifin_gx_overlay_cb)(void) = (void*)0;
volatile int g_wiifin_gx_active = 0;

/* mplayer.c */
volatile int async_quit_request = 0;

int mplayer_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return 0;
}

int mplayer_get_key(int fd)
{
    (void)fd;
    return -3; /* MP_INPUT_NOTHING */
}

void register_mpegts_demuxer(void) {}

/* mp_input */
struct mp_cmd_t;
struct mp_cmd_t *mp_input_parse_cmd(char *str) { (void)str; return (void*)0; }
void mp_input_queue_cmd(struct mp_cmd_t *cmd)  { (void)cmd; }

/* gx_supp.c frame pipeline */
void mpgxWaitDrawDone(void) {}
void mpgxRunOverlay(void)   {}
void mpgxPushFrame(void)    {}
