#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QMutex>
#include <QWaitCondition>

#include <deque>

// Entries kept for frames still on their way to being presented. Far more than the
// decoder's frame pool, so one is never evicted before the pacer asks for it.
#define DECODE_TIMER_MAX_ENTRIES 32

// Notes when the GPU finishes decoding each frame.
//
// A hardware decoder hands a frame over once its decode is submitted, often well before
// the GPU has finished it, so timing the decode to the hand-over leaves the GPU's part
// out. A fence or semaphore only says whether the GPU has finished, not when, so a
// thread of its own waits for each frame's mark in turn and notes the time it wakes.
//
// Frames are looked up by their AVFrame, which only needs to stay the same from the
// decoder thread handing the frame over to the pacer presenting it.
class DecodeTimer
{
public:
    explicit DecodeTimer(IFFmpegRenderer* renderer);

    // Stops the thread. Must be done before the renderer or the decoder free anything a
    // mark waits on.
    ~DecodeTimer();

    // Starts timing a frame's decode, if the renderer can mark its end. Called on the
    // decoder thread as the frame comes out of the decoder.
    void track(AVFrame* frame);

    // When the GPU finished decoding a frame, on the LiGetMicroseconds() clock: nowUs if
    // it hasn't yet, or 0 if that isn't known. Called on the thread that renders.
    int64_t completionUs(const AVFrame* frame, int64_t nowUs);

private:
    struct Entry {
        const AVFrame* frame;
        DECODE_MARK mark;
        uint64_t seq;
        int64_t doneUs;
    };

    static int threadProc(void* context);

    void run();

    Entry* findEntry(uint64_t seq);

    IFFmpegRenderer* m_Renderer;
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
