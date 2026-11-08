#include "overlaymenucommands.h"
#include "materialsymbols.h"
#include "path.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPainter>
#include <QSvgRenderer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#define SER_OVERLAYMENUCOMMANDS "overlaymenucommands"

static const char* const k_PostActionNames[] = { "none", "closeMenu", "disconnect", "quit" };

QString OverlayMenuCommand::label() const
{
    return name.trimmed().isEmpty() ? describeKeys(steps) : name;
}

QString OverlayMenuCommand::describeKeys(const QVector<QVector<int>>& steps)
{
    QStringList chords;
    for (const QVector<int>& step : steps) {
        QStringList keys;
        for (int vk : step) {
            keys.append(keyName(vk));
        }
        chords.append(keys.join('+'));
    }
    return chords.join(", ");
}

QString OverlayMenuCommand::keyName(int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return QString(QChar(vk));
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return QCoreApplication::translate("OverlayMenuCommand", "Num %1").arg(vk - 0x60);
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return QString("F%1").arg(vk - 0x70 + 1);
    }

    switch (vk) {
    case 0x08: return QCoreApplication::translate("OverlayMenuCommand", "Backspace");
    case 0x09: return QCoreApplication::translate("OverlayMenuCommand", "Tab");
    case 0x0D: return QCoreApplication::translate("OverlayMenuCommand", "Enter");
    case 0x10:
    case 0xA0: return QCoreApplication::translate("OverlayMenuCommand", "Shift");
    case 0xA1: return QCoreApplication::translate("OverlayMenuCommand", "Right Shift");
    case 0x11:
    case 0xA2: return QCoreApplication::translate("OverlayMenuCommand", "Ctrl");
    case 0xA3: return QCoreApplication::translate("OverlayMenuCommand", "Right Ctrl");
    case 0x12:
    case 0xA4: return QCoreApplication::translate("OverlayMenuCommand", "Alt");
    case 0xA5: return QCoreApplication::translate("OverlayMenuCommand", "Right Alt");
    case 0x5B: return QCoreApplication::translate("OverlayMenuCommand", "Win");
    case 0x5C: return QCoreApplication::translate("OverlayMenuCommand", "Right Win");
    case 0x5D: return QCoreApplication::translate("OverlayMenuCommand", "Menu");
    case 0x13: return QCoreApplication::translate("OverlayMenuCommand", "Pause");
    case 0x14: return QCoreApplication::translate("OverlayMenuCommand", "Caps Lock");
    case 0x1B: return QCoreApplication::translate("OverlayMenuCommand", "Esc");
    case 0x20: return QCoreApplication::translate("OverlayMenuCommand", "Space");
    case 0x21: return QCoreApplication::translate("OverlayMenuCommand", "Page Up");
    case 0x22: return QCoreApplication::translate("OverlayMenuCommand", "Page Down");
    case 0x23: return QCoreApplication::translate("OverlayMenuCommand", "End");
    case 0x24: return QCoreApplication::translate("OverlayMenuCommand", "Home");
    case 0x25: return QCoreApplication::translate("OverlayMenuCommand", "Left");
    case 0x26: return QCoreApplication::translate("OverlayMenuCommand", "Up");
    case 0x27: return QCoreApplication::translate("OverlayMenuCommand", "Right");
    case 0x28: return QCoreApplication::translate("OverlayMenuCommand", "Down");
    case 0x2C: return QCoreApplication::translate("OverlayMenuCommand", "Print Screen");
    case 0x2D: return QCoreApplication::translate("OverlayMenuCommand", "Insert");
    case 0x2E: return QCoreApplication::translate("OverlayMenuCommand", "Delete");
    case 0x6A: return QCoreApplication::translate("OverlayMenuCommand", "Num *");
    case 0x6B: return QCoreApplication::translate("OverlayMenuCommand", "Num +");
    case 0x6D: return QCoreApplication::translate("OverlayMenuCommand", "Num -");
    case 0x6E: return QCoreApplication::translate("OverlayMenuCommand", "Num .");
    case 0x6F: return QCoreApplication::translate("OverlayMenuCommand", "Num /");
    case 0x90: return QCoreApplication::translate("OverlayMenuCommand", "Num Lock");
    case 0x91: return QCoreApplication::translate("OverlayMenuCommand", "Scroll Lock");
    case 0xAD: return QCoreApplication::translate("OverlayMenuCommand", "Mute");
    case 0xAE: return QCoreApplication::translate("OverlayMenuCommand", "Volume Down");
    case 0xAF: return QCoreApplication::translate("OverlayMenuCommand", "Volume Up");
    case 0xB0: return QCoreApplication::translate("OverlayMenuCommand", "Next Track");
    case 0xB1: return QCoreApplication::translate("OverlayMenuCommand", "Previous Track");
    case 0xB2: return QCoreApplication::translate("OverlayMenuCommand", "Stop");
    case 0xB3: return QCoreApplication::translate("OverlayMenuCommand", "Play/Pause");
    case 0xBA: return ";";
    case 0xBB: return "=";
    case 0xBC: return ",";
    case 0xBD: return "-";
    case 0xBE: return ".";
    case 0xBF: return "/";
    case 0xC0: return "`";
    case 0xDB: return "[";
    case 0xDC: return "\\";
    case 0xDD: return "]";
    case 0xDE: return "'";
    default:
        return QString("0x%1").arg(vk, 2, 16, QChar('0')).toUpper().replace("0X", "0x");
    }
}

