#include "pacertrace.h"
#include "path.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <utility>

// How many rows the pacing thread may get ahead of the writer before rows are
// dropped. Over a minute of slack at 120 FPS.
#define PACER_TRACE_CAPACITY 8192

// A host interval this many times the tracked one is the host pausing, not a
// frame time, so the pair across it is not charged as spacing error.
#define PACER_TRACE_STALL_INTERVALS 2.5

void PacerTrace::Histogram::add(int64_t us)
{
    int64_t magnitude = us < 0 ? -us : us;
    counts[qMin<int64_t>(magnitude / 100, k_Buckets - 1)]++;
    samples++;
    totalUs += magnitude;
    maxUs = qMax(maxUs, magnitude);
}

// The top of the bucket the percentile falls in, so a figure reads as "no worse
// than". The overflow bucket reports the maximum instead.
int64_t PacerTrace::Histogram::percentileUs(double percent) const
{
    if (samples == 0) {
        return 0;
    }

    uint64_t target = (uint64_t)(samples * percent / 100.0);
    uint64_t seen = 0;

    for (int i = 0; i < k_Buckets; i++) {
        seen += counts[i];
        if (seen > target) {
            return i == k_Buckets - 1 ? maxUs : qMin<int64_t>((i + 1) * 100, maxUs);
        }
    }

    return maxUs;
}

QString PacerTrace::Histogram::describe() const
{
    if (samples == 0) {
        return QStringLiteral("none");
    }

    return QString("mean %1, p50 %2, p95 %3, p99 %4, p99.9 %5, max %6 us (%7 samples)")
            .arg(totalUs / (int64_t)samples)
            .arg(percentileUs(50))
            .arg(percentileUs(95))
            .arg(percentileUs(99))
            .arg(percentileUs(99.9))
            .arg(maxUs)
            .arg(samples);
}

PacerTrace::PacerTrace(int displayHz) :
    m_PeriodUs(displayHz > 0 ? 1000000 / displayHz : 0),
    m_Thread(nullptr),
    m_Stopping(false),
    m_DroppedRows(0),
    m_HavePrevious(false),
    m_Previous{},
    m_FirstPresentUs(0),
    m_Rows(0),
    m_LearningRows(0),
    m_DroppedFrames(0),
    m_MissedTargets(0),
    m_LongFrames(0),
    m_ShortFrames(0),
    m_SourceStalls(0),
    m_TornFrames(0),
    m_PossibleTears(0),
    m_TearThirds{},
    m_TearSwitches(0),
    m_HostStepFrames(0),
    m_PresentIds{},
    m_PresentTimesUs{},
    m_LastDisplayedId(0),
    m_StatsRows(0),
    m_QueuedCounts{},
    m_ModeCounts{},
    m_QueueDrains(0),
    m_WorstSpacingUs{},
    m_WorstFrame{}
{
    // Reserved up front so recording a row never allocates on the pacing thread
    m_Pending.reserve(PACER_TRACE_CAPACITY);
    m_Writing.reserve(PACER_TRACE_CAPACITY);
}

