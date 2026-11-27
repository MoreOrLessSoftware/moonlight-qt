#pragma once

#include <QPoint>
#include <QString>

class NvComputer;
class NvApp;

// Running the stream in its own process: the GUI starts it and exits, and the stream
// process starts the GUI again when the stream ends.
namespace StreamProcess
{
    // Whether the GUI should hand streams off. Only for portable Windows installs, which ship
    // the stream executable, unless MOONLIGHT_STREAM_PROCESS=1 or 0 says otherwise.
    bool isHandoffEnabled();

    // Starts the stream process for an app on a host, on the display at displayOrigin
    // (the top left corner of the GUI's screen). The caller exits once this succeeds.
    bool launchStream(NvComputer* computer, const NvApp& app, const QPoint& displayOrigin);

    // Starts the GUI again after a stream, showing the apps of the host it streamed from
    bool relaunchGui(const QString& hostUuid);
}
