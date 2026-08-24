#pragma once
#include <grrlib.h>
#include <string>
#include <deque>

/* On-screen + on-disk debug log.
 *
 * Every Log() call always updates two in-memory buffers (cheap, no I/O):
 *   - a small HUD ring (7 lines) drawn bottom-right by Render()
 *   - a larger timestamped History() (capped) for the in-app log viewer
 *
 * Writing to disk is a separate, opt-in concern (see SetFileLoggingEnabled)
 * because open/write/close on every line is real SD wear over a long
 * session. When enabled, new lines queue in memory and get flushed to disk
 * in one batched append every few seconds (or on Flush()), not one file
 * operation per line. */
namespace DebugLog {
    void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    /* Draws the HUD ring bottom-right of a 640x480 GRRLIB screen. Safe to
     * call every frame from any screen's render(); a no-op when there's
     * nothing logged yet. */
    void Render(GRRLIB_ttfFont* font);

    /* Sets the on-disk log path (e.g. "sd:/apps/WiiFin/wiifin_log.txt") and
     * truncates it -- one fresh file per app launch, not appended across
     * sessions. Call once at startup once the cache/settings device is
     * known. Does not itself enable file writing (see below); safe to call
     * even if file logging is off, so the path is ready if the user
     * enables it mid-session. */
    void Init(const std::string& path);

    /* Gate for actually touching the SD card. Off by default. Flipping
     * false->true does not retroactively write anything already logged
     * this session -- only new Log() calls from that point on. */
    void SetFileLoggingEnabled(bool enabled);
    bool IsFileLoggingEnabled();

    /* Writes any lines queued since the last flush in one batched
     * fopen/fwrite/fclose. Log() already calls this periodically on its
     * own (a few times a minute) when file logging is enabled; call
     * explicitly on shutdown (power off / reset / HBC return) so nothing
     * queued is lost. No-op when file logging is off or nothing is queued. */
    void Flush();

    /* Full in-session history (oldest first, each line already prefixed
     * with a "[mm:ss.mmm]" timestamp), independent of file logging -- for
     * the in-app log viewer. Capped; oldest entries drop silently once full. */
    const std::deque<std::string>& History();
}
