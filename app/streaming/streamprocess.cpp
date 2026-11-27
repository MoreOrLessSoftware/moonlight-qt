#include "streamprocess.h"

#include "backend/nvapp.h"
#include "backend/nvcomputer.h"
#include "path.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>

#ifdef Q_OS_WIN32
#include <Windows.h>
#endif

namespace StreamProcess
{

static QString siblingExecutable(const QString& baseName)
{
#ifdef Q_OS_WIN32
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(baseName + ".exe");
#else
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(baseName);
#endif
}

static qint64 startDetached(const QString& program, const QStringList& args)
{
    // The working directory is ours, which is where portable.dat is looked for
    qint64 pid = 0;
    if (!QProcess::startDetached(program, args, QDir::currentPath(), &pid)) {
        qWarning() << "Unable to start" << program << args;
        return 0;
    }

    qInfo() << "Started" << program << args << "as process" << pid;

#ifdef Q_OS_WIN32
    // We're about to exit, so let the new process take the foreground from us
    AllowSetForegroundWindow((DWORD)pid);
#endif

    return pid;
}

bool isHandoffEnabled()
{
#ifdef Q_OS_WIN32
    bool enabled;
    if (Utils::getEnvironmentVariableOverride("MOONLIGHT_STREAM_PROCESS", &enabled)) {
        return enabled;
    }

    return Path::isPortable() && QFile::exists(siblingExecutable("MoonlightStream"));
#else
    return false;
#endif
}

bool launchStream(NvComputer* computer, const NvApp& app, const QPoint& displayOrigin)
{
    QString program = siblingExecutable("MoonlightStream");
    QStringList args;

    // Without the stream executable (MOONLIGHT_STREAM_PROCESS=1 in a build tree),
    // we run the stream mode of this one
    if (!QFile::exists(program)) {
        program = QCoreApplication::applicationFilePath();
        args << "--stream-process";
    }

    QString uuid;
    {
        QReadLocker lock(&computer->lock);
        uuid = computer->uuid;
    }

    args << "stream" << uuid << app.name
         << "--app-id" << QString::number(app.id)
         << "--display-origin" << QString("%1,%2").arg(displayOrigin.x()).arg(displayOrigin.y())
         << "--return-to-gui";

    return startDetached(program, args) != 0;
}

qint64 relaunchGui(const QString& hostUuid)
{
    QString program = siblingExecutable("Moonlight");
    if (!QFile::exists(program)) {
        program = QCoreApplication::applicationFilePath();
    }

    QStringList args;
    if (!hostUuid.isEmpty()) {
        args << "--select-host" << hostUuid;
    }

    return startDetached(program, args);
}

#ifdef Q_OS_WIN32
struct WindowSearch {
    DWORD pid;
    bool found;
};

static BOOL CALLBACK findVisibleWindow(HWND hwnd, LPARAM param)
{
    auto search = (WindowSearch*)param;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == search->pid && IsWindowVisible(hwnd)) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}
#endif

bool hasVisibleWindow(qint64 pid)
{
#ifdef Q_OS_WIN32
    WindowSearch search = { (DWORD)pid, false };
    EnumWindows(findVisibleWindow, (LPARAM)&search);
    return search.found;
#else
    Q_UNUSED(pid);
    return true;
#endif
}

}
