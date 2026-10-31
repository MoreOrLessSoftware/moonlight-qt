#pragma once

#include "pyrowaveapi.h"

#include <QMutex>
#include <QWaitCondition>

#include <Limelight.h>
#include "SDL_compat.h"

#include <cstdint>
#include <deque>

// Logs how long each stage of decoding a PyroWave frame takes, to find where the decode
// time goes. Enabled by ML_PYROWAVE_STAGE_LOG=1. The stages, for each frame:
// - Bitstream: from decode() being called to its packets being pushed to PyroWave
// - Submit: from there to PyroWave's decode being submitted, on the CPU
// - Vulkan: from there to the GPU finishing PyroWave's decode
//
// Like GpuTimer, a thread of its own waits for each frame's decode in turn and notes
// when it wakes.
class PyrowaveStageTimer
{
public:
    // Null unless ML_PYROWAVE_STAGE_LOG=1
    static PyrowaveStageTimer* createIfEnabled(pyrowave_result (*cpuWait)(pyrowave_sync_object, uint64_t, uint64_t));

    // Stops the thread. Must be done before the sync objects it waits on are destroyed.
    ~PyrowaveStageTimer();

    // On the decoder thread, in this order for each frame. frameEnded() drops a frame
    // whose decode wasn't submitted.
    void frameStarted();
    void framePushed();
    void decodeSubmitted(pyrowave_sync_object sync, uint64_t decodedValue);
    void frameEnded();

private:
    struct Frame {
        int64_t startUs;
        int64_t pushedUs;
        int64_t submittedUs;
        pyrowave_sync_object sync;
        uint64_t decodedValue;
    };

    struct Stage {
        int64_t totalUs;
        int64_t maxUs;
    };

    enum {
        STAGE_BITSTREAM,
        STAGE_SUBMIT,
        STAGE_VULKAN,
        STAGE_TOTAL,
        STAGE_COUNT
    };

    explicit PyrowaveStageTimer(pyrowave_result (*cpuWait)(pyrowave_sync_object, uint64_t, uint64_t));

    static int threadProc(void* context);

    void run();

    // Waits for the GPU to finish a frame's decode, on the timer's thread. Returns when
    // it did, or 0 if stopping or waiting failed.
    int64_t waitForDecode(const Frame& frame);

    void count(const Frame& frame, int64_t decodedUs);
    void countStage(int stage, int64_t us);

    pyrowave_result (*m_CpuWait)(pyrowave_sync_object, uint64_t, uint64_t);
    SDL_Thread* m_Thread;

    // Only the decoder thread touches this
    Frame m_Current;

    QMutex m_Lock;
    QWaitCondition m_FrameAdded;
    std::deque<Frame> m_Frames;
    bool m_Stopping;

    // Only the timer's thread touches these
    Stage m_Stages[STAGE_COUNT];
    int m_FrameCount;
    int64_t m_PeriodStartUs;
};
