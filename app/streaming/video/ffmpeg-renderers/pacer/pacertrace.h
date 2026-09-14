#pragma once

#include <QFile>
#include <QMutex>
#include <QString>
#include <QTextStream>
#include <QVector>
#include <QWaitCondition>

#include <SDL.h>

#include <cstdint>

// One presented frame, as the pacer saw it. Everything here is known the moment
// Present() returns, so recording a row never waits on anything.
typedef struct _PACER_TRACE_ROW {
    uint64_t frame;             // Sequence number, counting frames the pacer dropped
    int64_t hostUs;             // Host capture time (host clock)
    int64_t arrivalUs;          // When the decoder handed the frame over
    int64_t smoothedUs;         // Host capture time after smoothing (host clock)
    int64_t delayUs;            // Offset mapping smoothed host time onto our clock
    int64_t targetUs;           // When Present() was meant to return
    int64_t renderStartUs;      // When drawing started
    int64_t drawEndUs;          // When drawing finished
    int64_t presentStartUs;     // When Present() was called
    int64_t presentUs;          // When Present() returned
    int32_t intervalUs;         // Median of recent host frame intervals
    uint16_t queueDepth;        // Frames already waiting behind this one
    uint16_t droppedBefore;     // Frames never shown since the previous row
    uint8_t learning;           // Still learning the host's cadence
    uint8_t tear;               // Presented with tearing permission
    uint8_t hostStep;           // Part of a host timestamp step, held like the frame before it
} PACER_TRACE_ROW, *PPACER_TRACE_ROW;

// Writes one CSV row per presented frame and a summary on close. Rows are handed
// to a writer thread, so a slow disk costs rows rather than frames.
class PacerTrace
{
public:
    // Returns a running trace when ML_PACING_TRACE asks for one, otherwise nullptr.
    // 1 writes beside the log, a folder gets a file named after the session start,
    // and anything else is used as the file path.
    static PacerTrace* startIfRequested(int displayHz, int streamFps, const QString& settings);

    ~PacerTrace();

    // Safe to call from the pacing thread. Never blocks on the file.
    void record(const PACER_TRACE_ROW& row);

private:
    explicit PacerTrace(int displayHz);

    static int writerThread(void* context);
    void drain();
    void writeRow(const PACER_TRACE_ROW& row);
    void writeFooter();

    // Magnitudes in 100 us buckets up to 50 ms, the last bucket collecting the rest.
    struct Histogram {
        static const int k_Buckets = 501;
        uint64_t counts[k_Buckets] = {};
        uint64_t samples = 0;
        int64_t totalUs = 0;
        int64_t maxUs = 0;

        void add(int64_t us);
        int64_t percentileUs(double percent) const;
        QString describe() const;
    };

    int64_t m_PeriodUs;

    QFile m_File;
    QTextStream m_Stream;
    SDL_Thread* m_Thread;
    bool m_Stopping;

    QMutex m_Lock;
    QWaitCondition m_RowsAvailable;
    QVector<PACER_TRACE_ROW> m_Pending;
    QVector<PACER_TRACE_ROW> m_Writing;
    uint64_t m_DroppedRows;

    // Accumulated by the writer thread only
    bool m_HavePrevious;
    PACER_TRACE_ROW m_Previous;
    int64_t m_FirstPresentUs;
    uint64_t m_Rows;
    uint64_t m_LearningRows;
    uint64_t m_DroppedFrames;
    uint64_t m_MissedTargets;
    uint64_t m_LongFrames;
    uint64_t m_ShortFrames;
    uint64_t m_SourceStalls;
    uint64_t m_TornFrames;
    uint64_t m_PossibleTears;
    uint64_t m_TearThirds[3];
    uint64_t m_TearSwitches;
    uint64_t m_HostStepFrames;

    Histogram m_Spacing;
    Histogram m_Late;
    Histogram m_Hold;
    Histogram m_Draw;
    Histogram m_PresentCall;
    Histogram m_HostJitter;

    static const int k_WorstPairs = 10;
    int64_t m_WorstSpacingUs[k_WorstPairs];
    uint64_t m_WorstFrame[k_WorstPairs];
};
