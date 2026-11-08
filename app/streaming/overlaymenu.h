#pragma once

#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "SDL_compat.h"
#include "video/overlaymanager.h"

class QPainter;
class QRectF;

// The in-stream menu, ported from the Android client's OverlayMenuView: a column of buttons
// going up from the bottom left corner, and a row of custom commands and Close running right
// along the bottom.
//
// It's drawn as an image overlay (Overlay::OverlayMenu), only on renderers that support one.
// It takes no input of its own: the input handler moves the selection and runs the selected
// action. Main thread only.
class OverlayMenu
{
public:
    enum Action {
        ActionNone,
        ActionGuide,
        ActionMouseEmulation,
        ActionFullScreen,
        ActionPasteText,
        ActionStats,
        ActionJitterTolerance,
        ActionQuit,
        ActionDisconnect,
        ActionClose,
        // Back from options to the main buttons
        ActionBack,
        // Custom command n is ActionCustomBase + n
        ActionCustomBase = 100,
        // Option n of the options showing is ActionOptionBase + n
        ActionOptionBase = 200,
    };

    enum Direction {
        DirectionUp,
        DirectionDown,
        DirectionLeft,
        DirectionRight,
    };

    explicit OverlayMenu(Overlay::OverlayManager& overlays);

    // Called after each change to the menu, to have the renderer draw it even if the host
    // sends no new frames
    void setRedrawHandler(std::function<void()> handler);

    // Called once the menu has closed, however it closed
    void setClosedHandler(std::function<void()> handler);

    // Whether the Guide and Mouse buttons are shown, which need a gamepad. Takes effect the
    // next time the menu opens.
    void setShowGamepadButtons(bool show);

    // The custom commands along the row, as icons (see OverlayMenuCommand::icon) and labels.
    // Takes effect the next time the menu opens.
    void setCustomCommands(const QVector<QPair<QString, QString>>& commands);

    // Whether the Jitter Tolerance button is shown (only with frame pacing). Takes effect the
    // next time the menu opens.
    void setShowJitterTolerance(bool show);

    bool isOpen() const { return m_Open; }

    // Opens with Disconnect selected, as on Android. Does nothing (returning false) if the
    // renderer can't draw the menu.
    bool open(SDL_Window* window);
    void close();

    // Draws the menu again for the window's current size, as after a resize or a new
    // renderer. Closes it if the renderer can't draw it.
    void refresh();

    void navigate(Direction direction);

    // Shows options in place of the main buttons, for the main button with the given action
    // (like ActionStats), with the checked one marked and selected. Picking one is
    // ActionOptionBase plus its index, and Back returns to the main buttons.
    void showOptions(int action, const QStringList& labels, int checked);
    void showMainButtons();
    // The main button whose options are showing, or ActionNone
    int getOptionsAction() const { return m_OptionsAction; }

    // Quit Session asks to be pressed again. Moving the selection ends this.
    void setQuitConfirmation(bool confirm);
    bool isQuitConfirmation() const { return m_QuitConfirmation; }

    // The selected button's action, or ActionNone if the menu is closed
    int selectedAction() const;

private:
    struct Item {
        int action;
        // A Material Symbols icon name, another icon (see OverlayMenuCommand::icon), or empty
        QString icon;
        QString label;
        // A longer label it may change to, whose width is kept for it so the menu doesn't move
        QString widestLabel;
    };

    enum Region {
        RegionColumn,
        RegionRow,
    };

    void buildItems();
    void setColumnIndex(int index);
    void setRowIndex(int index);
    void draw();
    void drawButton(QPainter& painter, const QRectF& rect, const Item& item, bool selected,
                    qreal scale, qreal verticalPadding);

    Overlay::OverlayManager& m_Overlays;
    std::function<void()> m_RedrawHandler;
    std::function<void()> m_ClosedHandler;
    SDL_Window* m_Window;
    bool m_Open;
    bool m_ShowJitterTolerance;
    bool m_ShowGamepadButtons;
    bool m_QuitConfirmation;

    int m_OptionsAction;
    QStringList m_OptionLabels;
    int m_OptionChecked;

    QVector<QPair<QString, QString>> m_CustomCommands;

    // Icons as drawn, by icon, size and color. Cleared as the menu opens.
    QHash<QString, QImage> m_IconCache;

    QVector<Item> m_Column;
    QVector<Item> m_Row;
    Region m_Region;
    int m_ColumnIndex;
    int m_RowIndex;
    // How far the row is scrolled left (pixels) where it's wider than the window
    qreal m_RowScroll;
};
