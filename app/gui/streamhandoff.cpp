#include "streamhandoff.h"

#include "settings/streamingpreferences.h"
#include "streaming/session.h"
#include "streaming/streamprocess.h"

#include <QScreen>

#ifdef Q_OS_WIN32
#include <QWinEventNotifier>
#include <Windows.h>
#endif

StreamHandoff::StreamHandoff(bool allowed, QObject* parent)
    : QObject(parent),
      m_Enabled(allowed && StreamProcess::isHandoffEnabled()),
      m_ProcessHandle(nullptr),
      m_ProcessNotifier(nullptr)
{
}

StreamHandoff::~StreamHandoff()
{
#ifdef Q_OS_WIN32
    delete m_ProcessNotifier;
    if (m_ProcessHandle != nullptr) {
        CloseHandle(m_ProcessHandle);
    }
#endif
}

bool StreamHandoff::isEnabled() const
{
    return m_Enabled;
}

bool StreamHandoff::launch(Session* session, QWindow* window)
{
#ifdef Q_OS_WIN32
    if (!m_Enabled || session == nullptr || m_ProcessHandle != nullptr) {
        return false;
    }

    QPoint displayOrigin;
    if (window != nullptr && window->screen() != nullptr) {
        displayOrigin = window->screen()->geometry().topLeft();
    }

    // The stream process reads the settings from disk
    StreamingPreferences::get()->save();

    qint64 pid = StreamProcess::launchStream(session->getComputer(), session->getApp(), displayOrigin);
    if (pid == 0) {
        return false;
    }

    // We wait for it with its handle rather than QProcess, which would give it our
    // standard handles (and it only logs to a file without them)
    m_ProcessHandle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (m_ProcessHandle == nullptr) {
        qWarning() << "Unable to watch the stream process:" << GetLastError();

        // It's running, so the GUI hides, but it has to come back on its own
        QMetaObject::invokeMethod(this, [this] { emit streamFinished(false); }, Qt::QueuedConnection);
        return true;
    }

    m_ProcessNotifier = new QWinEventNotifier(m_ProcessHandle, this);
    connect(m_ProcessNotifier, &QWinEventNotifier::activated, this, &StreamHandoff::onProcessExited);
    return true;
#else
    Q_UNUSED(session);
    Q_UNUSED(window);
    return false;
#endif
}

void StreamHandoff::onProcessExited()
{
#ifdef Q_OS_WIN32
    DWORD exitCode = 0;
    GetExitCodeProcess(m_ProcessHandle, &exitCode);
    qInfo() << "Stream process exited with" << exitCode;

    m_ProcessNotifier->setEnabled(false);
    m_ProcessNotifier->deleteLater();
    m_ProcessNotifier = nullptr;
    CloseHandle(m_ProcessHandle);
    m_ProcessHandle = nullptr;

    emit streamFinished(exitCode == (DWORD)StreamProcess::k_ExitCodeQuitAndExit);
#endif
}
