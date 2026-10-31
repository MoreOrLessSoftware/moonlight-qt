#include "gputimer.h"

#include <QMutexLocker>

// How long the thread waits at a time, so it notices when it is stopped
#define GPU_TIMER_WAIT_MS 50

// A mark the GPU hasn't reached in this long never will be, such as after the device
// was lost. Timing stops rather than wait on it forever.
#define GPU_TIMER_GIVE_UP_US 1000000

// How long completionUs() waits for the thread to note a mark the GPU has reached. The
// thread wakes for it within microseconds, unless it is still behind on earlier marks,
// which the GPU has reached too.
#define GPU_TIMER_NOTE_WAIT_US 2000

GpuTimer::GpuTimer(IFFmpegRenderer* renderer, const char* name)
    : m_Renderer(renderer),
      m_Name(name),
      m_Thread(nullptr),
      m_NextSeq(1),
      m_NextToWait(1),
      m_Stopping(false),
      m_Failed(false)
{
}

GpuTimer::~GpuTimer()
{
    m_Lock.lock();
    m_Stopping = true;
    m_EntryAdded.wakeAll();
    m_Lock.unlock();

    if (m_Thread != nullptr) {
        SDL_WaitThread(m_Thread, nullptr);
    }
}

uint64_t GpuTimer::track(const AVFrame* frame, const GPU_MARK* mark)
{
    QMutexLocker locker(&m_Lock);

    // The AVFrame may be one an earlier frame was freed from, whose entry must not be
    // taken for this frame's
    for (Entry& entry : m_Entries) {
        if (entry.frame == frame) {
            entry.frame = nullptr;
        }
    }

    if (mark == nullptr || m_Failed) {
        return 0;
    }

    // Started with the first mark, so renderers that never make one don't get a thread
    if (m_Thread == nullptr) {
        m_Thread = SDL_CreateThread(GpuTimer::threadProc, m_Name, this);
        if (m_Thread == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s: unable to start its thread: %s",
                        m_Name,
                        SDL_GetError());
            m_Failed = true;
            return 0;
        }
    }

    uint64_t seq = m_NextSeq++;
    m_Entries.push_back({ frame, *mark, seq, 0 });
    if (m_Entries.size() > GPU_TIMER_MAX_ENTRIES) {
        m_Entries.pop_front();
    }

    m_EntryAdded.wakeOne();
    return seq;
}

uint64_t GpuTimer::ticketFor(const AVFrame* frame)
{
    QMutexLocker locker(&m_Lock);

    for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it) {
        if (it->frame == frame) {
            return it->seq;
        }
    }

    return 0;
}

int64_t GpuTimer::noteUs(uint64_t ticket)
{
    QMutexLocker locker(&m_Lock);

    Entry* entry = findEntry(ticket);
    if (entry == nullptr || (m_Failed && entry->doneUs == 0)) {
        return -1;
    }

    return entry->doneUs;
}

int64_t GpuTimer::completionUs(uint64_t ticket, int64_t nowUs)
{
    QMutexLocker locker(&m_Lock);

    Entry* entry = findEntry(ticket);
    if (entry == nullptr || m_Failed) {
        return 0;
    }
    else if (entry->doneUs != 0) {
        return entry->doneUs;
    }

    // Not noted yet. Either the GPU hasn't reached the mark, or it has just reached it
    // and the thread is about to note when.
    GPU_MARK mark = entry->mark;

    locker.unlock();
    int result = m_Renderer->waitForGpuMark(mark, 0);
    locker.relock();

    if (result == 0) {
        return nowUs;
    }
    else if (result < 0) {
        return 0;
    }

    int64_t giveUpUs = (int64_t)LiGetMicroseconds() + GPU_TIMER_NOTE_WAIT_US;
    for (;;) {
        entry = findEntry(ticket);
        if (entry == nullptr || m_Failed) {
            return 0;
        }
        else if (entry->doneUs != 0) {
            return entry->doneUs;
        }

        int64_t remainingUs = giveUpUs - (int64_t)LiGetMicroseconds();
        if (remainingUs <= 0) {
            return 0;
        }

        m_EntryDone.wait(&m_Lock, (unsigned long)qMax<int64_t>(1, remainingUs / 1000));
    }
}

GpuTimer::Entry* GpuTimer::findEntry(uint64_t seq)
{
    if (seq == 0 || m_Entries.empty() || seq < m_Entries.front().seq || seq > m_Entries.back().seq) {
        return nullptr;
    }

    // Sequence numbers are consecutive
    return &m_Entries[seq - m_Entries.front().seq];
}

int GpuTimer::threadProc(void* context)
{
    GpuTimer* me = reinterpret_cast<GpuTimer*>(context);

    // It notes the time it wakes, so it has to wake promptly
#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    me->run();
    return 0;
}

// Waits for each mark in the order they were tracked, which is the order the GPU
// reaches them
void GpuTimer::run()
{
    QMutexLocker locker(&m_Lock);

    uint64_t waitingSeq = 0;
    int64_t waitStartUs = 0;

    while (!m_Stopping) {
        // Marks evicted before their turn are skipped
        if (!m_Entries.empty() && m_NextToWait < m_Entries.front().seq) {
            m_NextToWait = m_Entries.front().seq;
        }

        Entry* entry = findEntry(m_NextToWait);
        if (entry == nullptr) {
            m_EntryAdded.wait(&m_Lock);
            continue;
        }

        uint64_t seq = entry->seq;
        GPU_MARK mark = entry->mark;
        if (seq != waitingSeq) {
            waitingSeq = seq;
            waitStartUs = (int64_t)LiGetMicroseconds();
        }

        locker.unlock();
        int result = m_Renderer->waitForGpuMark(mark, GPU_TIMER_WAIT_MS);
        int64_t nowUs = (int64_t)LiGetMicroseconds();
        locker.relock();

        if (result == 0 && nowUs - waitStartUs < GPU_TIMER_GIVE_UP_US) {
            continue;
        }
        else if (result != 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s: waiting for the GPU failed (%d); timing runs to the CPU's part from now on",
                        m_Name,
                        result);
            m_Failed = true;
            m_EntryDone.wakeAll();
            return;
        }

        entry = findEntry(seq);
        if (entry != nullptr) {
            entry->doneUs = nowUs;
        }

        m_NextToWait = seq + 1;
        m_EntryDone.wakeAll();
    }
}
