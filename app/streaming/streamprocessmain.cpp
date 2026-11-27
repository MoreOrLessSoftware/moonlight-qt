#include "streamprocessmain.h"

#include "session.h"
#include "streamprocess.h"
#include "backend/computermanager.h"
#include "cli/commandlineparser.h"
#include "cli/startstream.h"
#include "settings/streamingpreferences.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLocale>
#include <QTimer>

#ifdef Q_OS_WIN32
#include <Windows.h>
#include <dwmapi.h>
#endif

// How long our window stays up for the GUI to show its own after the stream
#define GUI_SHOW_TIMEOUT_MS 3000

// Launch warnings are shown this long before the stream starts, like the GUI's toasts
#define LAUNCH_WARNING_DELAY_MS 3500

int StreamProcessController::run(QGuiApplication& app)
{
    StreamingPreferences* preferences = StreamingPreferences::get();
    StreamCommandLineParser streamParser;
    streamParser.parse(app.arguments(), preferences);

    QPoint displayOrigin;
    bool hasDisplayOrigin = streamParser.getDisplayOrigin(&displayOrigin);

    StreamProcessController controller(preferences, streamParser.getHost(), streamParser.getAppName(),
                                       streamParser.getAppId(), hasDisplayOrigin ? &displayOrigin : nullptr,
                                       streamParser.getGuiPid());
    if (!controller.start()) {
        return -1;
    }

    return app.exec();
}

StreamProcessController::StreamProcessController(StreamingPreferences* preferences, const QString& host,
                                                 const QString& appName, int appId, const QPoint* displayOrigin,
                                                 qint64 guiPid)
    : m_Preferences(preferences),
      m_Host(host),
      m_AppName(appName),
      m_AppId(appId),
      m_HasDisplayOrigin(displayOrigin != nullptr),
      m_GuiPid(guiPid),
      m_ExitRequested(false),
      m_State(StateLaunching),
      m_Launcher(nullptr),
      m_Session(nullptr),
      m_Finished(false)
{
    if (displayOrigin != nullptr) {
        m_DisplayOrigin = *displayOrigin;
    }
}

bool StreamProcessController::start()
{
    if (!m_Window.create(m_Preferences, m_HasDisplayOrigin ? &m_DisplayOrigin : nullptr)) {
        return false;
    }

    connect(&m_Window, &LaunchWindow::canceled, this, &StreamProcessController::onCanceled);
    connect(&m_Window, &LaunchWindow::finished, this, [this](LaunchWindow::Choice choice) {
        switch (m_State) {
        case StateQuitPrompt:
            if (choice == LaunchWindow::ChoiceAccept) {
                m_State = StateQuitting;
                m_Window.showStatus(tr("Quitting %1...").arg(m_AppName));
                m_Launcher->quitRunningApp();
            }
            else {
                finish();
            }
            break;
        case StateFinishing:
            finish();
            break;
        default:
            break;
        }
    });

    m_Launcher = new CliStartStream::Launcher(m_Host, m_AppName, m_Preferences, this);
    m_Launcher->setAppId(m_AppId);
    if (m_HasDisplayOrigin) {
        m_Launcher->setDisplayOrigin(m_DisplayOrigin);
    }
    connect(m_Launcher, &CliStartStream::Launcher::searchingComputer, this, &StreamProcessController::onSearchingComputer);
    connect(m_Launcher, &CliStartStream::Launcher::searchingApp, this, &StreamProcessController::onSearchingApp);
    connect(m_Launcher, &CliStartStream::Launcher::appQuitRequired, this, &StreamProcessController::onAppQuitRequired);
    connect(m_Launcher, &CliStartStream::Launcher::failed, this, &StreamProcessController::onLaunchFailed);
    connect(m_Launcher, &CliStartStream::Launcher::sessionCreated, this, &StreamProcessController::onSessionCreated);
    m_Launcher->execute(new ComputerManager(m_Preferences));
    return true;
}

void StreamProcessController::onSearchingComputer()
{
    m_Window.showStatus(tr("Establishing connection to PC..."));
}

