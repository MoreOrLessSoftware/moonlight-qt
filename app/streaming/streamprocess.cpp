#include "streamprocess.h"

#include "backend/nvapp.h"
#include "backend/nvcomputer.h"
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
    // Our window is about to go, so let the new process take the foreground from us
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

    return QFile::exists(siblingExecutable("MoonlightStream"));
#else
    return false;
#endif
}

qint64 launchStream(NvComputer* computer, const NvApp& app, const QPoint& displayOrigin)
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
         << "--gui-pid" << QString::number(QCoreApplication::applicationPid());

    return startDetached(program, args);
}

bool forwardStream(const QStringList& arguments, int* exitCode)
{
#ifdef Q_OS_WIN32
    QString program = siblingExecutable("MoonlightStream");
    if (!QFile::exists(program)) {
        return false;
    }

    // The arguments are ours without the program, and its output is ours too
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments.mid(1));
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start();
    if (!process.waitForStarted(-1)) {
        qWarning() << "Unable to start" << program << ":" << process.errorString();
        return false;
    }

    qInfo() << "Streaming in" << program << "as process" << process.processId();
    AllowSetForegroundWindow((DWORD)process.processId());

    // Launchers watch our process, so we stay until the stream does
    process.waitForFinished(-1);
    *exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return true;
#else
    Q_UNUSED(arguments);
    Q_UNUSED(exitCode);
    return false;
#endif
}

}
