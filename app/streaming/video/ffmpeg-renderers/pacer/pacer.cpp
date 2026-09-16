#include "pacer.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

// Pacing to the host's cadence. See scheduleFrame() for how these fit together.

// Frames shown as they arrive while the host's cadence is learned, at least. A
// second's worth at the stream's rate is used when that is more.
#define CADENCE_MIN_LEARN_FRAMES 30

// How much of the gap between the predicted and the actual host capture time each
// frame corrects. Lower irons out more of the host's jitter; 1 follows the host's
// timestamps exactly. ML_PACING_SMOOTH sets it as a percentage.
#define CADENCE_SMOOTH_GAIN 0.25

// A host time further from its prediction than this many intervals, or than
// CADENCE_SMOOTH_MAX_US, is a change of pace or a stall rather than jitter, and is
// followed straight away.
#define CADENCE_RESYNC_INTERVALS 0.5

// Largest prediction error smoothing irons out. The host's own jitter at a steady
// rate is a few hundred microseconds; anything bigger is the game's frame timing
// or the host's stamps and is shown as it is. Half an interval alone let smoothing
// move frames by several milliseconds at 30 FPS, which caused 375 of 449 spacing
// errors over 5 ms in one session. ML_PACING_SMOOTH_MAX_US sets it, and 0 leaves
// only the half interval.
#define CADENCE_SMOOTH_MAX_US 2000

// How quickly the tracked host interval follows a change of pace, as the share of
// each new interval taken in
#define CADENCE_INTERVAL_EMA_DIVISOR 8

// Which point in the recent spread of arrivals frames wait for, and the margin on
// top. Frames arriving later than that go out when they arrive.
// ML_PACING_PERCENTILE sets the percentile.
#define CADENCE_ARRIVAL_PERCENTILE 97
#define CADENCE_DELAY_GUARD_US 500

// Arrivals at the start of a host timeline left out of the delay. The first frame or
// two of a stream arrive late while it starts up, by 20 ms to a quarter of a second
// in five sessions measured, where every later arrival was within a couple of
// milliseconds. Counted in, they set the delay for the first paced frames, which were
// then held up to 27 ms for about a second.
#define CADENCE_STARTUP_ARRIVALS 10

// How fast the delay may move per frame. Up by at most a fixed step so late
// arrivals are covered without jumps. Down by a share of the distance still to go,
// at least a fixed step, so a delay built up in a slow scene is gone in about a
// second rather than the 10-25 s a fixed 25 us per frame took.
#define CADENCE_DELAY_RISE_US 250
#define CADENCE_DELAY_FALL_US 25
#define CADENCE_DELAY_FALL_DIVISOR 16

// Longest a frame is held beyond the fastest recent arrival, in source intervals.
// It caps the delay itself as well as its target, so when the source speeds up the
// hold shrinks with it at once. Held longer, two frames are already waiting when a
// frame comes up and it is dropped: after slow scenes a 100 FPS source was shown
// at 50 FPS for seconds at a time.
#define CADENCE_MAX_HOLD_INTERVALS 1.5

// Tearing stops once the source reaches this fraction of the display's refresh
// rate, where the display can show every frame without it, and resumes a little
// below. ML_PACING_NO_TEAR_PCT sets the fraction as a percentage.
#define CADENCE_NO_TEAR_FRACTION 0.96
#define CADENCE_TEAR_HYSTERESIS 0.02

// Least time between turning tearing on and off. With the hysteresis alone, a source
// hovering at 94-96% of a 100 Hz display switched 25 times in 11 minutes, six of them
// within 0.7 s of the switch before and the shortest after 0.22 s.
#define CADENCE_TEAR_MIN_SWITCH_US 2000000

// Longest a tearing present is held to keep it out of the previous frame's scanout.
// The display takes about one period at its top refresh rate to scan a frame out, and
// a present landing inside that window tears across the picture. Over three sessions
// about 1% of torn presents landed inside it, over half of them needing less than half
// a millisecond to clear, so a short hold removes most tearing. Frames needing longer
// are presented as they are rather than held that far, which also stops a hold from
// pushing the frames behind it. At 2000 us, 13 of 156 presents that would have torn in
// a 248 s session still did; at 3000 us it was 4 of 119, with the holds costing about
// half a millisecond each. ML_PACING_TEAR_GUARD_US sets it, and 0 turns it off.
#define CADENCE_TEAR_GUARD_US 3000

