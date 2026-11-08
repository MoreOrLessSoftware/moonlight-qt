#include "overlaymenucommandmodel.h"
#include "materialsymbols.h"
#include "path.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QSvgRenderer>
#include <QUuid>

// Larger images than this aren't worth keeping for a 24-pixel icon
#define MAX_ICON_FILE_BYTES (4 * 1024 * 1024)

OverlayMenuCommandModel::OverlayMenuCommandModel(QObject* parent) :
    QAbstractListModel(parent),
    m_Commands(OverlayMenuCommand::loadSaved())
{
}

int OverlayMenuCommandModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_Commands.size();
}

QVariant OverlayMenuCommandModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_Commands.size()) {
        return QVariant();
    }

    const OverlayMenuCommand& command = m_Commands[index.row()];
    switch (role) {
    case NameRole:
        return command.name;
    case LabelRole:
        return command.label();
    case IconRole:
        return command.icon;
    case KeysRole:
        return OverlayMenuCommand::describeKeys(command.steps);
    case PostActionRole:
        return (int)command.postAction;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> OverlayMenuCommandModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { LabelRole, "label" },
        { IconRole, "icon" },
        { KeysRole, "keys" },
        { PostActionRole, "postAction" },
    };
}

QVariantMap OverlayMenuCommandModel::get(int row) const
{
    if (row < 0 || row >= m_Commands.size()) {
        return QVariantMap();
    }

    const OverlayMenuCommand& command = m_Commands[row];

    QVariantList steps;
    for (const QVector<int>& step : command.steps) {
        QVariantList keys;
        for (int vk : step) {
            keys.append(vk);
        }
        steps.append(QVariant(keys));
    }

    return {
        { "name", command.name },
        { "icon", command.icon },
        { "postAction", (int)command.postAction },
        { "steps", steps },
    };
}

OverlayMenuCommand OverlayMenuCommandModel::fromMap(const QVariantMap& map)
{
    OverlayMenuCommand command;
    command.name = map.value("name").toString().trimmed();
    command.icon = map.value("icon").toString();
    command.postAction = (OverlayMenuCommand::PostAction)qBound((int)OverlayMenuCommand::PostActionNone,
                                                                map.value("postAction").toInt(),
                                                                (int)OverlayMenuCommand::PostActionQuit);

    const QVariantList steps = map.value("steps").toList();
    for (const QVariant& stepValue : steps) {
        QVector<int> step;
        const QVariantList keys = stepValue.toList();
        for (const QVariant& key : keys) {
            int vk = key.toInt();
            if (vk > 0 && vk < 0x100) {
                step.append(vk);
            }
        }
        if (!step.isEmpty()) {
            command.steps.append(step);
        }
    }

    return command;
}

void OverlayMenuCommandModel::add(const QVariantMap& command)
{
    beginInsertRows(QModelIndex(), m_Commands.size(), m_Commands.size());
    m_Commands.append(fromMap(command));
    endInsertRows();
    save();
}

void OverlayMenuCommandModel::update(int row, const QVariantMap& command)
{
    if (row < 0 || row >= m_Commands.size()) {
        return;
    }

    m_Commands[row] = fromMap(command);
    emit dataChanged(index(row), index(row));
    save();
}

void OverlayMenuCommandModel::remove(int row)
{
    if (row < 0 || row >= m_Commands.size()) {
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_Commands.removeAt(row);
    endRemoveRows();
    save();
}

void OverlayMenuCommandModel::move(int from, int to)
{
    if (from < 0 || from >= m_Commands.size() || to < 0 || to >= m_Commands.size() || from == to) {
        return;
    }

    // beginMoveRows() takes the row the moved one goes before, as it was before the move
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to);
    m_Commands.move(from, to);
    endMoveRows();
    save();
}

void OverlayMenuCommandModel::save()
{
    // Commands with no keys do nothing, so they aren't kept
    QVector<OverlayMenuCommand> commands;
    for (const OverlayMenuCommand& command : std::as_const(m_Commands)) {
        if (!command.steps.isEmpty()) {
            commands.append(command);
        }
    }
    OverlayMenuCommand::save(commands);

    // Imported images no command uses any more are deleted
    QSet<QString> used;
    for (const OverlayMenuCommand& command : std::as_const(commands)) {
        QString path = OverlayMenuCommand::iconImagePath(command.icon);
        if (!path.isEmpty()) {
            used.insert(QFileInfo(path).fileName());
        }
    }

    QDir dir(Path::getOverlayMenuIconDir());
    const QStringList files = dir.entryList(QDir::Files);
    for (const QString& file : files) {
        if (!used.contains(file)) {
            dir.remove(file);
        }
    }
}

