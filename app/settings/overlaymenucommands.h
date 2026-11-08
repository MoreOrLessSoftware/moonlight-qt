#pragma once

#include <QColor>
#include <QImage>
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
    // "material:<name>" for a Material Symbols icon, "image:<file>" for an image imported into
    // Path::getOverlayMenuIconDir() and drawn in the menu's colours, "image-color:<file>" for
    // one drawn in its own, or empty for none
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

    // An image icon's file, and whether it keeps its own colours. Empty for any other icon.
    static QString iconImagePath(const QString& icon, bool* keepsColors = nullptr);

    // Draws an icon (as above, or a bare Material Symbols name) in a square of the given size,
    // in the given color unless it's an image that keeps its own. Transparent if it can't be
    // drawn. Main thread.
    static QImage renderIcon(const QString& icon, int size, const QColor& color);

    // The saved commands. ML_OVERLAY_MENU_COMMANDS (in the saved format, JSON) replaces them
    // for testing.
    static QVector<OverlayMenuCommand> load();
    // The saved commands only, for editing them
    static QVector<OverlayMenuCommand> loadSaved();
    static void save(const QVector<OverlayMenuCommand>& commands);

    static QVector<OverlayMenuCommand> fromJson(const QByteArray& json);
    static QByteArray toJson(const QVector<OverlayMenuCommand>& commands);
};
