#include "DebugLog.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ogc/lwp_watchdog.h> // gettime(), ticks_to_millisecs()

namespace {
    // HUD ring (unchanged from the original design)
    const int kHudLines = 7;
    const int kLineLen  = 40;
    char s_hudLines[kHudLines][kLineLen];
    int  s_hudCount = 0;

    // Full-session history for the in-app viewer -- capped independently of
    // whatever's queued for disk, so viewer scrollback keeps working even
    // with file logging off.
    const size_t kMaxHistory = 300; // ~300 * ~55 bytes =~ 16 KB, negligible
    std::deque<std::string> s_history;

    // Disk logging (opt-in)
    std::string s_logPath;
    bool        s_fileLoggingEnabled = false;
    std::string s_pending;              // queued text, written in one batch
    u64         s_lastFlushTicks = 0;
    const u32   kFlushIntervalMs = 20000; // batch a few times a minute, not per line
}

namespace DebugLog {

void Log(const char* fmt, ...) {
    // Full message, not truncated to the HUD's compact width -- the on-screen
    // HUD ring only ever shows kLineLen (40) chars, but the full-history
    // in-app viewer and the on-disk log have plenty of room and shouldn't
    // lose detail just because the HUD does.
    char full[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(full, sizeof(full), fmt, ap);
    va_end(ap);

    // HUD ring buffer -- deliberately truncated to kLineLen, it's a small
    // corner overlay.
    for (int i = 0; i < kHudLines - 1; i++)
        memcpy(s_hudLines[i], s_hudLines[i + 1], kLineLen);
    strncpy(s_hudLines[kHudLines - 1], full, kLineLen - 1);
    s_hudLines[kHudLines - 1][kLineLen - 1] = '\0';
    if (s_hudCount < kHudLines) s_hudCount++;

    // Timestamped history entry (relative to boot -- Wii's RTC isn't
    // trustworthy, see JellyfinClient's TLS date-check comments; relative
    // ordering/timing is what actually matters for debugging anyway).
    // Uses the full untruncated message -- this is what the in-app Log
    // Viewer and the on-disk log actually show.
    u32 ms = (u32)ticks_to_millisecs(gettime());
    char line[220];
    snprintf(line, sizeof(line), "[%02u:%02u.%03u] %s",
             (ms / 60000) % 100, (ms / 1000) % 60, ms % 1000, full);

    if (s_history.size() >= kMaxHistory) s_history.pop_front();
    s_history.push_back(line);

    if (s_fileLoggingEnabled && !s_logPath.empty()) {
        s_pending += line;
        s_pending += "\n";
        u32 sinceFlushMs = (u32)ticks_to_millisecs(gettime() - s_lastFlushTicks);
        if (sinceFlushMs >= kFlushIntervalMs) Flush();
    }
}

void Render(GRRLIB_ttfFont* font) {
    if (!font || s_hudCount == 0) return;

    const int sz    = 11;
    const int lineH = 14;
    const int pad   = 6;
    const int boxW  = 260;
    const int boxH  = s_hudCount * lineH + pad * 2;
    const int boxX  = 640 - boxW - 8;
    const int boxY  = 480 - boxH - 8;

    GRRLIB_Rectangle(boxX, boxY, boxW, boxH, 0x000000B0, 1);
    GRRLIB_Rectangle(boxX, boxY, boxW, 1, 0x44AAFF99, 1);

    int startIdx = kHudLines - s_hudCount;
    for (int i = 0; i < s_hudCount; i++)
        GRRLIB_PrintfTTF(boxX + pad, boxY + pad + i * lineH, font,
                          s_hudLines[startIdx + i], sz, 0xCCEEFFEE);
}

void Init(const std::string& path) {
    s_logPath = path;
    s_lastFlushTicks = gettime();
    if (s_logPath.empty()) return;
    // Fresh file per launch, not appended across sessions -- keeps it
    // bounded and relevant without needing rotation logic.
    FILE* f = fopen(s_logPath.c_str(), "w");
    if (f) fclose(f);
}

void SetFileLoggingEnabled(bool enabled) {
    s_fileLoggingEnabled = enabled;
}

bool IsFileLoggingEnabled() { return s_fileLoggingEnabled; }

void Flush() {
    s_lastFlushTicks = gettime();
    if (s_logPath.empty() || s_pending.empty()) return;
    FILE* f = fopen(s_logPath.c_str(), "a");
    if (f) {
        fwrite(s_pending.data(), 1, s_pending.size(), f);
        fclose(f);
    }
    s_pending.clear();
}

const std::deque<std::string>& History() { return s_history; }

} // namespace DebugLog