// Held this far past the end of the scanout, so that the jitter in when Present()
// returns does not leave the frame just inside it after all. Without it, 53 of 105
// presents that would have torn in a 269 s session still landed inside the window,
// missing it by 44 us at the median.
#define CADENCE_TEAR_GUARD_MARGIN_US 400

// Longest believable gap between two host frames. Past it the timeline is learned again.
#define CADENCE_MAX_HOST_GAP_US 1000000

// Longest a host timestamp step may last before its offset is taken to be where the
// host's timeline now is, and frames are paced by it again. Runs measured on earlier
// builds lasted up to about a second and a half.
#define CADENCE_HOST_STEP_MAX_US 2000000

// Left for busy-spinning at the end of a wait, depending on how precise the timer
// is. No sleep lands within a few hundred microseconds of a target, and at a frame
// rate the display follows that error is on screen: measured at 90 FPS on 100 Hz,
// late starts made presented intervals less even than the host's own.
//
// Once drawing was scheduled, a frame usually reached this thread 1-2 ms before it
// was due to start drawing, and with 1000 us of spinning that left a timer sleep of
// a few hundred microseconds. Now and then that sleep ended 2-3 ms late, three times
// in a 105 s session, each a hitch of several milliseconds with the schedule itself
// steady. Spinning 2000 us leaves no sleep that short. ML_PACING_SPIN_US sets it.
#define CADENCE_SPIN_US 2000
#define CADENCE_SPIN_LOW_RES_US 3000

// Frames are scheduled with room to draw and present them: this percentile of recent
// draw times, the usual Present() call, and a margin, so that a slow draw rarely
// holds up the Present() call behind it.
//
// Before the renderer waited for the GPU to finish drawing, the margin was also the
// GPU's time to finish, and 1000 us kept Present() calls steady. That time is now
// part of the draw times, so the margin only covers draws slower than the
// percentile. It is latency, since every frame waits for it. ML_PACING_DRAW_MARGIN_US
// sets it.
#define CADENCE_DRAW_PERCENTILE 95
#define CADENCE_DRAW_MARGIN_US 200

// A frame waiting in the display's queue behind every present for this long, while
// presenting without tearing, is cleared by skipping one frame, at most once per
// CADENCE_DRAIN_MIN_GAP_US. See presentAt().
#define CADENCE_DRAIN_BACKLOG_SECONDS 2
#define CADENCE_DRAIN_MIN_GAP_US 10000000

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats),
    m_RendererAttributes(0),
    m_CadenceThread(nullptr),
    m_Trace(nullptr),
    m_WaitTimer(nullptr),
    m_WaitTimerHighRes(false),
    m_SmoothGain(CADENCE_SMOOTH_GAIN),
    m_SmoothMaxUs(CADENCE_SMOOTH_MAX_US),
    m_ArrivalPercentile(CADENCE_ARRIVAL_PERCENTILE),
    m_NoTearFraction(CADENCE_NO_TEAR_FRACTION),
    m_TearGuardUs(CADENCE_TEAR_GUARD_US),
    m_LearnFramesLeft(0),
    m_LastHostUs(0),
    m_IntervalUs(0),
    m_SmoothedUs(0),
    m_HostIntervalsUs{},
    m_HostIntervalCount(0),
    m_NextHostInterval(0),
    m_TransitUs{},
    m_TransitCount(0),
    m_NextTransit(0),
    m_TimelineArrivals(0),
    m_DelayUs(0),
    m_Tearing(true),
    m_LastTearSwitchUs(0),
    m_RenderCostUs(0),
    m_DrawCostsUs{},
    m_DrawCostCount(0),
    m_NextDrawCost(0),
    m_PresentCostUs(0),
    m_DrawMarginUs(CADENCE_DRAW_MARGIN_US),
    m_DrawLeadUs(0),
    m_SpinUs(CADENCE_SPIN_US),
    m_HostStepsEnabled(true),
    m_HostStepActive(false),
    m_HostStepStartUs(0),
    m_HostStepLatenessUs(0),
    m_LastArrivalUs(0),
    m_LastPacedHoldUs(0),
    m_QueueDrainEnabled(true),
    m_BacklogFrames(0),
    m_DrainNext(false),
    m_LastDrainUs(0),
    m_LastPresentUs(0),
    m_FrameIndex(0),
    m_DroppedSinceRow(0),
    m_DrainedSinceRow(0),
    m_EvictedFrames(0)
{

}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the cadence thread. It renders for itself and cleans up the render
    // context on its own way out.
    if (m_CadenceThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_CadenceThread, nullptr);
    }

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else if (m_CadenceThread == nullptr) {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);

    // Written out once every row is in
    delete m_Trace;

