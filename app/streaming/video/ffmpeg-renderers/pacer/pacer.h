#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "pacertrace.h"
#include "gputimer.h"

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

// Draw times remembered when choosing how early to start drawing
#define PACER_CADENCE_DRAW_SAMPLES 64

// Times from a frame's last packet to its being ready to draw, remembered when choosing
// when a frame still arriving is cut short
#define PACER_CADENCE_READY_SAMPLES 64

// Presented frames whose statistics may wait for the GPU to finish them. Past this, the
// oldest is counted with whatever is known.
#define PACER_MAX_PENDING_STATS 8

// A presented frame, kept until the GPU has finished it so its statistics can run to
// then. See finishFrame().
typedef struct _PACER_PENDING_STATS {
    uint64_t decodeTicket;      // For the decode timer, 0 if none
    uint64_t drawTicket;        // For the draw timer, 0 if none
    int64_t handedOverUs;       // When the decoder handed it over
    int64_t firstPacketUs;      // When its first packet arrived, 0 if unknown
    int64_t lastPacketUs;       // When its last packet arrived
    int64_t renderStartUs;      // When drawing started
    int64_t drawEndUs;          // When drawing finished on the CPU
    int64_t presentStartUs;     // When Present() was called
    int64_t presentUs;          // When Present() returned
    int64_t hostUs;             // Host capture time (host clock), 0 if none
    uint16_t hostLatency;       // See ML_FRAME_HOST_LATENCY()
    uint32_t markerFlip;        // The latency marker flip it shows, 0 if the marker is off
    uint64_t markerFlipUs;      // When that flip happened
    int markerTestPress;        // The latency test press that made that flip, 0 if none
} PACER_PENDING_STATS;

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
    Pacer(IFFmpegRenderer* renderer, GpuTimer* decodeTimer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(PDECODER_PARAMETERS params, bool enablePacing);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame, int64_t handedOverUs);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    static int cadenceThread(void* context);

    int64_t scheduleFrame(AVFrame* frame, PPACER_TRACE_ROW row);

    void waitUntilUs(int64_t targetUs);

    void presentAt(AVFrame* frame, int64_t targetUs, PPACER_TRACE_ROW row);

    void finishFrame(AVFrame* frame, int64_t handedOverUs, int64_t renderStartUs, int64_t drawEndUs,
                     int64_t presentStartUs, int64_t presentUs);

    void updateOverlaysForDraw();

    void countPendingStats();

    void countFrameStats(const PACER_PENDING_STATS& stats, int64_t gpuDecodedUs, int64_t gpuDrawnUs);

    void updatePartialDeadline(int64_t hostUs, bool paced);

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
    GpuTimer* m_DecodeTimer;
    GpuTimer* m_DrawTimer;
    QQueue<PACER_PENDING_STATS> m_PendingStats;
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
    bool m_TrackerEnabled;
    double m_TrackerAlpha;
    double m_TrackerBeta;
    double m_TrackerResyncUs;
    int m_ArrivalPercentile;
    double m_NoTearFraction;
    int m_TearGuardUs;
    bool m_DrawAheadEnabled;

    // What has been learned about the host's cadence
    int m_LearnFramesLeft;
    int64_t m_LastHostUs;
    double m_IntervalUs;
    double m_SmoothedUs;
    bool m_TrackerOutlier;
    int64_t m_HostIntervalsUs[PACER_CADENCE_INTERVAL_SAMPLES];
    int m_HostIntervalCount;
    int m_NextHostInterval;
    int64_t m_TransitUs[PACER_CADENCE_TRANSIT_SAMPLES];
    int m_TransitCount;
    int m_NextTransit;
    int m_TimelineArrivals;
    double m_DelayUs;
    bool m_Tearing;
    bool m_CanTear;
    int64_t m_LastTearSwitchUs;
    double m_RenderCostUs;
    uint32_t m_DrawCostsUs[PACER_CADENCE_DRAW_SAMPLES];
    int m_DrawCostCount;
    int m_NextDrawCost;
    double m_PresentCostUs;
    double m_DrawMarginUs;
    double m_DrawLeadUs;
    int m_SpinUs;

    // Host timestamp steps. See scheduleFrame().
    bool m_HostStepsEnabled;
    bool m_HostStepActive;
    int64_t m_HostStepStartUs;
    double m_HostStepLatenessUs;
    int64_t m_LastArrivalUs;
    double m_LastPacedHoldUs;

    // Frames cut short at their deadline. See updatePartialDeadline().
    bool m_PartialEnabled;
    int m_PartialMinPercent;
    int m_PartialMarginUs;
    uint32_t m_ReadyCostsUs[PACER_CADENCE_READY_SAMPLES];
    int m_ReadyCostCount;
    int m_NextReadyCost;
    bool m_PartialDeadlineSet;

    // Frames held up in the display's queue. See presentAt().
    bool m_QueueDrainEnabled;
    int m_BacklogFrames;
    bool m_DrainNext;
    int64_t m_LastDrainUs;
    int64_t m_LastPresentUs;

    // Trace bookkeeping
    uint64_t m_FrameIndex;
    uint32_t m_DroppedSinceRow;
    uint32_t m_DrainedSinceRow;
    uint32_t m_EvictedFrames;

    // The latency marker flip in the frame being drawn, and the latest one logged. See
    // countFrameStats().
    uint32_t m_DrawnMarkerFlip;
    uint64_t m_DrawnMarkerFlipUs;
    int m_DrawnMarkerTestPress;
    uint32_t m_LoggedMarkerFlip;
};
