#include "BackgroundImageLoader.h"
#include "../jellyfin/JellyfinClient.h"
#include <unistd.h>     // usleep
#include <ogc/system.h> // SYS_Report

// Defined in LibraryView.cpp -- pure CPU/heap JPEG decode, no GX calls beyond
// GRRLIB_SetHandle/GRRLIB_FlushTex (a cache flush, not a draw call), already
// proven safe to run off the main thread by the existing runWithLoading() path.
extern GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size);

void BackgroundImageLoader::init(JellyfinClient* client, const std::string* serverUrl,
                                  const JellyfinAuth* auth) {
    client_    = client;
    serverUrl_ = serverUrl;
    auth_      = auth;
    stop_      = false;
    stack_     = new u8[STACK_SIZE];
    LWP_CreateThread(&thread_, workerTrampoline, this, stack_, STACK_SIZE, 64);
    running_ = true;
}

void BackgroundImageLoader::shutdown() {
    if (!running_) return;
    stop_ = true;
    LWP_JoinThread(thread_, nullptr);
    running_ = false;
    delete[] stack_;
    stack_ = nullptr;
    for (int i = 0; i < CAPACITY; i++) {
        if (slots_[i].tex) { GRRLIB_FreeTexture(slots_[i].tex); slots_[i].tex = nullptr; }
        slots_[i].state = SlotState::Empty;
    }
}

void BackgroundImageLoader::clearStaleSlots() {
    for (int i = 0; i < CAPACITY; i++) {
        if (slots_[i].state == SlotState::Pending) {
            slots_[i].state = SlotState::Empty;
        } else if (slots_[i].state == SlotState::Done) {
            if (slots_[i].tex) { GRRLIB_FreeTexture(slots_[i].tex); slots_[i].tex = nullptr; }
            slots_[i].state = SlotState::Empty;
        }
        // Fetching slots are left alone -- the worker notices the generation
        // bump itself and discards the result rather than marking it Done.
    }
}

void BackgroundImageLoader::submitBatch(const Request* requests, int count) {
    generation_ = generation_ + 1;
    int gen = generation_;
    clearStaleSlots();
    int r = 0;
    for (int i = 0; i < CAPACITY && r < count; i++) {
        if (slots_[i].state == SlotState::Empty) {
            slots_[i].req        = requests[r];
            slots_[i].generation = gen;
            slots_[i].state      = SlotState::Pending;
            r++;
        }
    }
    SYS_Report("[bgload] submitBatch count=%d placed=%d gen=%d\n", count, r, gen);
}

void BackgroundImageLoader::cancelAll() {
    generation_ = generation_ + 1;
    clearStaleSlots();
    SYS_Report("[bgload] cancelAll gen=%d\n", generation_);
}

bool BackgroundImageLoader::isDone(int targetIndex) const {
    for (int i = 0; i < CAPACITY; i++)
        if (slots_[i].state == SlotState::Done && slots_[i].req.targetIndex == targetIndex)
            return true;
    return false;
}

GRRLIB_texImg* BackgroundImageLoader::takeResult(int targetIndex) {
    for (int i = 0; i < CAPACITY; i++) {
        if (slots_[i].state == SlotState::Done && slots_[i].req.targetIndex == targetIndex) {
            GRRLIB_texImg* t = slots_[i].tex;
            slots_[i].tex   = nullptr;
            slots_[i].state = SlotState::Empty;
            return t;
        }
    }
    return nullptr;
}

void* BackgroundImageLoader::workerTrampoline(void* arg) {
    static_cast<BackgroundImageLoader*>(arg)->workerLoop();
    return nullptr;
}

void BackgroundImageLoader::workerLoop() {
    while (!stop_) {
        int idx = -1;
        for (int i = 0; i < CAPACITY; i++) {
            if (slots_[i].state == SlotState::Pending) { idx = i; break; }
        }
        if (idx < 0) { usleep(20000); continue; }

        Slot& s = slots_[idx];
        s.state = SlotState::Fetching;
        int myGen = s.generation;

        SYS_Report("[bgload] fetching itemId=%s size=%dx%d gen=%d\n",
                   s.req.itemId.c_str(), s.req.width, s.req.height, myGen);

        std::string bytes;
        bool ok = client_->getItemImageBytes(*serverUrl_, *auth_, s.req.itemId,
                                             s.req.width, s.req.height, bytes);
        GRRLIB_texImg* tex = nullptr;
        if (ok && !bytes.empty())
            tex = loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());

        if (myGen == generation_) {
            s.tex   = tex;
            s.state = SlotState::Done;
            SYS_Report("[bgload] done itemId=%s tex=%p gen=%d\n",
                       s.req.itemId.c_str(), (void*)tex, myGen);
        } else {
            if (tex) GRRLIB_FreeTexture(tex);
            s.state = SlotState::Empty;
            SYS_Report("[bgload] discarded (stale) itemId=%s myGen=%d curGen=%d\n",
                       s.req.itemId.c_str(), myGen, generation_);
        }
    }
}