QString OverlayMenuCommand::iconImagePath(const QString& icon, bool* keepsColors)
{
    QString file;
    bool keep = false;
    if (icon.startsWith("image:")) {
        file = icon.mid(6);
    }
    else if (icon.startsWith("image-color:")) {
        file = icon.mid(12);
        keep = true;
    }

    // Only ever a file in the icons' folder
    file = QFileInfo(file).fileName();
    if (file.isEmpty()) {
        return QString();
    }

    if (keepsColors != nullptr) {
        *keepsColors = keep;
    }
    return Path::getOverlayMenuIconDir() + "/" + file;
}

QImage OverlayMenuCommand::renderIcon(const QString& icon, int size, const QColor& color)
{
    QImage image(qMax(1, size), qMax(1, size), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    bool keepsColors;
    QString path = iconImagePath(icon, &keepsColors);
    if (!path.isEmpty()) {
        // Fitted to the square, keeping its shape
        if (path.endsWith(".svg", Qt::CaseInsensitive)) {
            QSvgRenderer renderer(path);
            if (renderer.isValid()) {
                QSizeF fitted = QSizeF(renderer.defaultSize()).scaled(image.size(), Qt::KeepAspectRatio);
                renderer.render(&painter, QRectF(QPointF((size - fitted.width()) / 2, (size - fitted.height()) / 2), fitted));
            }
        }
        else {
            QImage source(path);
            if (!source.isNull()) {
                source = source.scaled(image.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
                painter.drawImage(QPointF((size - source.width()) / 2.0, (size - source.height()) / 2.0), source);
            }
        }

        // Its shape in the menu's color, like the other icons
        if (!keepsColors) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(image.rect(), color);
        }
    }
    else {
        QString glyph = MaterialSymbols::glyph(icon.startsWith("material:") ? icon.mid(9) : icon);
        if (!glyph.isEmpty() && !MaterialSymbols::family().isEmpty()) {
            QFont font(MaterialSymbols::family());
            font.setPixelSize(qMax(1, size));
            painter.setFont(font);
            painter.setPen(color);
            painter.drawText(image.rect(), Qt::AlignCenter, glyph);
        }
    }

    painter.end();
    return image;
}

QVector<OverlayMenuCommand> OverlayMenuCommand::fromJson(const QByteArray& json)
{
    QVector<OverlayMenuCommand> commands;

    const QJsonArray array = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& value : array) {
        QJsonObject object = value.toObject();

        OverlayMenuCommand command;
        command.name = object.value("name").toString();
        command.icon = object.value("icon").toString();

        QString postAction = object.value("postAction").toString();
        for (int i = 0; i < (int)(sizeof(k_PostActionNames) / sizeof(k_PostActionNames[0])); i++) {
            if (postAction == k_PostActionNames[i]) {
                command.postAction = (PostAction)i;
            }
        }

        for (const QJsonValue& stepValue : object.value("steps").toArray()) {
            QVector<int> step;
            for (const QJsonValue& key : stepValue.toArray()) {
                int vk = key.toInt();
                if (vk > 0 && vk < 0x100) {
                    step.append(vk);
                }
            }
            if (!step.isEmpty()) {
                command.steps.append(step);
            }
        }

        // A command with no keys does nothing
        if (!command.steps.isEmpty()) {
            commands.append(command);
        }
    }

    return commands;
}

QByteArray OverlayMenuCommand::toJson(const QVector<OverlayMenuCommand>& commands)
{
    QJsonArray array;
    for (const OverlayMenuCommand& command : commands) {
        QJsonArray steps;
        for (const QVector<int>& step : command.steps) {
            QJsonArray keys;
            for (int vk : step) {
                keys.append(vk);
            }
            steps.append(keys);
        }

        QJsonObject object;
        object.insert("name", command.name);
        object.insert("icon", command.icon);
        object.insert("postAction", k_PostActionNames[command.postAction]);
        object.insert("steps", steps);
        array.append(object);
    }

    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QVector<OverlayMenuCommand> OverlayMenuCommand::load()
{
    if (qEnvironmentVariableIsSet("ML_OVERLAY_MENU_COMMANDS")) {
        return fromJson(qgetenv("ML_OVERLAY_MENU_COMMANDS"));
    }

    return loadSaved();
}

QVector<OverlayMenuCommand> OverlayMenuCommand::loadSaved()
{
    QSettings settings;
    return fromJson(settings.value(SER_OVERLAYMENUCOMMANDS).toByteArray());
}

void OverlayMenuCommand::save(const QVector<OverlayMenuCommand>& commands)
{
    QSettings settings;
    settings.setValue(SER_OVERLAYMENUCOMMANDS, QString::fromUtf8(toJson(commands)));
}
