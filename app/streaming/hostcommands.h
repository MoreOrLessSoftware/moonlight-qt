#pragma once

#include "backend/nvaddress.h"

#include <QByteArray>
#include <QSslCertificate>
#include <QString>
#include <QVector>

// Commands the host offers for the overlay menu, which run on the host (Sunshine's client
// commands): its /clientcommands answer, and /runclientcommand to run one.

// The host's answers, posted to the main thread as an SDL user event with the result in data1
// (to be deleted there) and the session's token in data2
#define SDL_CODE_HOST_COMMANDS_FETCHED 109
#define SDL_CODE_HOST_COMMAND_RAN 110

struct HostCommand
{
    QString id;
    QString label;
    // A Material Symbols icon as "material:<name>", or empty. Other kinds of icon from the
    // host are dropped, since they'd name files on this computer.
    QString icon;
    // Whether the menu closes once the command has run
    bool closeMenu = true;
};

struct HostCommandGroup
{
    // Where it comes from on the host, like "app"
    QString kind;
    QString label;
    QString icon;
    QVector<HostCommand> commands;
};

struct HostCommands
{
    QVector<HostCommand> commands;
    QVector<HostCommandGroup> groups;

    bool isEmpty() const { return commands.isEmpty() && groups.isEmpty(); }

    // Reads the host's /clientcommands answer, returning false if it isn't one
    static bool fromJson(const QByteArray& json, HostCommands& out);
};

// How to reach the host, copied from its NvComputer for a worker thread
struct HostCommandsTarget
{
    NvAddress address;
    uint16_t httpsPort = 0;
    QSslCertificate serverCert;
    bool useTrueUid = true;
};

struct HostCommandsFetchResult
{
    // The host doesn't offer client commands (an answer that isn't one, like a 404)
    bool unsupported = false;
    // Whether commands came back (a failed request keeps what the menu has)
    bool ok = false;
    HostCommands commands;
};

struct HostCommandRunResult
{
    QString id;
    bool ok = false;
    // Why it didn't run
    QString message;
};

namespace HostCommandsClient
{
    // Each runs the request on a pool thread and posts its result (see above)
    void fetch(const HostCommandsTarget& target, int token);
    void run(const HostCommandsTarget& target, const QString& id, int token);
}
