// Link-time fix for libogc wait-queue slot exhaustion.
//
// libogc 3.1's LWP compatibility layer hands out wait queues from a fixed
// pool of 64 slots (a u64 bitmask), and only LWP_CloseQueue() returns one.
// GX_Init() and VIDEO_Init() each do LWP_InitQueue() on a static handle
// EVERY time they're called, without closing the previous queue -- and
// WiiFin re-runs both several times per playback session (MPlayer's
// mpgxInit, then GRRLIB_Init after playback). After roughly 60 of those the
// pool is empty: LWP_InitQueue() returns LWP_TQUEUE_NULL, LWP_ThreadSleep()
// on it returns -1 immediately, and GX_WaitDrawDone()/GX_DrawDone() -- which
// loop on it with interrupts disabled -- spin forever. Hard lockup: no
// thread switches, no interrupts, nothing logs. Found via Dolphin's GDB stub
// (_gxwaitfinish == 0xFFFFFFFF, lwpc_queue_used == 0x7fffffffffffffff).
//
// The Makefile links with -Wl,--wrap=LWP_InitQueue,--wrap=LWP_CloseQueue,
// redirecting every call (including those inside libogc itself) here:
//   - if the same handle variable was given a queue by an earlier init and
//     still holds it (and it was never closed), keep using it instead of
//     allocating another;
//   - otherwise allocate normally.
// LWP_CloseQueue is wrapped only to remember which handles were closed, so a
// stale closed handle left in a static is never mistaken for a live one.

#include <ogc/lwp.h>
#include <ogc/system.h>
#include <stdint.h>

extern "C" {
s32  __real_LWP_InitQueue(lwpq_t* thequeue);
void __real_LWP_CloseQueue(lwpq_t thequeue);
s32  __wrap_LWP_InitQueue(lwpq_t* thequeue);
void __wrap_LWP_CloseQueue(lwpq_t thequeue);
}

namespace {
    const lwpq_t kMaxHandle = 64;          // handles are slot+1, slots 0..63
    volatile uint64_t s_closed = 0;        // bit (handle-1) set = closed
    volatile unsigned s_reused = 0;

    // Handle variables this wrapper has filled in, and the handle each got.
    // Reuse requires an exact match: same variable, still holding the handle
    // we gave it, never closed. Checking only "does *q look like a live
    // handle" is NOT enough -- on real hardware RAM is not zeroed at boot
    // (it holds whatever the Homebrew Channel left there), so an
    // uninitialized lwpq_t in a heap/stack struct can hold a small number
    // that looks like a live handle, and we'd hand back someone else's
    // queue. Dolphin zeroes RAM, which hid this: the first version of this
    // file ran fine there and crashed at boot on a real Wii.
    struct Owner { lwpq_t* var; lwpq_t handle; };
    Owner s_owners[kMaxHandle];            // indexed by handle-1

    bool validHandle(lwpq_t q) { return q >= 1 && q <= kMaxHandle; }

    bool ownedLive(lwpq_t* var) {
        lwpq_t q = *var;
        return validHandle(q) && !(s_closed & (1ull << (q - 1))) &&
               s_owners[q - 1].var == var && s_owners[q - 1].handle == q;
    }
}

s32 __wrap_LWP_InitQueue(lwpq_t* thequeue)
{
    if (thequeue && ownedLive(thequeue)) {
        s_reused = s_reused + 1;
        return 0; // this same variable got this queue from an earlier init -- reuse it
    }
    s32 r = __real_LWP_InitQueue(thequeue);
    if (r == 0 && thequeue && validHandle(*thequeue)) {
        lwpq_t q = *thequeue;
        s_closed &= ~(1ull << (q - 1));
        s_owners[q - 1].var    = thequeue;
        s_owners[q - 1].handle = q;
    } else {
        SYS_Report("[LwpQueueFix] LWP_InitQueue FAILED (pool exhausted?) caller=%p\n",
                   __builtin_return_address(0));
    }
    return r;
}

void __wrap_LWP_CloseQueue(lwpq_t thequeue)
{
    if (validHandle(thequeue)) {
        s_closed |= (1ull << (thequeue - 1));
        s_owners[thequeue - 1].var = nullptr;
    }
    __real_LWP_CloseQueue(thequeue);
}

// For diagnostics (MemStats-style logging): how many re-inits were absorbed.
unsigned LwpQueueFix_ReusedCount() { return s_reused; }