void StreamProcessController::onSearchingApp()
{
    m_Window.showStatus(tr("Loading app list..."));
}

void StreamProcessController::onAppQuitRequired(QString appName)
{
    m_State = StateQuitPrompt;
    m_AppName = appName;
    m_Window.showQuestion(tr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName));
}

void StreamProcessController::onLaunchFailed(QString text)
{
    qWarning() << text;
    finishWithError(text);
}

void StreamProcessController::onSessionCreated(QString appName, Session* session)
{
    m_State = StateStarting;
    m_AppName = appName;
    m_Session = session;

    connect(session, &Session::stageStarting, this, &StreamProcessController::onStageStarting);
    connect(session, &Session::stageFailed, this, &StreamProcessController::onStageFailed);
    connect(session, &Session::displayLaunchError, this, &StreamProcessController::onDisplayLaunchError);
    connect(session, &Session::connectionStarted, this, &StreamProcessController::onConnectionStarted);
    connect(session, &Session::quitStarting, this, &StreamProcessController::onQuitStarting);
    connect(session, &Session::sessionFinished, this, &StreamProcessController::onSessionFinished);
    connect(session, &Session::readyForDeletion, session, &QObject::deleteLater);

    bool isResume = session->getComputer()->currentGameId == session->getApp().id;
    m_Window.showStatus(isResume ? tr("Resuming %1...").arg(appName) : tr("Starting %1...").arg(appName),
                        QStringList(), streamHint());

    // Checking the decoders keeps the main thread busy for a moment, so the card goes up first
    m_Window.paintNow();

    // The session streams in our window, and tells us before it lets go of it
    session->setWindow(m_Window.getWindow());
    session->setWindowReleaseHandler([this] { m_Window.prepareWindowRelease(); });
    if (!session->initialize(nullptr)) {
        // It emitted displayLaunchError() with the reason
        onSessionFinished(0);
        session->deleteLater();
        m_Session = nullptr;
        return;
    }

    // Show any warnings for a moment before starting, like the GUI does
    QStringList warnings = session->property("launchWarnings").toStringList();
    for (const QString& warning : std::as_const(warnings)) {
        qWarning() << warning;
    }
    if (!warnings.isEmpty()) {
        m_Window.showStatus(isResume ? tr("Resuming %1...").arg(appName) : tr("Starting %1...").arg(appName),
                            warnings, streamHint());
    }

    QTimer::singleShot(warnings.isEmpty() ? 0 : LAUNCH_WARNING_DELAY_MS, this, [this, session] {
        // Canceled while showing the warnings
        if (m_State != StateStarting || m_Session != session) {
            return;
        }
        session->start();
    });
}

void StreamProcessController::onStageStarting(QString stage)
{
    if (m_State == StateStarting) {
        m_Window.showStatus(tr("Starting %1...").arg(stage), QStringList(), streamHint());
    }
}

void StreamProcessController::onStageFailed(QString stage, int errorCode, QString failingPorts)
{
    // Shown when the session finishes
    m_ErrorText = tr("Starting %1 failed: Error %2").arg(stage).arg(errorCode);
    if (!failingPorts.isEmpty()) {
        m_ErrorText += "\n\n" + tr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts);
    }
}

void StreamProcessController::onDisplayLaunchError(QString text)
{
    qWarning() << text;
    m_ErrorText = text;
}

void StreamProcessController::onConnectionStarted()
{
    // The session's renderer takes the window over
    m_State = StateStreaming;
    m_Window.suspend(true);
}

void StreamProcessController::onQuitStarting()
{
    m_State = StateEnding;

    // The card first, so the window shows it as soon as we have it back
    m_Window.showStatus(tr("Quitting %1...").arg(m_AppName));
    m_Window.resume();
}