PacerTrace* PacerTrace::startIfRequested(int displayHz, int streamFps, const QString& settings)
{
    QString request = qEnvironmentVariable("ML_PACING_TRACE");
    if (request.isEmpty() || request == QLatin1String("0")) {
        return nullptr;
    }

    QString path = request;
    QString directory;

    if (request == QLatin1String("1")) {
        directory = Path::getLogDir();
    }
    else if (QFileInfo(request).isDir()) {
        directory = request;
    }

    if (!directory.isEmpty()) {
        path = QDir(directory).filePath(QString("Moonlight-pacing-%1.csv")
                                        .arg(QDateTime::currentSecsSinceEpoch()));
    }

    PacerTrace* trace = new PacerTrace(displayHz);

    trace->m_File.setFileName(path);
    if (!trace->m_File.open(QIODevice::WriteOnly | QIODevice::Text)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to open pacing trace '%s': %s",
                    qPrintable(path),
                    qPrintable(trace->m_File.errorString()));
        delete trace;
        return nullptr;
    }

    trace->m_Stream.setDevice(&trace->m_File);
    trace->m_Stream << "# Moonlight frame pacing trace\n"
                    << "# display " << displayHz << " Hz (" << trace->m_PeriodUs << " us refresh period), stream "
                    << streamFps << " FPS, " << settings << "\n"
                    << "# All times are microseconds. host_us and smoothed_us are on the host's clock, everything else on ours.\n"
                    << "# host_interval_us, present_interval_us: time since the previous row, on each clock.\n"
                    << "# spacing_error_us = present_interval_us - host_interval_us. Positive: the previous frame stayed\n"
                    << "#   on screen longer than the host held it. This is the stutter figure.\n"
                    << "# late_us = present_us - target_us: how far Present() missed its schedule. Negative is early.\n"
                    << "# hold_us = present_us - arrival_us: time the frame spent in the pacer, drawing included.\n"
                    << "# draw_us: time spent drawing, including waiting for the GPU to finish where the renderer does.\n"
                    << "#   present_call_us: how long the Present() call took. Drawing starts\n"
                    << "#   early, and Present() is timed to return at target_us.\n"
                    << "# tear_line_pct: for a frame presented with tearing less than a refresh period after the previous\n"
                    << "#   one, where down the screen the tear lands if the display was still scanning that frame out.\n"
                    << "#   A variable refresh display that had already finished shows no tear. -1 otherwise.\n"
                    << "# A row with dropped_before > 0 follows frames that were never shown; its intervals are not frame times.\n"
                    << "# host_step: the host stamped the frame well before it appeared, so it was held as long as the frame\n"
                    << "#   before it instead of being paced by its stamp. Pairs touching one are left out of the spacing figures.\n"
                    << "# dequeue_us: when the pacing thread took the frame from the queue. draw_due_us: when drawing was\n"
                    << "#   meant to start; render_start_us - draw_due_us is how late the thread woke for it.\n"
                    << "# drained_before: frames skipped since the previous row to clear a frame waiting in the display's queue.\n"
                    << "# DXGI frame statistics, read by default (ML_PACING_FRAME_STATS=0 turns them off; zeros and -1 when not read):\n"
                    << "#   present_id: the swapchain's present count after this frame. displayed_id, displayed_us: the\n"
                    << "#   latest present the display had shown when this frame was presented, and when.\n"
                    << "#   presentation_mode: how that frame reached the screen: 0 composed by DWM, 1 hardware overlay,\n"
                    << "#   2 none, 3 composition failure, -1 unknown. queued_presents = present_id - displayed_id, this\n"
                    << "#   frame included. display_latency_us: present_us to displayed_us for frame displayed_id, on the\n"
                    << "#   first row that names it.\n"
                    << "frame,host_us,arrival_us,smoothed_us,delay_us,target_us,render_start_us,present_us,"
                       "host_interval_us,present_interval_us,spacing_error_us,late_us,hold_us,draw_us,present_call_us,"
                       "source_interval_us,tear,tear_line_pct,queue_depth,dropped_before,learning,host_step,"
                       "dequeue_us,draw_due_us,present_id,displayed_id,displayed_us,presentation_mode,queued_presents,"
                       "display_latency_us,drained_before\n";

    trace->m_Thread = SDL_CreateThread(PacerTrace::writerThread, "PacerTrace", trace);
    if (trace->m_Thread == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to start pacing trace writer: %s",
                    SDL_GetError());
        delete trace;
        return nullptr;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Recording a pacing trace to %s",
                qPrintable(path));
    return trace;
}

PacerTrace::~PacerTrace()
{
    if (m_Thread != nullptr) {
        m_Lock.lock();
        m_Stopping = true;
        m_RowsAvailable.wakeAll();
        m_Lock.unlock();

        SDL_WaitThread(m_Thread, nullptr);
        m_Thread = nullptr;

        // Whatever the writer had not reached yet, then the summary
        drain();
        writeFooter();
    }

    if (m_File.isOpen()) {
        m_Stream.flush();
        m_File.close();
    }
}

void PacerTrace::record(const PACER_TRACE_ROW& row)
{
    m_Lock.lock();

    if (m_Pending.size() < PACER_TRACE_CAPACITY) {
        m_Pending.append(row);
    }
    else {
        m_DroppedRows++;
    }

    m_Lock.unlock();
    m_RowsAvailable.wakeOne();
}

int PacerTrace::writerThread(void* context)
{
    PacerTrace* me = reinterpret_cast<PacerTrace*>(context);

    for (;;) {
        me->m_Lock.lock();

        while (!me->m_Stopping && me->m_Pending.isEmpty()) {
            me->m_RowsAvailable.wait(&me->m_Lock);
        }

        bool stopping = me->m_Stopping;
        me->m_Lock.unlock();

        me->drain();

        if (stopping) {
            break;
        }
    }

    return 0;
}

void PacerTrace::drain()
{
    m_Lock.lock();
    m_Writing.swap(m_Pending);
    m_Pending.clear();
    m_Lock.unlock();

    for (const PACER_TRACE_ROW& row : std::as_const(m_Writing)) {
        writeRow(row);
    }

    m_Writing.clear();
}

