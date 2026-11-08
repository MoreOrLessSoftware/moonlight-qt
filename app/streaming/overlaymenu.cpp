#include "overlaymenu.h"
#include "path.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

// Sizes from the Android client's OverlayMenuButton and OverlayMenuView, in dp there and in
// units of the menu's scale here (see draw())
#define MENU_ICON_SIZE 24
#define MENU_PADDING 12
#define MENU_TEXT_SIZE 12
#define MENU_LABEL_GAP 8
#define MENU_CORNER_RADIUS 8
#define MENU_SPACING 8
#define MENU_MARGIN 20
#define MENU_STROKE 2

// A window this many pixels tall draws the menu at a scale of 1. Taller windows scale it up,
// so it takes the same share of the screen at any resolution.
#define MENU_REFERENCE_HEIGHT 720.0

namespace {

struct MaterialSymbols
{
    QString family;
    QHash<QString, uint> codepoints;
};

// The Material Symbols font, registered with Qt on first use, and its icon names
const MaterialSymbols& materialSymbols()
{
    static MaterialSymbols symbols = [] {
        MaterialSymbols s;

        int id = QFontDatabase::addApplicationFontFromData(Path::readDataFile("MaterialSymbolsOutlined.ttf"));
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()) {
            s.family = QFontDatabase::applicationFontFamilies(id).first();
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to load the Material Symbols font; the overlay menu will have no icons");
        }

        // Each line is "<name> <hex codepoint>"
        const QList<QByteArray> lines = Path::readDataFile("MaterialSymbolsOutlined.codepoints").split('\n');
        for (const QByteArray& line : lines) {
            QList<QByteArray> fields = line.trimmed().split(' ');
            if (fields.size() == 2) {
                bool ok;
                uint codepoint = fields[1].toUInt(&ok, 16);
                if (ok) {
                    s.codepoints.insert(QString::fromLatin1(fields[0]), codepoint);
                }
            }
        }

        return s;
    }();

    return symbols;
}

// The text that draws an icon in the Material Symbols font, or empty for an unknown icon
QString iconText(const QString& name)
{
    uint codepoint = materialSymbols().codepoints.value(name, 0);
    if (codepoint == 0) {
        return QString();
    }

    // Some of the icons are outside the Basic Multilingual Plane
    char32_t ucs4 = codepoint;
    return QString::fromUcs4(&ucs4, 1);
}

}

OverlayMenu::OverlayMenu(Overlay::OverlayManager& overlays) :
    m_Overlays(overlays),
    m_Window(nullptr),
    m_Open(false),
    m_ShowJitterTolerance(false),
    m_Region(RegionColumn),
    m_ColumnIndex(0),
    m_RowIndex(0),
    m_RowScroll(0)
{
}

void OverlayMenu::setRedrawHandler(std::function<void()> handler)
{
    m_RedrawHandler = handler;
}

void OverlayMenu::setShowJitterTolerance(bool show)
{
    m_ShowJitterTolerance = show;
}

bool OverlayMenu::open(SDL_Window* window)
{
    if (m_Open) {
        return true;
    }

    if (!m_Overlays.isOverlaySupported(Overlay::OverlayMenu)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "The overlay menu isn't supported by this renderer");
        return false;
    }

    m_Window = window;
    buildItems();

    // Disconnect, the bottom of the column, starts out selected
    m_Region = RegionColumn;
    m_ColumnIndex = m_Column.size() - 1;
    m_RowIndex = 0;
    m_RowScroll = 0;

    m_Open = true;

    // Drawn before it's enabled, so it shows up with its image
    draw();
    m_Overlays.setOverlayState(Overlay::OverlayMenu, true);

    if (m_RedrawHandler) {
        m_RedrawHandler();
    }
    return true;
}

void OverlayMenu::close()
{
    if (!m_Open) {
        return;
    }

    m_Open = false;
    m_Overlays.setOverlayState(Overlay::OverlayMenu, false);

    if (m_RedrawHandler) {
        m_RedrawHandler();
    }
}

