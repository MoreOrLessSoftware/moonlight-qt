#pragma once

#include "settings/overlaymenucommands.h"

#include <QAbstractListModel>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// The in-stream menu's custom commands, for the settings page that edits them. Every change is
// saved straight away. A command goes to and from QML as a map of name, icon, postAction (an
// OverlayMenuCommand::PostAction) and steps (lists of Windows virtual key codes).
class OverlayMenuCommandModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        LabelRole,
        IconRole,
        KeysRole,
        PostActionRole,
    };

    explicit OverlayMenuCommandModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void add(const QVariantMap& command);
    Q_INVOKABLE void update(int row, const QVariantMap& command);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void move(int from, int to);

    // Like "Ctrl+Shift+Esc, Win+R", for a command's steps
    Q_INVOKABLE QString describeKeys(const QVariantList& steps) const;

    // The Material Symbols font's family, and the text that draws an icon in it
    Q_INVOKABLE QString iconFontFamily() const;
    Q_INVOKABLE QString iconGlyph(const QString& name) const;

    // The Material Symbols icons whose names contain the words searched for, best matches
    // first, or all of them for an empty search
    Q_INVOKABLE QStringList searchIcons(const QString& query) const;

    // An icon (see OverlayMenuCommand::icon) drawn in a color, as a data URL for an Image
    Q_INVOKABLE QString iconPreview(const QString& icon, const QString& color, int size) const;

    // Copies an image file (SVG, PNG and the like) into the icons' folder, returning its icon,
    // or empty if it isn't an image that can be read
    Q_INVOKABLE QString importIcon(const QUrl& file, bool keepColors);

    // Whether QML has a file dialog to import an image with (QtQuick.Dialogs, Qt 6)
    Q_INVOKABLE bool canImportIcons() const;

private:
    static OverlayMenuCommand fromMap(const QVariantMap& map);
    void save();

    QVector<OverlayMenuCommand> m_Commands;
};
