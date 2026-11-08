#include "hostcommands.h"
#include "backend/nvhttp.h"

#include "SDL_compat.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QUrlQuery>

#define HOST_COMMANDS_TIMEOUT_MS 5000

static HostCommand readCommand(const QJsonObject& object)
{
    HostCommand command;
    command.id = object.value("id").toString();
    command.label = object.value("label").toString();
    command.closeMenu = object.value("close-menu").toBool(true);

    QString icon = object.value("icon").toString();
    if (icon.startsWith("material:")) {
        command.icon = icon;
    }
    return command;
}

static QVector<HostCommand> readCommands(const QJsonArray& array)
{
    QVector<HostCommand> commands;
    for (const QJsonValue& value : array) {
        HostCommand command = readCommand(value.toObject());
        if (!command.id.isEmpty() && !command.label.isEmpty()) {
            commands.append(command);
        }
    }
    return commands;
}

bool HostCommands::fromJson(const QByteArray& json, HostCommands& out)
{
    QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject() || !document.object().contains("version")) {
        return false;
    }

    QJsonObject root = document.object();
    out = {};
    out.commands = readCommands(root.value("commands").toArray());

    for (const QJsonValue& value : root.value("groups").toArray()) {
        QJsonObject object = value.toObject();

        HostCommandGroup group;
        group.kind = object.value("kind").toString();
        group.label = object.value("label").toString();
        QString icon = object.value("icon").toString();
        if (icon.startsWith("material:")) {
            group.icon = icon;
        }
        group.commands = readCommands(object.value("commands").toArray());

        if (!group.label.isEmpty() && !group.commands.isEmpty()) {
            out.groups.append(group);
        }
    }

    return true;
}

static void postResult(Sint32 code, void* result, int token)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = code;
    event.user.data1 = result;
    event.user.data2 = (void*)(intptr_t)token;
    if (SDL_PushEvent(&event) <= 0) {
        // Nobody's left to read it
        if (code == SDL_CODE_HOST_COMMANDS_FETCHED) {
            delete (HostCommandsFetchResult*)result;
        }
        else {
            delete (HostCommandRunResult*)result;
        }
    }
}

void HostCommandsClient::fetch(const HostCommandsTarget& target, int token)
{
    QThreadPool::globalInstance()->start([target, token]() {
        auto result = new HostCommandsFetchResult();

        // Made on this thread, which runs its requests
        NvHTTP http(target.address, target.httpsPort, target.serverCert, target.useTrueUid);
        QByteArray body;
        int status = http.requestHttps("clientcommands", QString(), HOST_COMMANDS_TIMEOUT_MS, body);
        if (status == 200 && HostCommands::fromJson(body, result->commands)) {
            result->ok = true;
        }
        else if (status != 0) {
            // An answer, but not one with commands: the host doesn't have them
            result->unsupported = true;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host commands: status %d, %d commands, %d groups%s",
                    status, (int)result->commands.commands.size(), (int)result->commands.groups.size(),
                    result->unsupported ? " (not offered by this host)" : "");

        postResult(SDL_CODE_HOST_COMMANDS_FETCHED, result, token);
    });
}

void HostCommandsClient::run(const HostCommandsTarget& target, const QString& id, int token)
{
    QThreadPool::globalInstance()->start([target, id, token]() {
        auto result = new HostCommandRunResult();
        result->id = id;

        QUrlQuery query;
        query.addQueryItem("id", id);

        NvHTTP http(target.address, target.httpsPort, target.serverCert, target.useTrueUid);
        QByteArray body;
        int status = http.requestHttps("runclientcommand", query.toString(QUrl::FullyEncoded), HOST_COMMANDS_TIMEOUT_MS, body);

        QJsonObject answer = QJsonDocument::fromJson(body).object();
        if (status == 200 && answer.value("status").toString() == "ok") {
            result->ok = true;
        }
        else {
            result->message = answer.value("message").toString();
            if (result->message.isEmpty()) {
                result->message = status == 0 ? QCoreApplication::translate("OverlayMenu", "No answer from the host")
                                              : QCoreApplication::translate("OverlayMenu", "Failed (%1)").arg(status);
            }
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host command %s: status %d%s%s",
                    qPrintable(id), status,
                    result->ok ? "" : ", ", qPrintable(result->message));

        postResult(SDL_CODE_HOST_COMMAND_RAN, result, token);
    });
}