void PacerTrace::writeRow(const PACER_TRACE_ROW& row)
{
    int64_t hostIntervalUs = 0;
    int64_t presentIntervalUs = 0;
    int64_t spacingErrorUs = 0;

    if (m_HavePrevious) {
        hostIntervalUs = row.hostUs - m_Previous.hostUs;
        presentIntervalUs = row.presentUs - m_Previous.presentUs;
        spacingErrorUs = presentIntervalUs - hostIntervalUs;
    }
    else {
        m_FirstPresentUs = row.presentUs;
    }

    int64_t lateUs = row.presentUs - row.targetUs;
    int64_t holdUs = row.presentUs - row.arrivalUs;
    int64_t drawUs = row.drawEndUs - row.renderStartUs;
    int64_t presentCallUs = row.presentUs - row.presentStartUs;

    int tearLinePct = -1;
    if (row.tear && m_HavePrevious && m_PeriodUs > 0 &&
            presentIntervalUs >= 0 && presentIntervalUs < m_PeriodUs) {
        tearLinePct = (int)(100 * presentIntervalUs / m_PeriodUs);
    }

    m_Rows++;
    m_DroppedFrames += row.droppedBefore;

    if (row.tear) {
        m_TornFrames++;
    }
    if (tearLinePct >= 0) {
        m_PossibleTears++;
        m_TearThirds[qMin(2, tearLinePct * 3 / 100)]++;
    }
    if (m_HavePrevious && row.tear != m_Previous.tear) {
        m_TearSwitches++;
    }

    if (row.hostStep) {
        m_HostStepFrames++;
    }

    if (row.learning) {
        m_LearningRows++;
    }
    else {
        m_Hold.add(holdUs);
        m_Draw.add(drawUs);
        m_PresentCall.add(presentCallUs);

        if (lateUs > 1000) {
            m_MissedTargets++;
            m_Late.add(lateUs);
        }

        // Spacing only means a frame time across two consecutive paced frames with
        // nothing dropped between them
        if (m_HavePrevious && !m_Previous.learning && row.droppedBefore == 0 && hostIntervalUs > 0 &&
                !row.hostStep && !m_Previous.hostStep) {
            if (row.intervalUs > 0 && hostIntervalUs > row.intervalUs * PACER_TRACE_STALL_INTERVALS) {
                m_SourceStalls++;
            }
            else {
                m_Spacing.add(spacingErrorUs);

                if (row.intervalUs > 0) {
                    m_HostJitter.add(hostIntervalUs - row.intervalUs);
                }

                if (m_PeriodUs > 0 && spacingErrorUs > m_PeriodUs / 2) {
                    m_LongFrames++;
                }
                else if (m_PeriodUs > 0 && spacingErrorUs < -m_PeriodUs / 2) {
                    m_ShortFrames++;
                }

                int slot = -1;
                for (int i = 0; i < k_WorstPairs; i++) {
                    if (qAbs(spacingErrorUs) > qAbs(m_WorstSpacingUs[i]) &&
                            (slot < 0 || qAbs(m_WorstSpacingUs[i]) < qAbs(m_WorstSpacingUs[slot]))) {
                        slot = i;
                    }
                }
                if (slot >= 0) {
                    m_WorstSpacingUs[slot] = spacingErrorUs;
                    m_WorstFrame[slot] = row.frame;
                }
            }
        }
    }

    int64_t queuedPresents = -1;
    int64_t displayLatencyUs = -1;

    m_QueueDrains += row.drainedBefore;

    if (row.presentId != 0) {
        m_StatsRows++;
        m_ModeCounts[qBound(0, row.presentationMode + 1, 4)]++;

        m_PresentIds[row.presentId % k_PresentHistory] = row.presentId;
        m_PresentTimesUs[row.presentId % k_PresentHistory] = row.presentUs;

        if (row.displayedId != 0 && row.displayedId <= row.presentId) {
            queuedPresents = row.presentId - row.displayedId;
            m_QueuedCounts[qMin<int64_t>(queuedPresents, 4)]++;

            // The first row to name a newly shown frame gives its present-to-display time
            if (row.displayedId != m_LastDisplayedId &&
                    m_PresentIds[row.displayedId % k_PresentHistory] == row.displayedId) {
                displayLatencyUs = row.displayedUs - m_PresentTimesUs[row.displayedId % k_PresentHistory];
                if (displayLatencyUs >= 0) {
                    m_Display.add(displayLatencyUs);
                }
            }

            m_LastDisplayedId = row.displayedId;
        }
    }

    m_Stream << row.frame << ','
             << row.hostUs << ','
             << row.arrivalUs << ','
             << row.smoothedUs << ','
             << row.delayUs << ','
             << row.targetUs << ','
             << row.renderStartUs << ','
             << row.presentUs << ','
             << hostIntervalUs << ','
             << presentIntervalUs << ','
             << spacingErrorUs << ','
             << lateUs << ','
             << holdUs << ','
             << drawUs << ','
             << presentCallUs << ','
             << row.intervalUs << ','
             << (row.tear ? 1 : 0) << ','
             << tearLinePct << ','
             << row.queueDepth << ','
             << row.droppedBefore << ','
             << (row.learning ? 1 : 0) << ','
             << (row.hostStep ? 1 : 0) << ','
             << row.dequeueUs << ','
             << row.drawDueUs << ','
             << row.presentId << ','
             << row.displayedId << ','
             << row.displayedUs << ','
             << (int)row.presentationMode << ','
             << queuedPresents << ','
             << displayLatencyUs << ','
             << (int)row.drainedBefore << '\n';

    m_Previous = row;
    m_HavePrevious = true;
}

