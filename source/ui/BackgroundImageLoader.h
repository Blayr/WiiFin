#pragma once
#include <grrlib.h>
#include <ogc/lwp.h>
#include <string>

class JellyfinClient;
struct JellyfinAuth;

// Fetches poster images on a persistent background LWP worker thread, so the
// UI never blocks waiting on network I/O. Cancellation is generation-based:
// submitBatch()/cancelAll() bump a generation counter and immediately free any
// request that hasn't started yet; a request already mid-fetch (at most one,
// since only one JellyfinClient call may be in flight at a time -- see below)
// is left to finish naturally but its result is discarded rather than applied.
//
// JellyfinClient::httpRequest()/httpsRequest() use function-local *static*
// response buffers, so only one thread may ever call into a given
// JellyfinClient at a time. This loader is therefore the only caller that may
// use `client` while active, and it only ever has one fetch in flight itself.
//
// All cross-thread state uses plain `volatile` fields under a single-writer
// discipline (no mutex), matching this codebase's existing convention
// (MusicBGM, WiiPlayer's bgThread, MusicPlayerView's asyncReporterFunc).
class BackgroundImageLoader {
public:
    // One extra slot beyond the largest batch size (8, matching
    // LibraryView::POSTER_VISIBLE) guarantees submitBatch() can always place a
    // full new batch even while the one possibly-still-fetching old slot drains.
    static const int CAPACITY = 9;

    struct Request {
        std::string itemId;
        int width       = 0;
        int height      = 0;
        int targetIndex = 0; // which grid slot (e.g. posterTextures[i]) this belongs to
    };

    void init(JellyfinClient* client, const std::string* serverUrl, const JellyfinAuth* auth);
    void shutdown(); // stop + join the worker; call once, before the owning view is destroyed

    // Cancels everything left over from the previous batch and installs up to
    // CAPACITY-1 new requests. Main thread only.
    void submitBatch(const Request* requests, int count);

    // Cancels everything without starting a new batch. Main thread only --
    // call whenever leaving the screen that owns this loader for good.
    void cancelAll();

    // Main thread only. True once targetIndex's fetch has finished (image may
    // still be null if the fetch/decode failed -- caller should keep showing
    // its placeholder in that case).
    bool isDone(int targetIndex) const;

    // Main thread only. Transfers ownership of the decoded texture (caller
    // must eventually GRRLIB_FreeTexture it) and frees the slot. Returns
    // nullptr if the fetch failed (isDone() was still true).
    GRRLIB_texImg* takeResult(int targetIndex);

private:
    enum class SlotState { Empty, Pending, Fetching, Done };
    struct Slot {
        volatile SlotState state = SlotState::Empty;
        Request        req;
        volatile int    generation = 0;
        GRRLIB_texImg*  tex        = nullptr;
    };
    Slot slots_[CAPACITY];

    JellyfinClient*     client_    = nullptr;
    const std::string*  serverUrl_ = nullptr;
    const JellyfinAuth* auth_      = nullptr;

    volatile int  generation_ = 0;
    volatile bool running_    = false;
    volatile bool stop_       = false;
    lwp_t         thread_     = 0;
    // Heap-allocated (not an inline array member): BackgroundImageLoader
    // instances live inside LibraryView, which is itself a stack-local
    // object on the main thread. An inline stack buffer here would embed
    // this worker thread's stack inside the main thread's stack allocation
    // -- two live threads aliasing the same memory, corrupting each other.
    static const int STACK_SIZE = 64 * 1024;
    u8* stack_ = nullptr;

    void clearStaleSlots(); // shared by submitBatch()/cancelAll()

    static void* workerTrampoline(void* arg);
    void workerLoop();
};