#ifdef Q_OS_WIN32
    if (m_WaitTimer != nullptr) {
        CloseHandle((HANDLE)m_WaitTimer);
    }
#endif
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread,
    // and when the cadence thread is rendering for itself
    if (m_RenderThread != nullptr || m_CadenceThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        me->m_FrameQueueLock.unlock();

        me->renderFrame(frame);
    }

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

bool Pacer::initialize(PDECODER_PARAMETERS params, bool enablePacing)
{
    SDL_Window* window = params->window;

    m_MaxVideoFps = params->frameRate;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    // Follow the host's cadence where the renderer can choose per present whether to
    // tear, which it only reports when frame pacing was asked for, in a window or full
    // screen. Pacing a renderer forces on without that, and a renderer that can't tear,
    // keep the V-blank pacer below. ML_PACING_CADENCE=0 uses the V-blank pacer
    // everywhere; D3D11VARenderer::initialize() checks it as well.
    bool cadenceAllowed = !(qEnvironmentVariableIsSet("ML_PACING_CADENCE") && qEnvironmentVariableIntValue("ML_PACING_CADENCE") == 0);

    if (enablePacing && cadenceAllowed &&
            m_VsyncRenderer->isRenderThreadSupported() && m_VsyncRenderer->supportsPresentTearing()) {
        // What the user chose, which the environment variables below override
        m_SmoothGain = qBound(1, params->pacingSmoothingPercent, 100) / 100.0;
        m_ArrivalPercentile = qBound(50, params->pacingArrivalPercentile, 100);
        m_NoTearFraction = qBound(0, params->pacingTearPercent, 100) / 100.0;

        if (qEnvironmentVariableIsSet("ML_PACING_SMOOTH")) {
            m_SmoothGain = qBound(1, qEnvironmentVariableIntValue("ML_PACING_SMOOTH"), 100) / 100.0;
        }
        if (qEnvironmentVariableIsSet("ML_PACING_SMOOTH_MAX_US")) {
            m_SmoothMaxUs = qBound(0, qEnvironmentVariableIntValue("ML_PACING_SMOOTH_MAX_US"), 100000);
        }
        if (qEnvironmentVariableIsSet("ML_PACING_PERCENTILE")) {
            m_ArrivalPercentile = qBound(50, qEnvironmentVariableIntValue("ML_PACING_PERCENTILE"), 100);
        }
        if (qEnvironmentVariableIsSet("ML_PACING_DRAW_MARGIN_US")) {
            m_DrawMarginUs = qBound(0, qEnvironmentVariableIntValue("ML_PACING_DRAW_MARGIN_US"), 20000);
        }
        if (qEnvironmentVariableIsSet("ML_PACING_SPIN_US")) {
            m_SpinUs = qBound(0, qEnvironmentVariableIntValue("ML_PACING_SPIN_US"), 20000);
        }
        if (qEnvironmentVariableIsSet("ML_PACING_HOST_STEPS")) {
            m_HostStepsEnabled = qEnvironmentVariableIntValue("ML_PACING_HOST_STEPS") != 0;
        }
        if (qEnvironmentVariableIsSet("ML_PACING_QUEUE_DRAIN")) {
            m_QueueDrainEnabled = qEnvironmentVariableIntValue("ML_PACING_QUEUE_DRAIN") != 0;
        }
        if (qEnvironmentVariableIsSet("ML_PACING_TEAR_GUARD_US")) {
            m_TearGuardUs = qBound(0, qEnvironmentVariableIntValue("ML_PACING_TEAR_GUARD_US"), 20000);
        }
        if (qEnvironmentVariableIsSet("ML_PACING_NO_TEAR_PCT")) {
            m_NoTearFraction = qBound(10, qEnvironmentVariableIntValue("ML_PACING_NO_TEAR_PCT"), 1000) / 100.0;
        }

        m_LearnFramesLeft = qMax(CADENCE_MIN_LEARN_FRAMES, m_MaxVideoFps);

#ifdef Q_OS_WIN32
        m_WaitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        m_WaitTimerHighRes = m_WaitTimer != nullptr;
        if (m_WaitTimer == nullptr) {
            m_WaitTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
#endif

        QString settings = QString("smoothing gain %1% on errors up to %5 us, arrival percentile %2, "
                                   "no tearing from %3% of the refresh rate, %4 wait timer, host timestamp steps %6, %7 us drawing margin, %8 us spin, display queue drain %9, %10 us tear guard")
                .arg((int)std::lround(m_SmoothGain * 100))
                .arg(m_ArrivalPercentile)
                .arg((int)std::lround(m_NoTearFraction * 100))
                .arg(m_WaitTimerHighRes ? "high resolution" : "standard")
                .arg((int)m_SmoothMaxUs)
                .arg(m_HostStepsEnabled ? "handled" : "ignored")
                .arg((int)m_DrawMarginUs)
                .arg(m_WaitTimerHighRes ? m_SpinUs : qMax(m_SpinUs, CADENCE_SPIN_LOW_RES_US))
                .arg(m_QueueDrainEnabled ? "on" : "off")
                .arg(m_TearGuardUs);

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: following the host's cadence, %d FPS stream on a %d Hz display (%s)",
                    m_MaxVideoFps, m_DisplayFps, qPrintable(settings));

        m_Trace = PacerTrace::startIfRequested(m_DisplayFps, m_MaxVideoFps, settings);

        m_CadenceThread = SDL_CreateThread(Pacer::cadenceThread, "PacerCadence", this);
        if (m_CadenceThread == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Unable to create frame pacing thread: %s",
                         SDL_GetError());
            return false;
        }

        return true;
    }

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // Render it
    m_VsyncRenderer->renderFrame(frame);

    finishFrame(frame, LiGetMicroseconds() - beforeRender);
}