void StreamProcessController::onSessionFinished(int portTestResult)
{
    // The session quits the application itself right after this, but we exit first
    if (m_Session != nullptr && m_Session->isExitRequested()) {
        m_ExitRequested = true;
        finish();
        return;
    }

    m_State = StateEnding;

    if (portTestResult != 0 && portTestResult != -1 && !m_ErrorText.isEmpty()) {
        m_ErrorText += "\n\n" + tr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.");
    }

    finishWithError(m_ErrorText);
}

void StreamProcessController::onCanceled()
{
    switch (m_State) {
    case StateLaunching:
        finish();
        break;
    case StateStarting:
        if (m_Session != nullptr) {
            // The session finishes when the connection stops (or if it never started)
            m_Window.showStatus(tr("Canceling..."));
            m_Window.suspend();
            m_State = StateEnding;
            if (Session::get() == m_Session) {
                m_Session->interrupt();
            }
            else {
                // Still showing the warnings, so it hasn't started
                m_Session->deleteLater();
                m_Session = nullptr;
                finish();
            }
        }
        break;
    case StateQuitting:
        // The host quits the app anyway, and we're done
        finish();
        break;
    default:
        // The session ending: wait for it
        break;
    }
}

void StreamProcessController::finishWithError(const QString& text)
{
    if (text.isEmpty()) {
        finish();
        return;
    }

    m_State = StateFinishing;
    m_Window.showMessage(text);
    m_Window.resume();
}

void StreamProcessController::finish()
{
    if (m_Finished) {
        return;
    }
    m_Finished = true;

    // Quit and exit (Ctrl+Alt+Shift+E) closes the GUI too. From the command line, it's
    // just the end of the stream.
    int exitCode = m_ExitRequested && m_GuiPid != 0 ? StreamProcess::k_ExitCodeQuitAndExit : 0;

#ifdef Q_OS_WIN32
    if (m_GuiPid != 0 && !m_ExitRequested) {
        // The GUI is waiting to show itself again. We still have the foreground, so we
        // can give it to the GUI, and our window stays up until the GUI's is, so there's
        // no moment with neither.
        AllowSetForegroundWindow((DWORD)m_GuiPid);

        HANDLE endingEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                        (LPCWSTR)StreamProcess::streamEndingEventName(m_GuiPid).utf16());
        if (endingEvent != nullptr) {
            SetEvent(endingEvent);
            CloseHandle(endingEvent);

            auto waited = new QElapsedTimer();
            waited->start();
            auto timer = new QTimer(this);
            connect(timer, &QTimer::timeout, this, [this, timer, waited, exitCode] {
                if (StreamProcess::hasVisibleWindow(m_GuiPid) || waited->hasExpired(GUI_SHOW_TIMEOUT_MS)) {
                    timer->stop();
                    delete waited;

                    // Shown isn't on the screen until the compositor's next frame
                    DwmFlush();
                    DwmFlush();
                    QCoreApplication::exit(exitCode);
                }
            });
            timer->start(10);
            return;
        }
    }
#endif

    QCoreApplication::exit(exitCode);
}

QString StreamProcessController::streamHint() const
{
    QString trigger;
    switch (m_Preferences->overlayMenuTrigger) {
    case StreamingPreferences::OMT_SELECT: trigger = tr("Select"); break;
    case StreamingPreferences::OMT_START: trigger = tr("Start"); break;
    case StreamingPreferences::OMT_GUIDE: trigger = tr("Guide"); break;
    case StreamingPreferences::OMT_LB_RB: trigger = tr("LB+RB"); break;
    default: break;
    }

    // Unlike the GUI, we don't know whether there's a gamepad until the stream starts
    QString hint;
    if (!trigger.isEmpty()) {
        QString seconds = QLocale().toString(m_Preferences->overlayMenuHoldMs / 1000.0, 'f', 1);
        hint = tr("Hold %1 for %2 seconds or press %3 to open the in-stream menu").arg(trigger, seconds, tr("Ctrl+Alt+Shift+O"));
    }
    else {
        hint = tr("Press %1 to open the in-stream menu").arg(tr("Ctrl+Alt+Shift+O"));
    }

    return tr("Tip:") + " " + hint;
}