void OverlayMenu::refresh()
{
    if (!m_Open) {
        return;
    }

    // A new renderer may not draw the menu, and an open menu that isn't drawn would still
    // take the input
    if (!m_Overlays.isOverlaySupported(Overlay::OverlayMenu)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Closing the overlay menu, which the new renderer doesn't support");
        close();
        return;
    }

    draw();
}

void OverlayMenu::buildItems()
{
    m_Column.clear();
    m_Row.clear();

    // The column, top to bottom. The hints in brackets are the controller buttons that run
    // them directly while the menu is open.
    m_Column.append({ ActionGuide, "sports_esports", QCoreApplication::translate("OverlayMenu", "Guide (Start)") });
    m_Column.append({ ActionMouseEmulation, "mouse", QCoreApplication::translate("OverlayMenu", "Mouse (X)") });
    m_Column.append({ ActionFullScreen, "fullscreen", QCoreApplication::translate("OverlayMenu", "Full Screen (Y)") });
    m_Column.append({ ActionPasteText, "content_paste", QCoreApplication::translate("OverlayMenu", "Paste Text") });
    m_Column.append({ ActionStats, "monitoring", QCoreApplication::translate("OverlayMenu", "Stats (RB)") });
    if (m_ShowJitterTolerance) {
        m_Column.append({ ActionJitterTolerance, "network_check", QCoreApplication::translate("OverlayMenu", "Jitter Tolerance") });
    }
    m_Column.append({ ActionQuit, "power_settings_new", QCoreApplication::translate("OverlayMenu", "Quit Session") });
    m_Column.append({ ActionDisconnect, "desktop_access_disabled", QCoreApplication::translate("OverlayMenu", "Disconnect") });

    // The row, left to right: custom commands, then Close
    m_Row.append({ ActionClose, QString(), QCoreApplication::translate("OverlayMenu", "Close (B)") });
}

void OverlayMenu::navigate(Direction direction)
{
    if (!m_Open || m_Column.isEmpty()) {
        return;
    }

    // The same moves as the Android client's OverlayMenuView
    switch (direction) {
    case DirectionUp:
        if (m_Region == RegionColumn) {
            setColumnIndex((m_ColumnIndex - 1 + m_Column.size()) % m_Column.size());
        }
        else {
            // Up from the row goes to the button above Disconnect
            setColumnIndex(m_Column.size() >= 2 ? m_Column.size() - 2 : 0);
        }
        break;

    case DirectionDown:
        if (m_Region == RegionColumn) {
            setColumnIndex((m_ColumnIndex + 1) % m_Column.size());
        }
        else {
            // Down from the row wraps to the top of the column
            setColumnIndex(0);
        }
        break;

    case DirectionLeft:
        if (m_Region == RegionRow) {
            if (m_RowIndex > 0) {
                setRowIndex(m_RowIndex - 1);
            }
            else {
                // Left from the start of the row goes back to Disconnect
                setColumnIndex(m_Column.size() - 1);
            }
        }
        else if (!m_Row.isEmpty()) {
            // Left from the column wraps to the end of the row
            setRowIndex(m_Row.size() - 1);
        }
        break;

    case DirectionRight:
        if (m_Region == RegionColumn) {
            if (!m_Row.isEmpty()) {
                setRowIndex(0);
            }
        }
        else if (m_RowIndex < m_Row.size() - 1) {
            setRowIndex(m_RowIndex + 1);
        }
        else {
            // Right from the end of the row wraps to Disconnect
            setColumnIndex(m_Column.size() - 1);
        }
        break;
    }

    draw();
}

void OverlayMenu::setColumnIndex(int index)
{
    m_Region = RegionColumn;
    m_ColumnIndex = index;
}

void OverlayMenu::setRowIndex(int index)
{
    m_Region = RegionRow;
    m_RowIndex = index;
}

