#pragma once

#include <QPoint>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class NvComputer;
class NvApp;

// Running the stream in its own process: the GUI starts it, hides and waits for it to end.
namespace StreamProcess
{
    // Whether the GUI should hand streams off: on Windows, when the stream executable is next
    // to us, unless MOONLIGHT_STREAM_PROCESS=1 or 0 says otherwise.
    bool isHandoffEnabled();

    // Starts the stream process for an app on a host, on the display at displayOrigin
    // (the top left corner of the GUI's screen). The GUI hides while it streams, and shows
    // again when it exits. Returns its process ID, or 0 if it didn't start.
    qint64 launchStream(NvComputer* computer, const NvApp& app, const QPoint& displayOrigin);

    // The stream process's exit code when the user quit Moonlight from the stream
    // (Ctrl+Alt+Shift+Q), so the GUI exits too
    const int k_ExitCodeQuitAndExit = 3;

    // Runs a command line stream in the stream executable, if there is one, waiting for
    // it to finish. Returns false without it.
    bool forwardStream(const QStringList& arguments, int* exitCode);
}
