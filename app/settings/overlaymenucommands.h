#pragma once

#include <QString>
#include <QVector>

// A custom command in the in-stream menu, as on the Android client: keys pressed on the host,
// then optionally something more (like closing the menu)
struct OverlayMenuCommand
{
    enum PostAction {
        // The menu stays open, for sending more
        PostActionNone,
        PostActionCloseMenu,
        PostActionDisconnect,
        PostActionQuit,
    };

    QString name;
    // "material:<name>" for a Material Symbols icon, or empty for none
    QString icon;
    // Chords pressed one after another. Each is Windows virtual key codes, pressed in order and
    // released in reverse (so modifiers go first).
    QVector<QVector<int>> steps;
    PostAction postAction = PostActionNone;

    // Its name, or its keys if it has none
    QString label() const;

    // Like "Ctrl+Shift+Esc, Win+R"
    static QString describeKeys(const QVector<QVector<int>>& steps);
    static QString keyName(int vk);

    // The saved commands. ML_OVERLAY_MENU_COMMANDS (in the saved format, JSON) replaces them
    // for testing.
    static QVector<OverlayMenuCommand> load();
    static void save(const QVector<OverlayMenuCommand>& commands);

    static QVector<OverlayMenuCommand> fromJson(const QByteArray& json);
    static QByteArray toJson(const QVector<OverlayMenuCommand>& commands);
};