int OverlayMenu::selectedAction() const
{
    if (!m_Open) {
        return ActionNone;
    }

    if (m_Region == RegionColumn) {
        return m_ColumnIndex >= 0 && m_ColumnIndex < m_Column.size() ? m_Column[m_ColumnIndex].action : ActionNone;
    }
    else {
        return m_RowIndex >= 0 && m_RowIndex < m_Row.size() ? m_Row[m_RowIndex].action : ActionNone;
    }
}

void OverlayMenu::draw()
{
    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 || m_Column.isEmpty()) {
        return;
    }

    // Like the Android client, a column of full size buttons that won't fit the window's
    // height is made compact, with half the vertical padding and spacing. Beyond that, the
    // whole menu is scaled down to fit, where Android scrolls the column.
    int count = m_Column.size();
    auto columnHeight = [count](qreal scale, bool compact) {
        qreal padding = compact ? MENU_PADDING / 2.0 : MENU_PADDING;
        qreal spacing = compact ? MENU_SPACING / 2.0 : MENU_SPACING;
        return scale * (count * (MENU_ICON_SIZE + 2 * padding) + (count - 1) * spacing);
    };

    qreal scale = windowHeight / MENU_REFERENCE_HEIGHT;
    bool compact = false;
    if (columnHeight(scale, false) + 2 * MENU_MARGIN * scale > windowHeight) {
        compact = true;
        if (columnHeight(scale, true) + 2 * MENU_MARGIN * scale > windowHeight) {
            scale = windowHeight / (columnHeight(1, true) + 2 * MENU_MARGIN);
        }
    }

    const qreal verticalPadding = scale * (compact ? MENU_PADDING / 2.0 : MENU_PADDING);
    const qreal columnSpacing = scale * (compact ? MENU_SPACING / 2.0 : MENU_SPACING);
    const qreal spacing = scale * MENU_SPACING;
    const qreal margin = scale * MENU_MARGIN;
    const qreal buttonHeight = scale * MENU_ICON_SIZE + 2 * verticalPadding;

    QFont labelFont = QGuiApplication::font();
    labelFont.setPixelSize(qMax(1, qRound(scale * MENU_TEXT_SIZE)));
    QFontMetricsF labelMetrics(labelFont);

    auto buttonWidth = [&](const Item& item) {
        qreal width = 2 * scale * MENU_PADDING + labelMetrics.horizontalAdvance(item.label);
        if (!item.icon.isEmpty()) {
            width += scale * MENU_ICON_SIZE;
            if (!item.label.isEmpty()) {
                width += scale * MENU_LABEL_GAP;
            }
        }
        return qCeil(width);
    };

    // The column's buttons are all as wide as its widest one
    qreal columnWidth = 0;
    for (const Item& item : m_Column) {
        columnWidth = qMax(columnWidth, (qreal)buttonWidth(item));
    }
    qreal columnTotalHeight = count * buttonHeight + (count - 1) * columnSpacing;

    // The row starts right of the column, and scrolls where it's wider than the window
    QVector<qreal> rowX, rowWidths;
    qreal rowTotal = 0;
    for (const Item& item : m_Row) {
        if (!rowWidths.isEmpty()) {
            rowTotal += spacing;
        }
        rowX.append(rowTotal);
        rowWidths.append(buttonWidth(item));
        rowTotal += rowWidths.last();
    }

    const qreal rowLeft = margin + columnWidth + spacing;
    const qreal rowVisible = qMax((qreal)0, qMin(rowTotal, windowWidth - margin - rowLeft));
    if (m_Region == RegionRow && m_RowIndex < rowX.size()) {
        // Scrolled just far enough to show the selected button
        m_RowScroll = qMin(m_RowScroll, rowX[m_RowIndex]);
        m_RowScroll = qMax(m_RowScroll, rowX[m_RowIndex] + rowWidths[m_RowIndex] - rowVisible);
    }
    m_RowScroll = qBound((qreal)0, m_RowScroll, qMax((qreal)0, rowTotal - rowVisible));

    // The image covers the menu and its margins, and is drawn at the bottom left corner of
    // the window
    int imageWidth = qMin(windowWidth, qCeil(rowLeft + rowVisible + margin));
    int imageHeight = qMin(windowHeight, qCeil(columnTotalHeight + 2 * margin));

    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

        const qreal bottom = imageHeight - margin;

        qreal y = bottom - columnTotalHeight;
        for (int i = 0; i < m_Column.size(); i++) {
            drawButton(painter, QRectF(margin, y, columnWidth, buttonHeight), m_Column[i],
                       m_Region == RegionColumn && i == m_ColumnIndex, scale, verticalPadding);
            y += buttonHeight + columnSpacing;
        }

        // The row's bottom lines up with the column's
        painter.setClipRect(QRectF(rowLeft, 0, rowVisible, imageHeight));
        for (int i = 0; i < m_Row.size(); i++) {
            drawButton(painter, QRectF(rowLeft + rowX[i] - m_RowScroll, bottom - buttonHeight, rowWidths[i], buttonHeight),
                       m_Row[i], m_Region == RegionRow && i == m_RowIndex, scale, verticalPadding);
        }
    }

    // Overlay surfaces have straight alpha, like the text overlays SDL_ttf draws. Both
    // formats are a native-endian 0xAARRGGBB per pixel.
    image = image.convertToFormat(QImage::Format_ARGB32);

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, imageWidth, imageHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_CreateRGBSurfaceWithFormat() failed: %s",
                     SDL_GetError());
        return;
    }

    for (int row = 0; row < imageHeight; row++) {
        memcpy((char*)surface->pixels + row * surface->pitch, image.constScanLine(row), imageWidth * 4);
    }

    m_Overlays.updateOverlayImage(Overlay::OverlayMenu, surface);

    // Opening asks once it's enabled
    if (m_RedrawHandler && m_Overlays.isOverlayEnabled(Overlay::OverlayMenu)) {
        m_RedrawHandler();
    }
}

