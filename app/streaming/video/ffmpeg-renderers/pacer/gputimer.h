#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QMutex>
#include <QWaitCondition>

#include <deque>

// Entries kept for frames still on their way to being presented. Far more than the
// decoder's frame pool, so one is never evicted before the pacer asks for it.
#define GPU_TIMER_MAX_ENTRIES 32

// Notes when the GPU reaches marks the renderer made in its work for each frame, such as
// the end of a frame's decode or of its drawing.
//
// A hardware decoder hands a frame over once its decode is submitted, often well before
// the GPU has finished it, and without frame pacing Present() returns before the GPU has
// drawn the frame. Timing either to when the CPU is done leaves the GPU's part out. A
// fence or semaphore only says whether the GPU has finished, not when, so a thread of
// its own waits for each mark in turn and notes the time it wakes.
//
// Each mark gets a ticket. Frames can also be looked up by their AVFrame, which only
// needs to stay the same from the frame being tracked to it being looked up.
class GpuTimer
{
public:
    // name is for the thread and the log
    GpuTimer(IFFmpegRenderer* renderer, const char* name);

    // Stops the thread. Must be done before the renderer or the decoder free anything a
    // mark waits on.
    ~GpuTimer();

    // Starts timing the GPU work up to a mark, for a frame, and returns a ticket for it.
    // Marks must be tracked in the order the GPU reaches them. With no mark, only forgets
    // any earlier frame in the same AVFrame, so it isn't taken for this one, and returns 0.
    uint64_t track(const AVFrame* frame, const GPU_MARK* mark);

    // The ticket of the latest frame tracked in this AVFrame, 0 if there is none
    uint64_t ticketFor(const AVFrame* frame);

    // When the GPU reached a ticket's mark, on the LiGetMicroseconds() clock: 0 if that
    // hasn't been noted yet, or -1 if it never will be. Never waits.
    int64_t noteUs(uint64_t ticket);

    // When the GPU reached a ticket's mark: nowUs if it hasn't yet, or 0 if that isn't
    // known. Where the GPU has but the thread hasn't noted it yet, waits for it to.
    int64_t completionUs(uint64_t ticket, int64_t nowUs);

private:
    struct Entry {
        const AVFrame* frame;
        GPU_MARK mark;
        uint64_t seq;
        int64_t doneUs;
    };

    static int threadProc(void* context);

    void run();

    Entry* findEntry(uint64_t seq);

    IFFmpegRenderer* m_Renderer;
    const char* m_Name;
    SDL_Thread* m_Thread;

    QMutex m_Lock;
    QWaitCondition m_EntryAdded;
    QWaitCondition m_EntryDone;
    std::deque<Entry> m_Entries;
    uint64_t m_NextSeq;
    uint64_t m_NextToWait;
    bool m_Stopping;
    bool m_Failed;
};