void PacerTrace::writeFooter()
{
    if (!m_File.isOpen()) {
        return;
    }

    double seconds = m_HavePrevious ? (m_Previous.presentUs - m_FirstPresentUs) / 1000000.0 : 0;
    uint64_t pacedRows = m_Rows - m_LearningRows;

    m_Stream << "#\n# Summary\n"
             << "# frames presented: " << m_Rows << " over " << seconds << " s (" << m_LearningRows
             << " while learning the host's cadence, " << pacedRows << " paced), rows lost by the writer: "
             << m_DroppedRows << "\n"
             << "# frames never shown (dropped behind a backlog or evicted from a full queue): " << m_DroppedFrames << "\n"
             << "# spacing error |present interval - host interval|, paced pairs: " << m_Spacing.describe() << "\n"
             << "# stutters: " << m_LongFrames << " frames held more than half a refresh longer than the host held them, "
             << m_ShortFrames << " shown more than half a refresh sooner; host stalls excluded: " << m_SourceStalls << "\n"
             << "# host cadence jitter |host interval - median host interval|: " << m_HostJitter.describe() << "\n"
             << "# missed schedule by over 1 ms: " << m_MissedTargets << " frames, lateness " << m_Late.describe() << "\n"
             << "# hold arrival to present: " << m_Hold.describe() << "\n"
             << "# drawing: " << m_Draw.describe() << "\n"
             << "# Present() call: " << m_PresentCall.describe() << "\n"
             << "# presented with tearing: " << m_TornFrames << " of " << m_Rows << ", tearing switched on or off "
             << m_TearSwitches << " times\n"
             << "# possible tears (tearing present within a refresh of the previous frame): " << m_PossibleTears
             << ", tear line top/middle/bottom third: " << m_TearThirds[0] << '/' << m_TearThirds[1] << '/'
             << m_TearThirds[2] << "\n"
             << "# host timestamp steps: " << m_HostStepFrames
             << " frames held as long as the frame before them, left out of the spacing figures\n";

    if (m_StatsRows > 0) {
        m_Stream << "# present to display (DXGI frame statistics, frames it reported shown): " << m_Display.describe() << "\n"
                 << "# presents not yet shown when a frame was presented, that frame included, 0/1/2/3/4+: "
                 << m_QueuedCounts[0] << '/' << m_QueuedCounts[1] << '/' << m_QueuedCounts[2] << '/'
                 << m_QueuedCounts[3] << '/' << m_QueuedCounts[4] << "\n"
                 << "# presentation mode of the latest shown frame, per present: composed " << m_ModeCounts[1]
                 << ", hardware overlay " << m_ModeCounts[2] << ", none " << m_ModeCounts[3]
                 << ", composition failure " << m_ModeCounts[4] << ", unknown " << m_ModeCounts[0] << "\n"
                 << "# display queue drains (one frame skipped to clear a frame left waiting in the display's queue): "
                 << m_QueueDrains << "\n";
    }
    else {
        m_Stream << "# DXGI frame statistics: not read\n";
    }

    int order[k_WorstPairs];
    for (int i = 0; i < k_WorstPairs; i++) {
        order[i] = i;
    }
    std::sort(order, order + k_WorstPairs, [this](int a, int b) {
        return qAbs(m_WorstSpacingUs[a]) > qAbs(m_WorstSpacingUs[b]);
    });

    m_Stream << "# worst spacing errors, signed, with the frame they ended on:\n#  ";
    for (int i = 0; i < k_WorstPairs; i++) {
        if (m_WorstSpacingUs[order[i]] != 0) {
            m_Stream << m_WorstSpacingUs[order[i]] << " us at frame " << m_WorstFrame[order[i]] << "  ";
        }
    }
    m_Stream << '\n';

    m_Stream.flush();
}