// Counts a rendered frame, then releases the one before it and trims the render queue
void Pacer::finishFrame(AVFrame* frame, uint64_t renderTimeUs)
{
    m_VideoStats->totalRenderTimeUs += renderTimeUs;
    m_VideoStats->renderedFrames++;

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);

        // Only the cadence thread reads this, under the same lock
        m_EvictedFrames++;
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr || m_CadenceThread != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}

// Takes frames in order, waits until each one is due, then draws and presents it
// from this thread, so nothing sits between the wait ending and the present.
int Pacer::cadenceThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    while (!me->m_Stopping) {
        me->m_FrameQueueLock.lock();

        while (!me->m_Stopping && me->m_PacingQueue.isEmpty()) {
            me->m_PacingQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_PacingQueue.dequeue();
        int waiting = me->m_PacingQueue.count();
        me->m_DroppedSinceRow += me->m_EvictedFrames;
        me->m_EvictedFrames = 0;
        me->m_FrameQueueLock.unlock();

        PACER_TRACE_ROW row = {};
        row.dequeueUs = (int64_t)LiGetMicroseconds();

        // Wait for the GPU to finish decoding the frame, and count it as arriving once it
        // has, so it is scheduled from when it can actually be drawn. Only a wait that
        // blocked moves the arrival: a frame that finished decoding while it queued
        // arrived when the decoder handed it over.
        if (me->m_VsyncRenderer->waitForDecode((uint64_t)ML_FRAME_DECODE_BOUNDARY(frame))) {
            frame->pkt_dts = (int64_t)LiGetMicroseconds();
        }

        int64_t targetUs = me->scheduleFrame(frame, &row);

        // Two frames already waiting behind this one means we have fallen behind the
        // host, and showing it would only keep us there. A frame held up in the
        // display's queue is cleared the same way. See presentAt().
        bool drain = me->m_DrainNext;
        if (waiting >= 2 || drain) {
            if (drain) {
                me->m_DrainNext = false;
                me->m_LastDrainUs = (int64_t)LiGetMicroseconds();
                me->m_DrainedSinceRow++;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Frame pacing: a frame kept waiting in the display's queue; skipped one frame to clear it");
            }

            me->m_VideoStats->pacerDroppedFrames++;
            me->m_DroppedSinceRow++;
            av_frame_free(&frame);
            continue;
        }

        me->m_VsyncRenderer->setPresentTearing(row.tear != 0);
        me->presentAt(frame, targetUs, &row);

        if (me->m_Trace != nullptr) {
            row.queueDepth = (uint16_t)waiting;
            row.droppedBefore = (uint16_t)qMin<uint32_t>(me->m_DroppedSinceRow, 65535);
            row.drainedBefore = (uint8_t)qMin<uint32_t>(me->m_DrainedSinceRow, 255);
            me->m_Trace->record(row);
        }

        me->m_DroppedSinceRow = 0;
        me->m_DrainedSinceRow = 0;
    }

    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

// Works out when a frame should be presented, and whether it may tear.
//
// The host's capture timestamps carry its cadence. Each one is predicted from the
// tracked interval and corrected only part of the way towards its real value, which
// irons out the host's jitter while still following real changes of pace. The
// smoothed time is mapped onto our clock by a delay just long enough for nearly
// every recent frame to have arrived, so frames go out on the host's rhythm instead
// of the network's. For the first second nothing is paced while this is learned.
//
// Tearing lets a frame reach the display the moment it is presented, which is the
// lowest latency there is. Once the source runs close to the refresh rate the
// display can show every frame without it, so tearing stops there.
int64_t Pacer::scheduleFrame(AVFrame* frame, PPACER_TRACE_ROW row)
{
    int64_t hostUs = frame->pts;
    int64_t arrivalUs = frame->pkt_dts;

    row->frame = m_FrameIndex++;
    row->hostUs = hostUs;
    row->arrivalUs = arrivalUs;
    row->targetUs = arrivalUs;
    row->learning = 1;
    row->tear = m_Tearing ? 1 : 0;

    // How long before its target a frame starts drawing: long enough for nearly all
    // recent draws and the Present() call after them. Paced frames are scheduled that
    // much later, so the time a frame needs to draw is not taken out of its schedule.
    // Without it, frames drawing for about 2.8 ms were given about 1.5 ms: 96% of them
    // started drawing as they arrived and were presented when drawing finished, a
    // median 1.4 ms late, and presented intervals varied six times more than the
    // schedule.
    m_DrawLeadUs = m_RenderCostUs;
    if (m_DrawCostCount > 0) {
        uint32_t costs[PACER_CADENCE_DRAW_SAMPLES];
        std::copy(m_DrawCostsUs, m_DrawCostsUs + m_DrawCostCount, costs);
        uint32_t* percentile = costs + (m_DrawCostCount - 1) * CADENCE_DRAW_PERCENTILE / 100;
        std::nth_element(costs, percentile, costs + m_DrawCostCount);
        m_DrawLeadUs = *percentile + m_PresentCostUs;
    }
    m_DrawLeadUs += m_DrawMarginUs;

    if (hostUs <= 0) {
        // Nothing to pace against
        return arrivalUs;
    }

    int64_t hostIntervalUs = m_LastHostUs != 0 ? hostUs - m_LastHostUs : 0;
    m_LastHostUs = hostUs;

    if (hostIntervalUs <= 0 || hostIntervalUs > CADENCE_MAX_HOST_GAP_US) {
        // A new timeline, or a break in this one. Learn it again.
        m_SmoothedUs = (double)hostUs;
        m_TransitCount = 0;
        m_NextTransit = 0;
        m_TimelineArrivals = 0;
        m_LearnFramesLeft = qMax(CADENCE_MIN_LEARN_FRAMES, m_MaxVideoFps);
        m_HostStepActive = false;
    }
    else {
        m_HostIntervalsUs[m_NextHostInterval] = hostIntervalUs;
        m_NextHostInterval = (m_NextHostInterval + 1) % PACER_CADENCE_INTERVAL_SAMPLES;
        m_HostIntervalCount = qMin(m_HostIntervalCount + 1, PACER_CADENCE_INTERVAL_SAMPLES);

        // Tracked for prediction. A single long gap is limited so it cannot drag the
        // prediction far, but a real change of pace still comes through in a few frames.
        m_IntervalUs = m_IntervalUs > 0 ?
                    m_IntervalUs + (qBound(m_IntervalUs / 4, (double)hostIntervalUs, m_IntervalUs * 4) - m_IntervalUs) / CADENCE_INTERVAL_EMA_DIVISOR :
                    (double)hostIntervalUs;

        double predictedUs = m_SmoothedUs + m_IntervalUs;
        double errorUs = hostUs - predictedUs;

        double resyncUs = m_IntervalUs * CADENCE_RESYNC_INTERVALS;
        if (m_SmoothMaxUs > 0) {
            resyncUs = qMin(resyncUs, m_SmoothMaxUs);
        }

        if (std::fabs(errorUs) > resyncUs) {
            m_SmoothedUs = (double)hostUs;
        }
        else {
            m_SmoothedUs = predictedUs + errorUs * m_SmoothGain;
        }
    }

    int64_t targetUs = arrivalUs;

    // Is this frame part of a host timestamp step?
    //
    // While a game renders below the stream rate the host sometimes stamps a frame a
    // few milliseconds after the one before, although it arrives a whole game frame
    // later, and keeps stamping the frames after it that much early until one long
    // gap brings the stamps back into line. Paced by their stamps, those frames were
    // released on arrival while their neighbours were held: in a 433 s session that
    // was 150 of the 196 spacing errors over 5 ms, and counting their lateness held
    // the delay up by a frame even at a steady 100 FPS.
    //
    // A step opens when a frame arrives later after the previous one than its stamp
    // says by more than a stream frame interval, and later than its schedule by more
    // than half of one. Its frames are held as long as the last paced frame was, so
    // they keep the arrivals' spacing and the stream's latency, and they are left out
    // of the delay. It closes on the first frame back within half the lateness it
    // opened with, or after CADENCE_HOST_STEP_MAX_US. ML_PACING_HOST_STEPS=0 turns
    // this off.
    bool hostStep = false;

    if (m_HostStepsEnabled && m_LearnFramesLeft == 0 && m_TransitCount > 0 && m_MaxVideoFps > 0) {
        double streamIntervalUs = 1000000.0 / m_MaxVideoFps;
        double latenessUs = arrivalUs - (m_SmoothedUs + m_DelayUs);
        int64_t arrivalIntervalUs = m_LastArrivalUs != 0 ? arrivalUs - m_LastArrivalUs : 0;

        if (!m_HostStepActive && hostIntervalUs > 0 &&
                arrivalIntervalUs - hostIntervalUs > streamIntervalUs &&
                latenessUs > streamIntervalUs / 2) {
            m_HostStepActive = true;
            m_HostStepStartUs = arrivalUs;
            m_HostStepLatenessUs = latenessUs;
        }

        if (m_HostStepActive) {
            if (latenessUs < m_HostStepLatenessUs / 2 ||
                    arrivalUs - m_HostStepStartUs > CADENCE_HOST_STEP_MAX_US) {
                // Back in line with its stamps, or the offset has lasted long
                // enough to be the timeline now
                m_HostStepActive = false;
            }
            else {
                hostStep = true;
                targetUs = arrivalUs + (int64_t)m_LastPacedHoldUs;
                row->learning = 0;
                row->hostStep = 1;
            }
        }
    }

    m_LastArrivalUs = arrivalUs;

    if (!hostStep) {
        // The first arrivals of a timeline are dropped from the delay once they have
        // been passed. See CADENCE_STARTUP_ARRIVALS.
        if (m_TimelineArrivals < CADENCE_STARTUP_ARRIVALS) {
            m_TimelineArrivals++;
        }
        else if (m_TimelineArrivals == CADENCE_STARTUP_ARRIVALS) {
            m_TimelineArrivals++;
            m_TransitCount = 0;
            m_NextTransit = 0;
        }

        // How late this frame arrived relative to its smoothed host time, and how long
        // frames have to wait for nearly all of the recent ones to have arrived
        m_TransitUs[m_NextTransit] = arrivalUs - (int64_t)m_SmoothedUs;
        m_NextTransit = (m_NextTransit + 1) % PACER_CADENCE_TRANSIT_SAMPLES;
        m_TransitCount = qMin(m_TransitCount + 1, PACER_CADENCE_TRANSIT_SAMPLES);

        int64_t sorted[PACER_CADENCE_TRANSIT_SAMPLES];
        std::copy(m_TransitUs, m_TransitUs + m_TransitCount, sorted);
        int64_t* percentile = sorted + (m_TransitCount - 1) * m_ArrivalPercentile / 100;
        std::nth_element(sorted, percentile, sorted + m_TransitCount);
        int64_t fastestUs = *std::min_element(sorted, sorted + m_TransitCount);

        double wantedDelayUs = (double)*percentile + CADENCE_DELAY_GUARD_US;
        if (m_IntervalUs > 0) {
            wantedDelayUs = qMin(wantedDelayUs, fastestUs + CADENCE_MAX_HOLD_INTERVALS * m_IntervalUs);
        }

        if (m_LearnFramesLeft > 0) {
            m_LearnFramesLeft--;
            m_DelayUs = wantedDelayUs;
        }
        else {
            double stepUs = wantedDelayUs - m_DelayUs;

            if (stepUs < 0) {
                // A share of the way down, at least the fixed step, never past the target
                stepUs = qMax(qMin(stepUs / CADENCE_DELAY_FALL_DIVISOR, -(double)CADENCE_DELAY_FALL_US), stepUs);
            }
            else {
                stepUs = qMin(stepUs, (double)CADENCE_DELAY_RISE_US);
            }

            m_DelayUs += stepUs;

            // The hold limit binds on the delay itself, so a delay built up while the
            // source was slow cannot outlast the source speeding up
            if (m_IntervalUs > 0) {
                m_DelayUs = qMin(m_DelayUs, fastestUs + CADENCE_MAX_HOLD_INTERVALS * m_IntervalUs);
            }

            targetUs = qMax(arrivalUs, (int64_t)(m_SmoothedUs + m_DelayUs + m_DrawLeadUs));
            m_LastPacedHoldUs = (double)(targetUs - arrivalUs);
            row->learning = 0;
        }
    }

    // Tear below the no-tearing threshold, judged from the median host interval so a
    // single stall cannot flip it, and switched at most once every
    // CADENCE_TEAR_MIN_SWITCH_US
    int64_t medianIntervalUs = 0;
    if (m_HostIntervalCount > 0) {
        int64_t intervals[PACER_CADENCE_INTERVAL_SAMPLES];
        std::copy(m_HostIntervalsUs, m_HostIntervalsUs + m_HostIntervalCount, intervals);
        int64_t* median = intervals + m_HostIntervalCount / 2;
        std::nth_element(intervals, median, intervals + m_HostIntervalCount);
        medianIntervalUs = *median;
    }

    bool tearSwitchAllowed = m_LastTearSwitchUs == 0 ||
            arrivalUs - m_LastTearSwitchUs >= CADENCE_TEAR_MIN_SWITCH_US;

    if (medianIntervalUs > 0 && m_DisplayFps > 0 && tearSwitchAllowed) {
        double sourceFps = 1000000.0 / medianIntervalUs;

        if (m_Tearing && sourceFps >= m_NoTearFraction * m_DisplayFps) {
            m_Tearing = false;
            m_LastTearSwitchUs = arrivalUs;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Frame pacing: source at %.1f FPS on a %d Hz display, presenting without tearing",
                        sourceFps, m_DisplayFps);
        }
        else if (!m_Tearing && sourceFps < (m_NoTearFraction - CADENCE_TEAR_HYSTERESIS) * m_DisplayFps) {
            m_Tearing = true;
            m_LastTearSwitchUs = arrivalUs;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Frame pacing: source at %.1f FPS on a %d Hz display, presenting with tearing",
                        sourceFps, m_DisplayFps);
        }
    }

    // Keep a tearing present out of the previous frame's scanout, where it would tear
    // across the picture, as long as that means a short wait. See CADENCE_TEAR_GUARD_US.
    if (m_Tearing && m_TearGuardUs > 0 && m_DisplayFps > 0 && m_LastPresentUs != 0) {
        int64_t scanoutEndUs = m_LastPresentUs + 1000000 / m_DisplayFps + CADENCE_TEAR_GUARD_MARGIN_US;

        if (targetUs < scanoutEndUs && scanoutEndUs - targetUs <= m_TearGuardUs) {
            targetUs = scanoutEndUs;
            row->tearGuard = 1;
        }
    }

    row->smoothedUs = (int64_t)m_SmoothedUs;
    row->delayUs = (int64_t)m_DelayUs;
    row->targetUs = targetUs;
    row->intervalUs = (int32_t)qMin<int64_t>(medianIntervalUs, INT32_MAX);
    row->tear = m_Tearing ? 1 : 0;

    return targetUs;
}

