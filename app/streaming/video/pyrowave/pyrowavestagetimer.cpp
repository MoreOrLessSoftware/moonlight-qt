#include "pyrowavestagetimer.h"

#include <QMutexLocker>
#include <QtGlobal>

// How long the thread waits at a time, so it notices when it is stopped
#define STAGE_TIMER_WAIT_MS 50

// GPU work not done in this long never will be, such as after the device was lost
#define STAGE_TIMER_GIVE_UP_US 1000000

// How often the averages are logged
#define STAGE_TIMER_PERIOD_US 5000000

// Frames waiting for the thread. Far more than are ever in flight, so a stalled thread
// can't pile them up.
#define STAGE_TIMER_MAX_FRAMES 32

PyrowaveStageTimer* PyrowaveStageTimer::createIfEnabled(pyrowave_result (*cpuWait)(pyrowave_sync_object, uint64_t, uint64_t))
{
    if (qEnvironmentVariableIntValue("ML_PYROWAVE_STAGE_LOG") == 0) {
        return nullptr;
    }

    auto timer = new PyrowaveStageTimer(cpuWait);
    timer->m_Thread = SDL_CreateThread(PyrowaveStageTimer::threadProc, "PyroWaveStages", timer);
    if (timer->m_Thread == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: unable to start the stage timer's thread: %s",
                    SDL_GetError());
        delete timer;
        return nullptr;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave: logging how long each stage of decoding takes");
    return timer;
}

PyrowaveStageTimer::PyrowaveStageTimer(pyrowave_result (*cpuWait)(pyrowave_sync_object, uint64_t, uint64_t))
    : m_CpuWait(cpuWait),
      m_Thread(nullptr),
      m_Current(),
      m_Stopping(false),
      m_Stages(),
      m_FrameCount(0),
      m_PeriodStartUs(0)
{
}

PyrowaveStageTimer::~PyrowaveStageTimer()
{
    m_Lock.lock();
    m_Stopping = true;
    m_FrameAdded.wakeAll();
    m_Lock.unlock();

    if (m_Thread != nullptr) {
        SDL_WaitThread(m_Thread, nullptr);
    }
}

void PyrowaveStageTimer::frameStarted()
{
    m_Current = {};
    m_Current.startUs = (int64_t)LiGetMicroseconds();
}

void PyrowaveStageTimer::framePushed()
{
    m_Current.pushedUs = (int64_t)LiGetMicroseconds();
}

void PyrowaveStageTimer::decodeSubmitted(pyrowave_sync_object sync, uint64_t decodedValue)
{
    m_Current.submittedUs = (int64_t)LiGetMicroseconds();
    m_Current.sync = sync;
    m_Current.decodedValue = decodedValue;
}

void PyrowaveStageTimer::frameEnded()
{
    if (m_Current.decodedValue == 0) {
        return;
    }

    QMutexLocker locker(&m_Lock);
    if (m_Frames.size() < STAGE_TIMER_MAX_FRAMES) {
        m_Frames.push_back(m_Current);
        m_FrameAdded.wakeOne();
    }
}

int PyrowaveStageTimer::threadProc(void* context)
{
    PyrowaveStageTimer* me = reinterpret_cast<PyrowaveStageTimer*>(context);

    // It notes the time it wakes, so it has to wake promptly
#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    me->run();
    return 0;
}

void PyrowaveStageTimer::run()
{
    for (;;) {
        Frame frame;
        {
            QMutexLocker locker(&m_Lock);
            while (!m_Stopping && m_Frames.empty()) {
                m_FrameAdded.wait(&m_Lock);
            }
            if (m_Stopping) {
                return;
            }
            frame = m_Frames.front();
            m_Frames.pop_front();
        }

        int64_t decodedUs = waitForDecode(frame);
        if (decodedUs == 0) {
            return;
        }

        count(frame, decodedUs);
    }
}

int64_t PyrowaveStageTimer::waitForDecode(const Frame& frame)
{
    for (;;) {
        pyrowave_result result = m_CpuWait(frame.sync, frame.decodedValue, STAGE_TIMER_WAIT_MS * 1000000ULL);
        int64_t nowUs = (int64_t)LiGetMicroseconds();
        if (result == PYROWAVE_SUCCESS) {
            return nowUs;
        }

        QMutexLocker locker(&m_Lock);
        if (m_Stopping) {
            return 0;
        }
        else if (result != PYROWAVE_TIMEOUT || nowUs - frame.submittedUs > STAGE_TIMER_GIVE_UP_US) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave: the stage timer couldn't wait for a decode (%d); it has stopped",
                        result);
            return 0;
        }
    }
}

void PyrowaveStageTimer::countStage(int stage, int64_t us)
{
    m_Stages[stage].totalUs += us;
    m_Stages[stage].maxUs = qMax(m_Stages[stage].maxUs, us);
}

void PyrowaveStageTimer::count(const Frame& frame, int64_t decodedUs)
{
    countStage(STAGE_BITSTREAM, frame.pushedUs - frame.startUs);
    countStage(STAGE_SUBMIT, frame.submittedUs - frame.pushedUs);
    countStage(STAGE_VULKAN, decodedUs - frame.submittedUs);
    countStage(STAGE_TOTAL, decodedUs - frame.startUs);
    m_FrameCount++;

    if (m_PeriodStartUs == 0) {
        m_PeriodStartUs = frame.startUs;
    }
    if (decodedUs - m_PeriodStartUs < STAGE_TIMER_PERIOD_US) {
        return;
    }

    auto avgMs = [this](int stage) { return m_Stages[stage].totalUs / 1000.0 / m_FrameCount; };
    auto maxMs = [this](int stage) { return m_Stages[stage].maxUs / 1000.0; };
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave stages over %d frames, avg/max ms: bitstream %.2f/%.2f, submit %.2f/%.2f, "
                "Vulkan decode %.2f/%.2f, total %.2f/%.2f",
                m_FrameCount,
                avgMs(STAGE_BITSTREAM), maxMs(STAGE_BITSTREAM),
                avgMs(STAGE_SUBMIT), maxMs(STAGE_SUBMIT),
                avgMs(STAGE_VULKAN), maxMs(STAGE_VULKAN),
                avgMs(STAGE_TOTAL), maxMs(STAGE_TOTAL));

    for (Stage& stage : m_Stages) {
        stage = {};
    }
    m_FrameCount = 0;
    m_PeriodStartUs = decodedUs;
}
