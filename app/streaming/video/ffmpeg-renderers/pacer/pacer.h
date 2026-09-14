#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "pacertrace.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)

// Arrivals remembered when choosing how long frames wait, and host intervals
// remembered when judging the source's frame rate.
#define PACER_CADENCE_TRANSIT_SAMPLES 128
#define PACER_CADENCE_INTERVAL_SAMPLES 32

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    static int cadenceThread(void* context);

    int64_t scheduleFrame(AVFrame* frame, PPACER_TRACE_ROW row);

    void waitUntilUs(int64_t targetUs);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;

    // Pacing to the host's cadence. See scheduleFrame().
    SDL_Thread* m_CadenceThread;
    PacerTrace* m_Trace;
    void* m_WaitTimer;
    bool m_WaitTimerHighRes;

    // Tunables, read once in initialize()
    double m_SmoothGain;
    double m_SmoothMaxUs;
    int m_ArrivalPercentile;
    double m_NoTearFraction;

    // What has been learned about the host's cadence
    int m_LearnFramesLeft;
    int64_t m_LastHostUs;
    double m_IntervalUs;
    double m_SmoothedUs;
    int64_t m_HostIntervalsUs[PACER_CADENCE_INTERVAL_SAMPLES];
    int m_HostIntervalCount;
    int m_NextHostInterval;
    int64_t m_TransitUs[PACER_CADENCE_TRANSIT_SAMPLES];
    int m_TransitCount;
    int m_NextTransit;
    double m_DelayUs;
    bool m_Tearing;
    double m_RenderCostUs;

    // Trace bookkeeping
    uint64_t m_FrameIndex;
    uint32_t m_DroppedSinceRow;
    uint32_t m_EvictedFrames;
};