// Draws a frame and presents it so that Present() returns at the target.
//
// Drawing takes a varying time, much of it waiting on the decoder's context lock,
// and drawing and presenting in one call put all of that variation on screen: at
// 90 FPS on a 100 Hz panel, draw time accounted for nearly all of how far Present()
// missed its target, and presented intervals varied more than the host's. Where the
// renderer can separate the two, the frame is drawn ahead with room for a slow draw
// and only the Present() call, which takes a steady fraction of a millisecond, is
// timed. Drawing starts the lead scheduleFrame() worked out before the target.
void Pacer::presentAt(AVFrame* frame, int64_t targetUs, PPACER_TRACE_ROW row)
{
    int64_t presentCostUs = (int64_t)m_PresentCostUs;

    row->presentationMode = -1;
    row->drawDueUs = targetUs - (int64_t)m_DrawLeadUs;
    waitUntilUs(row->drawDueUs);

    row->renderStartUs = (int64_t)LiGetMicroseconds();
    uint64_t pacerTimeUs = (uint64_t)(row->renderStartUs - frame->pkt_dts);

    if (!m_VsyncRenderer->prepareFrame(frame)) {
        // This renderer only draws and presents together, so start that by what
        // the two usually take
        waitUntilUs(targetUs - (int64_t)m_RenderCostUs);

        row->renderStartUs = (int64_t)LiGetMicroseconds();
        renderFrame(frame);
        row->presentUs = (int64_t)LiGetMicroseconds();
        m_LastPresentUs = row->presentUs;
        row->drawEndUs = row->renderStartUs;
        row->presentStartUs = row->renderStartUs;

        m_RenderCostUs += ((row->presentUs - row->renderStartUs) - m_RenderCostUs) / 16;
        return;
    }

    row->drawEndUs = (int64_t)LiGetMicroseconds();

    waitUntilUs(targetUs - presentCostUs);

    row->presentStartUs = (int64_t)LiGetMicroseconds();
    m_VsyncRenderer->presentPreparedFrame();
    row->presentUs = (int64_t)LiGetMicroseconds();
    m_LastPresentUs = row->presentUs;

    // What the display has done with frames so far, where the renderer reads it back.
    // After the timestamp, so reading it never counts as part of the Present() call.
    PRESENT_FEEDBACK feedback;
    bool haveFeedback = m_VsyncRenderer->getPresentFeedback(&feedback);
    if (haveFeedback) {
        row->presentId = feedback.presentId;
        row->displayedId = feedback.displayedId;
        row->displayedUs = feedback.displayedUs;
        row->presentationMode = (int8_t)feedback.presentationMode;
    }

    // Is a frame being held up in the display's queue?
    //
    // Presented without tearing, a frame waits for the display's next refresh. When two
    // frames reach the queue within one refresh, one is left waiting behind every
    // present from then on, since at the display's own rate nothing takes it out, and
    // every frame reaches the screen a refresh later than it could. On a 60 Hz laptop
    // panel streaming 60 FPS, the display was two presents behind for a whole session
    // that started that way and one behind for one that didn't, a refresh of latency
    // decided by how the session happened to start. A refresh without a new frame
    // clears it, so a late frame or a host stall does that on its own; if nothing has
    // after CADENCE_DRAIN_BACKLOG_SECONDS, the next frame is skipped.
    // ML_PACING_QUEUE_DRAIN=0 turns this off.
    if (m_QueueDrainEnabled && haveFeedback) {
        bool backlog = !row->tear && !row->learning && feedback.displayedId != 0 &&
                feedback.presentId >= feedback.displayedId + 2;
        m_BacklogFrames = backlog ? m_BacklogFrames + 1 : 0;

        if (m_BacklogFrames >= CADENCE_DRAIN_BACKLOG_SECONDS * qMax(CADENCE_MIN_LEARN_FRAMES, m_MaxVideoFps) &&
                (m_LastDrainUs == 0 || row->presentUs - m_LastDrainUs >= CADENCE_DRAIN_MIN_GAP_US)) {
            m_DrainNext = true;
            m_BacklogFrames = 0;
        }
    }

    int64_t drawUs = row->drawEndUs - row->renderStartUs;
    int64_t presentCallUs = row->presentUs - row->presentStartUs;

    m_DrawCostsUs[m_NextDrawCost] = (uint32_t)qBound<int64_t>(0, drawUs, UINT32_MAX);
    m_NextDrawCost = (m_NextDrawCost + 1) % PACER_CADENCE_DRAW_SAMPLES;
    m_DrawCostCount = qMin(m_DrawCostCount + 1, PACER_CADENCE_DRAW_SAMPLES);
    m_PresentCostUs += (presentCallUs - m_PresentCostUs) / 16;

    m_VideoStats->totalPacerTimeUs += pacerTimeUs;
    finishFrame(frame, (uint64_t)(drawUs + presentCallUs));
}

