#pragma once

#include "launchwindow.h"

#include <QObject>
#include <QPoint>
#include <QString>

class QGuiApplication;
class Session;
class StreamingPreferences;

namespace CliStartStream {
class Launcher;
}

// The stream process: "stream <host> <app>" without the QML GUI. It finds the host and app
// like the command line stream does, then streams in a window that shows the launch's
// progress, questions and errors itself (LaunchWindow). Windows only.
class StreamProcessController : public QObject
{
    Q_OBJECT

public:
    // Runs the stream process for these command line arguments, returning its exit code
    static int run(QGuiApplication& app);

private:
    StreamProcessController(StreamingPreferences* preferences, const QString& host,
                            const QString& appName, int appId, const QPoint* displayOrigin,
                            bool returnToGui);

    bool start();

    void onSearchingComputer();
    void onSearchingApp();
    void onAppQuitRequired(QString appName);
    void onLaunchFailed(QString text);
    void onSessionCreated(QString appName, Session* session);

    void onStageStarting(QString stage);
    void onStageFailed(QString stage, int errorCode, QString failingPorts);
    void onDisplayLaunchError(QString text);
    void onConnectionStarted();
    void onQuitStarting();
    void onSessionFinished(int portTestResult);

    void onCanceled();

    // Shows the error, if there was one, and then finishes
    void finishWithError(const QString& text);
    void finish();

    QString streamHint() const;

    enum State {
        StateLaunching,     // Finding the host and app
        StateQuitPrompt,    // Asking whether to quit the app that's running
        StateQuitting,      // Quitting it
        StateStarting,      // The session is starting
        StateStreaming,     // The session's renderer owns the window
        StateEnding,        // The session is ending
        StateFinishing,     // Showing an error before we exit
    };

    StreamingPreferences* m_Preferences;
    QString m_Host;
    QString m_AppName;
    int m_AppId;
    QPoint m_DisplayOrigin;
    bool m_HasDisplayOrigin;
    bool m_ReturnToGui;

    State m_State;
    LaunchWindow m_Window;
    CliStartStream::Launcher* m_Launcher;
    Session* m_Session;
    QString m_ErrorText;
    bool m_Finished;
};