void OverlayMenu::drawButton(QPainter& painter, const QRectF& rect, const Item& item, bool selected,
                             qreal scale, qreal verticalPadding)
{
    // The Android client's colors: translucent black, or translucent white with a white
    // border and black content when selected
    const QColor contentColor = selected ? Qt::black : Qt::white;
    const qreal stroke = selected ? scale * MENU_STROKE : 0;

    QPainterPath background;
    QRectF backgroundRect = rect.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);
    background.addRoundedRect(backgroundRect, scale * MENU_CORNER_RADIUS, scale * MENU_CORNER_RADIUS);
    painter.fillPath(background, selected ? QColor(0xFF, 0xFF, 0xFF, 0xC0) : QColor(0, 0, 0, 0xD9));
    if (selected) {
        painter.strokePath(background, QPen(Qt::white, stroke));
    }

    qreal x = rect.left() + scale * MENU_PADDING;
    const qreal iconSize = scale * MENU_ICON_SIZE;
    const QRectF contentRect(x, rect.top() + verticalPadding, rect.width(), iconSize);

    painter.setPen(contentColor);

    if (!item.icon.isEmpty()) {
        QString glyph = iconText(item.icon);
        if (!glyph.isEmpty() && !materialSymbols().family.isEmpty()) {
            QFont iconFont(materialSymbols().family);
            iconFont.setPixelSize(qMax(1, qRound(iconSize)));
            painter.setFont(iconFont);
            painter.drawText(QRectF(x, contentRect.top(), iconSize, iconSize), Qt::AlignCenter, glyph);
        }

        x += iconSize;
        if (!item.label.isEmpty()) {
            x += scale * MENU_LABEL_GAP;
        }
    }

    if (!item.label.isEmpty()) {
        QFont labelFont = QGuiApplication::font();
        labelFont.setPixelSize(qMax(1, qRound(scale * MENU_TEXT_SIZE)));
        painter.setFont(labelFont);
        painter.drawText(QRectF(x, contentRect.top(), rect.right() - x, iconSize),
                         Qt::AlignLeft | Qt::AlignVCenter, item.label);
    }
}
