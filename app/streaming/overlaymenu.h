#pragma once

#include <QString>
#include <QVector>

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
        // Custom command n is ActionCustomBase + n
        ActionCustomBase = 100,
    };

    enum Direction {
        DirectionUp,
        DirectionDown,
        DirectionLeft,
        DirectionRight,
    };

    explicit OverlayMenu(Overlay::OverlayManager& overlays);

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

    // The selected button's action, or ActionNone if the menu is closed
    int selectedAction() const;

private:
    struct Item {
        int action;
        // A Material Symbols icon name, or empty for none
        QString icon;
        QString label;
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
    SDL_Window* m_Window;
    bool m_Open;
    bool m_ShowJitterTolerance;

    QVector<Item> m_Column;
    QVector<Item> m_Row;
    Region m_Region;
    int m_ColumnIndex;
    int m_RowIndex;
    // How far the row is scrolled left (pixels) where it's wider than the window
    qreal m_RowScroll;
};