// Sleeps until a moment on the LiGetMicroseconds() clock, spinning for the last
// stretch because no sleep is precise enough to land on it
void Pacer::waitUntilUs(int64_t targetUs)
{
    const int64_t spinUs = m_WaitTimerHighRes ? m_SpinUs : qMax(m_SpinUs, CADENCE_SPIN_LOW_RES_US);

    for (;;) {
        int64_t remainingUs = targetUs - (int64_t)LiGetMicroseconds();
        if (remainingUs <= 0 || m_Stopping) {
            return;
        }

        if (remainingUs <= spinUs) {
            // Spin the rest of the way. Yielding through SDL_Delay(0) slept instead
            // and started frames anywhere from 23 to 556 us late.
#ifdef Q_OS_WIN32
            YieldProcessor();
#endif
            continue;
        }

        // In bounded steps, so shutting down never waits long
        int64_t sleepUs = qMin<int64_t>(remainingUs, 100000) - spinUs;

#ifdef Q_OS_WIN32
        if (m_WaitTimer != nullptr) {
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -sleepUs * 10;

            if (SetWaitableTimer((HANDLE)m_WaitTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject((HANDLE)m_WaitTimer, INFINITE);
                continue;
            }
        }
#endif

        SDL_Delay((Uint32)qMax<int64_t>(1, sleepUs / 1000));
    }
}
