#pragma once

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class NvComputer;
class NvApp;

// Running the stream in its own process: the GUI starts it and exits, and the stream
// process starts the GUI again when the stream ends.
namespace StreamProcess
{
    // Whether the GUI should hand streams off: on Windows, when the stream executable is next
    // to us, unless MOONLIGHT_STREAM_PROCESS=1 or 0 says otherwise.
    bool isHandoffEnabled();

    // Starts the stream process for an app on a host, on the display at displayOrigin
    // (the top left corner of the GUI's screen). The caller exits once this succeeds.
    bool launchStream(NvComputer* computer, const NvApp& app, const QPoint& displayOrigin);

    // Runs a command line stream in the stream executable, if there is one, waiting for
    // it to finish. Returns false without it.
    bool forwardStream(const QStringList& arguments, int* exitCode);

    // Starts the GUI again after a stream, showing the apps of the host it streamed from.
    // Returns its process ID, or 0 if it didn't start.
    qint64 relaunchGui(const QString& hostUuid);

    // Whether the process has shown a window yet
    bool hasVisibleWindow(qint64 pid);
}
