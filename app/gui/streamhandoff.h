#pragma once

#include <QObject>
#include <QWindow>

class Session;

// Hands a stream the GUI was about to start to the stream process (see StreamProcess)
class StreamHandoff : public QObject
{
    Q_OBJECT

public:
    // allowed is false when this process is itself streaming from the command line
    explicit StreamHandoff(bool allowed, QObject* parent = nullptr);

    Q_PROPERTY(bool enabled READ isEnabled CONSTANT)
    bool isEnabled() const;

    // Starts the stream process for the session's app, on the display the window is on.
    // The GUI quits once this succeeds, and the session is never started.
    Q_INVOKABLE bool launch(Session* session, QWindow* window);

private:
    bool m_Enabled;
};
