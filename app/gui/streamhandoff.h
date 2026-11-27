#pragma once

#include <QObject>
#include <QWindow>

class QWinEventNotifier;
class Session;

// Hands a stream the GUI was about to start to the stream process (see StreamProcess),
// and tells the GUI when it ends
class StreamHandoff : public QObject
{
    Q_OBJECT

public:
    // allowed is false when this process is itself streaming from the command line
    explicit StreamHandoff(bool allowed, QObject* parent = nullptr);
    ~StreamHandoff();

    Q_PROPERTY(bool enabled READ isEnabled CONSTANT)
    bool isEnabled() const;

    // Starts the stream process for the session's app, on the display the window is on.
    // The session is never started. streamFinished() follows once this succeeds.
    Q_INVOKABLE bool launch(Session* session, QWindow* window);

signals:
    // The stream process exited. quitAndExit is set when the user quit Moonlight from the
    // stream, so the GUI should exit too.
    void streamFinished(bool quitAndExit);

private:
    void onProcessExited();

    bool m_Enabled;
    void* m_ProcessHandle;
    QWinEventNotifier* m_ProcessNotifier;
};
