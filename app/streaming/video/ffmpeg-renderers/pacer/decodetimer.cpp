#include "decodetimer.h"

#include <QMutexLocker>

// How long the thread waits at a time, so it notices when it is stopped
#define DECODE_TIMER_WAIT_MS 50

// A mark the GPU hasn't reached in this long never will be, such as after the device
// was lost. Timing stops rather than wait on it forever.
#define DECODE_TIMER_GIVE_UP_US 1000000

// How long completionUs() waits for the thread to note a decode the GPU has finished.
// The thread wakes for it within microseconds, unless it is still behind on earlier
// frames, which the GPU has finished too.
#define DECODE_TIMER_NOTE_WAIT_US 2000

DecodeTimer::DecodeTimer(IFFmpegRenderer* renderer)
    : m_Renderer(renderer),
      m_Thread(nullptr),
      m_NextSeq(1),
      m_NextToWait(1),
      m_Stopping(false),
      m_Failed(false)
{
}

DecodeTimer::~DecodeTimer()
{
    m_Lock.lock();
    m_Stopping = true;
    m_EntryAdded.wakeAll();
    m_Lock.unlock();

    if (m_Thread != nullptr) {
        SDL_WaitThread(m_Thread, nullptr);
    }
}

void DecodeTimer::track(AVFrame* frame)
{
    DECODE_MARK mark = {};
    bool marked = m_Renderer->markDecodeEnd(frame, &mark);

    QMutexLocker locker(&m_Lock);

    // The AVFrame may be one an earlier frame was freed from, whose entry must not be
    // taken for this frame's
    for (Entry& entry : m_Entries) {
        if (entry.frame == frame) {
            entry.frame = nullptr;
        }
    }

    if (!marked || m_Failed) {
        return;
    }

    // Started with the first frame there is something to wait for, so decoders that
    // never mark one don't get a thread at all
    if (m_Thread == nullptr) {
        m_Thread = SDL_CreateThread(DecodeTimer::threadProc, "DecodeTimer", this);
        if (m_Thread == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to start the thread that times decodes: %s",
                        SDL_GetError());
            m_Failed = true;
            return;
        }
    }

    m_Entries.push_back({ frame, mark, m_NextSeq++, 0 });
    if (m_Entries.size() > DECODE_TIMER_MAX_ENTRIES) {
        m_Entries.pop_front();
    }

    m_EntryAdded.wakeOne();
}

int64_t DecodeTimer::completionUs(const AVFrame* frame, int64_t nowUs)
{
    QMutexLocker locker(&m_Lock);

    if (m_Failed) {
        return 0;
    }

    Entry* entry = nullptr;
    for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it) {
        if (it->frame == frame) {
            entry = &*it;
            break;
        }
    }

    if (entry == nullptr) {
        return 0;
    }
    else if (entry->doneUs != 0) {
        return entry->doneUs;
    }

    // Not noted yet. Either the GPU is still decoding the frame, or it has just finished
    // and the thread is about to note when.
    uint64_t seq = entry->seq;
    DECODE_MARK mark = entry->mark;

    locker.unlock();
    int result = m_Renderer->waitForDecodeEnd(mark, 0);
    locker.relock();

    if (result == 0) {
        return nowUs;
    }
    else if (result < 0) {
        return 0;
    }

    int64_t giveUpUs = (int64_t)LiGetMicroseconds() + DECODE_TIMER_NOTE_WAIT_US;
    for (;;) {
        entry = findEntry(seq);
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

DecodeTimer::Entry* DecodeTimer::findEntry(uint64_t seq)
{
    if (m_Entries.empty() || seq < m_Entries.front().seq || seq > m_Entries.back().seq) {
        return nullptr;
    }

    // Sequence numbers are consecutive
    return &m_Entries[seq - m_Entries.front().seq];
}

int DecodeTimer::threadProc(void* context)
{
    DecodeTimer* me = reinterpret_cast<DecodeTimer*>(context);

    // It notes the time it wakes, so it has to wake promptly
#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    me->run();
    return 0;
}

// Waits for each frame's mark in the order the frames were decoded, which is the order
// the GPU finishes them in
void DecodeTimer::run()
{
    QMutexLocker locker(&m_Lock);

    uint64_t waitingSeq = 0;
    int64_t waitStartUs = 0;

    while (!m_Stopping) {
        // Frames evicted before their turn are skipped
        if (!m_Entries.empty() && m_NextToWait < m_Entries.front().seq) {
            m_NextToWait = m_Entries.front().seq;
        }

        Entry* entry = findEntry(m_NextToWait);
        if (entry == nullptr) {
            m_EntryAdded.wait(&m_Lock);
            continue;
        }

        uint64_t seq = entry->seq;
        DECODE_MARK mark = entry->mark;
        if (seq != waitingSeq) {
            waitingSeq = seq;
            waitStartUs = (int64_t)LiGetMicroseconds();
        }

        locker.unlock();
        int result = m_Renderer->waitForDecodeEnd(mark, DECODE_TIMER_WAIT_MS);
        int64_t nowUs = (int64_t)LiGetMicroseconds();
        locker.relock();

        if (result == 0 && nowUs - waitStartUs < DECODE_TIMER_GIVE_UP_US) {
            continue;
        }
        else if (result != 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Timing decodes on the GPU failed (%d); decoding times run to when frames are handed over from now on",
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