QString OverlayMenuCommandModel::describeKeys(const QVariantList& steps) const
{
    QVariantMap map;
    map.insert("steps", steps);
    return OverlayMenuCommand::describeKeys(fromMap(map).steps);
}

QString OverlayMenuCommandModel::iconFontFamily() const
{
    return MaterialSymbols::family();
}

QString OverlayMenuCommandModel::iconGlyph(const QString& name) const
{
    return MaterialSymbols::glyph(name);
}

QStringList OverlayMenuCommandModel::searchIcons(const QString& query) const
{
    const QStringList& names = MaterialSymbols::names();

    // Names are words joined by underscores, like "sports_esports"
    QStringList words = query.trimmed().toLower().split(QRegularExpression("[\\s_]+"), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        // Icons likely for a command first, then the rest in order (which would start with
        // the likes of "10k" and "123")
        static const QStringList common = {
            "keyboard", "keyboard_command_key", "keyboard_alt", "terminal", "code",
            "screenshot", "screenshot_monitor", "photo_camera", "videocam", "radio_button_checked",
            "mic", "mic_off", "volume_up", "volume_down", "volume_off", "headphones",
            "play_arrow", "pause", "play_pause", "skip_previous", "skip_next", "stop",
            "fast_rewind", "fast_forward", "replay", "refresh", "restart_alt",
            "power_settings_new", "logout", "lock", "home", "apps", "grid_view", "tab",
            "desktop_windows", "monitor", "tv", "fullscreen", "fullscreen_exit", "picture_in_picture",
            "sports_esports", "videogame_asset", "mouse", "touch_app", "settings", "tune",
            "save", "folder", "search", "chat", "forum", "translate", "map", "inventory_2",
            "backpack", "explore", "speed", "timer", "schedule", "star", "favorite", "bolt",
            "rocket_launch", "dark_mode", "light_mode", "close", "check", "undo", "redo",
            "content_copy", "content_paste", "content_cut", "select_all", "swap_horiz", "open_in_new",
        };

        QStringList ordered;
        QSet<QString> seen;
        for (const QString& name : common) {
            if (!MaterialSymbols::glyph(name).isEmpty()) {
                ordered.append(name);
                seen.insert(name);
            }
        }
        for (const QString& name : names) {
            if (!seen.contains(name)) {
                ordered.append(name);
            }
        }
        return ordered;
    }

    // Names starting with the search come first
    QString joined = words.join('_');
    QStringList starting, containing;
    for (const QString& name : names) {
        bool all = true;
        for (const QString& word : std::as_const(words)) {
            if (!name.contains(word)) {
                all = false;
                break;
            }
        }
        if (all) {
            (name.startsWith(joined) ? starting : containing).append(name);
        }
    }

    return starting + containing;
}

QString OverlayMenuCommandModel::iconPreview(const QString& icon, const QString& color, int size) const
{
    QImage image = OverlayMenuCommand::renderIcon(icon, size, QColor(color));

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    return "data:image/png;base64," + QString::fromLatin1(png.toBase64());
}

QString OverlayMenuCommandModel::importIcon(const QUrl& file, bool keepColors)
{
    QString source = file.toLocalFile();
    QFileInfo info(source);
    if (!info.isFile() || info.size() > MAX_ICON_FILE_BYTES) {
        return QString();
    }

    // Only files that read as images, kept with their own (lowercase) extension
    QString suffix = info.suffix().toLower();
    if (suffix == "svg") {
        if (!QSvgRenderer(source).isValid()) {
            return QString();
        }
    }
    else {
        QImageReader reader(source);
        if (!reader.canRead()) {
            return QString();
        }
        if (suffix.isEmpty()) {
            suffix = QString::fromLatin1(reader.format());
        }
    }

    QDir dir(Path::getOverlayMenuIconDir());
    if (!dir.mkpath(".")) {
        return QString();
    }

    QString name = QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + suffix;
    if (!QFile::copy(source, dir.filePath(name))) {
        return QString();
    }

    return (keepColors ? "image-color:" : "image:") + name;
}

bool OverlayMenuCommandModel::canImportIcons() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return true;
#else
    return false;
#endif
}
