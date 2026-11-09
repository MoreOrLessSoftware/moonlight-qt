#include "overlaymenu.h"
#include "settings/overlaymenucommands.h"

#include <QCoreApplication>
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

OverlayMenu::OverlayMenu(Overlay::OverlayManager& overlays) :
    m_Overlays(overlays),
    m_Window(nullptr),
    m_Open(false),
    m_ShowJitterTolerance(false),
    m_ShowGamepadButtons(true),
    m_DisconnectPage(false),
    m_OptionsAction(ActionNone),
    m_OptionChecked(-1),
    m_Group(-1),
    m_MessageAction(ActionNone),
    m_Region(RegionColumn),
    m_ColumnIndex(0),
    m_RowIndex(0),
    m_RowScroll(0),
    m_ColumnScroll(0)
{
}

void OverlayMenu::setRedrawHandler(std::function<void()> handler)
{
    m_RedrawHandler = handler;
}

void OverlayMenu::setClosedHandler(std::function<void()> handler)
{
    m_ClosedHandler = handler;
}

void OverlayMenu::setShowGamepadButtons(bool show)
{
    m_ShowGamepadButtons = show;
}

void OverlayMenu::setCustomCommands(const QVector<QPair<QString, QString>>& commands)
{
    m_CustomCommands = commands;
}

void OverlayMenu::setShowJitterTolerance(bool show)
{
    m_ShowJitterTolerance = show;
}

void OverlayMenu::setHostCommands(const QVector<QPair<QString, QString>>& commands, const QVector<HostGroup>& groups)
{
    m_HostCommands = commands;
    m_HostGroups = groups;

    if (!m_Open) {
        return;
    }

    // A group that's gone takes its page with it
    if (m_Group >= m_HostGroups.size()) {
        m_Group = -1;
        m_ColumnScroll = 0;
    }

    // The same button stays selected where it's still there
    int selected = selectedAction();
    Region region = m_Region;
    buildItems();

    const QVector<Item>& items = region == RegionColumn ? m_Column : m_Row;
    int index = -1;
    for (int i = 0; i < items.size(); i++) {
        if (items[i].action == selected) {
            index = i;
            break;
        }
    }
    if (region == RegionColumn) {
        setColumnIndex(index >= 0 ? index : qBound(0, m_ColumnIndex, (int)m_Column.size() - 1));
    }
    else {
        setRowIndex(index >= 0 ? index : qBound(0, m_RowIndex, (int)m_Row.size() - 1));
    }
    draw();
}

void OverlayMenu::showGroup(int group)
{
    if (!m_Open || group < 0 || group >= m_HostGroups.size()) {
        return;
    }

    m_Group = group;
    m_OptionsAction = ActionNone;
    m_DisconnectPage = false;
    m_MessageAction = ActionNone;
    buildItems();

    m_Region = RegionColumn;
    m_ColumnIndex = 0;
    m_RowIndex = 0;
    m_RowScroll = 0;
    m_ColumnScroll = 0;
    draw();
}

void OverlayMenu::setMessage(int action, const QString& message)
{
    if (!m_Open) {
        return;
    }
    if (message.isEmpty() && m_MessageAction != action) {
        return;
    }

    m_MessageAction = message.isEmpty() ? ActionNone : action;
    m_Message = message;
    buildItems();
    draw();
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
    m_IconCache.clear();
    m_OptionsAction = ActionNone;
    m_Group = -1;
    m_MessageAction = ActionNone;
    m_DisconnectPage = false;
    buildItems();

    // Disconnect, the bottom of the column, starts out selected
    m_Region = RegionColumn;
    m_ColumnIndex = m_Column.size() - 1;
    m_RowIndex = 0;
    m_RowScroll = 0;
    m_ColumnScroll = 0;

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
    if (m_ClosedHandler) {
        m_ClosedHandler();
    }
}

void OverlayMenu::showOptions(int action, const QStringList& labels, int checked)
{
    if (!m_Open) {
        return;
    }

    m_OptionsAction = action;
    m_OptionLabels = labels;
    m_OptionChecked = checked;
    m_Group = -1;
    m_MessageAction = ActionNone;
    m_DisconnectPage = false;
    buildItems();

    m_Region = RegionColumn;
    m_ColumnIndex = qBound(0, checked, (int)m_Column.size() - 1);
    m_RowIndex = 0;
    m_RowScroll = 0;
    m_ColumnScroll = 0;
    draw();
}

void OverlayMenu::showMainButtons()
{
    if (!m_Open) {
        return;
    }

    // Back on the button the page was for
    int action = m_DisconnectPage ? (int)ActionDisconnectPage :
                 m_Group >= 0 ? ActionHostGroupBase + m_Group : m_OptionsAction;
    m_OptionsAction = ActionNone;
    m_Group = -1;
    m_DisconnectPage = false;
    m_MessageAction = ActionNone;
    buildItems();

    m_Region = RegionColumn;
    m_ColumnIndex = m_Column.size() - 1;
    m_RowIndex = 0;
    m_RowScroll = 0;
    m_ColumnScroll = 0;
    for (int i = 0; i < m_Column.size(); i++) {
        if (m_Column[i].action == action) {
            m_ColumnIndex = i;
            break;
        }
    }
    for (int i = 0; i < m_Row.size(); i++) {
        if (m_Row[i].action == action) {
            setRowIndex(i);
            break;
        }
    }
    draw();
}

void OverlayMenu::showDisconnectPage()
{
    if (!m_Open) {
        return;
    }

    m_DisconnectPage = true;
    m_OptionsAction = ActionNone;
    m_Group = -1;
    m_MessageAction = ActionNone;
    buildItems();

    m_Region = RegionColumn;
    m_ColumnIndex = m_Column.size() - 1;
    m_RowIndex = 0;
    m_RowScroll = 0;
    m_ColumnScroll = 0;
    draw();
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

    if (m_OptionsAction != ActionNone) {
        // The options, with the one in effect marked, and Back
        for (int i = 0; i < m_OptionLabels.size(); i++) {
            m_Column.append({ ActionOptionBase + i,
                              i == m_OptionChecked ? "radio_button_checked" : "radio_button_unchecked",
                              m_OptionLabels[i] });
        }
        m_Row.append({ ActionBack, "arrow_back", QCoreApplication::translate("OverlayMenu", "Back (B)") });
        applyMessage();
        return;
    }

    if (m_Group >= 0 && m_Group < m_HostGroups.size()) {
        // The group's commands, and Back
        const HostGroup& group = m_HostGroups[m_Group];
        for (int i = 0; i < group.commands.size() && i < 1000; i++) {
            m_Column.append({ ActionGroupCommandBase + i, group.commands[i].first, group.commands[i].second });
        }
        m_Row.append({ ActionBack, "arrow_back", QCoreApplication::translate("OverlayMenu", "Back (B)") });
        applyMessage();
        return;
    }

    if (m_DisconnectPage) {
        // Disconnect stays last, where the button that opened this page was, so a repeated
        // press can only land on the choice that leaves the game running
        m_Column.append({ ActionQuit, "power_settings_new", QCoreApplication::translate("OverlayMenu", "Quit Session") });
        m_Column.append({ ActionDisconnect, "desktop_access_disabled", QCoreApplication::translate("OverlayMenu", "Disconnect") });
        m_Row.append({ ActionBack, "arrow_back", QCoreApplication::translate("OverlayMenu", "Back (B)") });
        applyMessage();
        return;
    }

    // The column, top to bottom. The hints in brackets are the controller buttons that run
    // them directly while the menu is open.
    if (m_ShowGamepadButtons) {
        m_Column.append({ ActionGuide, "sports_esports", QCoreApplication::translate("OverlayMenu", "Guide (Start)") });
        m_Column.append({ ActionMouseEmulation, "mouse", QCoreApplication::translate("OverlayMenu", "Mouse (X)") });
    }
    m_Column.append({ ActionFullScreen, "fullscreen", QCoreApplication::translate("OverlayMenu", "Full Screen (Y)") });
    m_Column.append({ ActionPasteText, "content_paste", QCoreApplication::translate("OverlayMenu", "Paste Text") });
    m_Column.append({ ActionStats, "monitoring", QCoreApplication::translate("OverlayMenu", "Stats (RB)") });
    if (m_ShowJitterTolerance) {
        m_Column.append({ ActionJitterTolerance, "network_check", QCoreApplication::translate("OverlayMenu", "Jitter Tolerance") });
    }
    m_Column.append({ ActionDisconnectPage, "desktop_access_disabled", QCoreApplication::translate("OverlayMenu", "Disconnect") });

    // The row, left to right: custom commands, the host's commands, the host's groups, then
    // Close
    for (int i = 0; i < m_CustomCommands.size() && ActionCustomBase + i < ActionOptionBase; i++) {
        m_Row.append({ ActionCustomBase + i, m_CustomCommands[i].first, m_CustomCommands[i].second });
    }
    for (int i = 0; i < m_HostCommands.size() && i < 1000; i++) {
        m_Row.append({ ActionHostCommandBase + i, m_HostCommands[i].first, m_HostCommands[i].second });
    }
    for (int i = 0; i < m_HostGroups.size() && i < 1000; i++) {
        m_Row.append({ ActionHostGroupBase + i, m_HostGroups[i].icon, m_HostGroups[i].label });
    }
    m_Row.append({ ActionClose, QString(), QCoreApplication::translate("OverlayMenu", "Close (B)") });
    applyMessage();
}

void OverlayMenu::applyMessage()
{
    if (m_MessageAction == ActionNone) {
        return;
    }

    for (QVector<Item>* items : { &m_Column, &m_Row }) {
        for (Item& item : *items) {
            if (item.action == m_MessageAction) {
                item.label = m_Message;
            }
        }
    }
}

void OverlayMenu::navigate(Direction direction)
{
    if (!m_Open || m_Column.isEmpty()) {
        return;
    }

    // A message goes as the selection moves
    if (m_MessageAction != ActionNone) {
        m_MessageAction = ActionNone;
        buildItems();
    }

    // The same moves as the Android client's OverlayMenuView
    switch (direction) {
    case DirectionUp:
        if (m_Region == RegionColumn) {
            setColumnIndex((m_ColumnIndex - 1 + m_Column.size()) % m_Column.size());
        }
        else if (isOnMainButtons()) {
            // Up from the row goes to the button above Disconnect
            setColumnIndex(m_Column.size() >= 2 ? m_Column.size() - 2 : 0);
        }
        else {
            setColumnIndex(m_Column.size() - 1);
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

bool OverlayMenu::selectAt(int x, int y)
{
    if (!m_Open || m_Window == nullptr) {
        return false;
    }

    // From window coordinates to the pixels the menu is drawn in
    int windowWidth, windowHeight, pixelWidth, pixelHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);
#else
    pixelWidth = windowWidth;
    pixelHeight = windowHeight;
#endif
    if (windowWidth <= 0 || windowHeight <= 0) {
        return false;
    }
    QPointF point(x * (qreal)pixelWidth / windowWidth, y * (qreal)pixelHeight / windowHeight);

    for (const HitTarget& target : std::as_const(m_HitTargets)) {
        if (!target.rect.contains(point)) {
            continue;
        }

        bool selected = m_Region == target.region &&
                (target.region == RegionColumn ? m_ColumnIndex : m_RowIndex) == target.index;
        if (!selected) {
            // A message goes as the selection moves
            if (m_MessageAction != ActionNone) {
                m_MessageAction = ActionNone;
                buildItems();
            }

            if (target.region == RegionColumn) {
                setColumnIndex(target.index);
            }
            else {
                setRowIndex(target.index);
            }
            draw();
        }
        return true;
    }

    return false;
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
    // In pixels, as the renderers draw overlays (a HiDPI window has more of them than its size)
    int windowWidth, windowHeight;
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(m_Window, &windowWidth, &windowHeight);
#else
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
#endif
    if (windowWidth <= 0 || windowHeight <= 0 || m_Column.isEmpty()) {
        return;
    }

    // Like the Android client, a column of full size buttons that won't fit the window's
    // height is made compact, with half the vertical padding and spacing. Beyond that, the
    // column scrolls, as on Android.
    int count = m_Column.size();
    auto columnHeight = [count](qreal scale, bool compact) {
        qreal padding = compact ? MENU_PADDING / 2.0 : MENU_PADDING;
        qreal spacing = compact ? MENU_SPACING / 2.0 : MENU_SPACING;
        return scale * (count * (MENU_ICON_SIZE + 2 * padding) + (count - 1) * spacing);
    };

    const qreal scale = windowHeight / MENU_REFERENCE_HEIGHT;
    const bool compact = columnHeight(scale, false) + 2 * MENU_MARGIN * scale > windowHeight;

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

    // The part of the column that fits, at least one button
    const qreal columnVisible = qMin(columnTotalHeight, qMax(buttonHeight, windowHeight - 2 * margin));
    if (m_Region == RegionColumn && m_ColumnIndex < count) {
        // Moved just far enough to show the selected button. With no scroll, the column's
        // bottom is at the bottom of the visible part.
        const qreal top = columnVisible - columnTotalHeight + m_ColumnIndex * (buttonHeight + columnSpacing);
        m_ColumnScroll = qMax(m_ColumnScroll, -top);
        m_ColumnScroll = qMin(m_ColumnScroll, columnVisible - (top + buttonHeight));
    }
    m_ColumnScroll = qBound((qreal)0, m_ColumnScroll, columnTotalHeight - columnVisible);

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
    int imageHeight = qMin(windowHeight, qCeil(columnVisible + 2 * margin));

    QImage image(imageWidth, imageHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

        const qreal bottom = imageHeight - margin;

        painter.setClipRect(QRectF(0, bottom - columnVisible, rowLeft, columnVisible));
        qreal y = bottom - columnTotalHeight + m_ColumnScroll;
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

    // Where the buttons are in the window, for the mouse. The image is drawn at its bottom left.
    {
        m_HitTargets.clear();

        const qreal top = windowHeight - imageHeight;
        const qreal bottom = imageHeight - margin;

        // Only the part of a column button scrolled into view
        const QRectF columnArea(0, top + bottom - columnVisible, rowLeft, columnVisible);
        qreal y = bottom - columnTotalHeight + m_ColumnScroll;
        for (int i = 0; i < m_Column.size(); i++) {
            QRectF rect = QRectF(margin, top + y, columnWidth, buttonHeight) & columnArea;
            if (!rect.isEmpty()) {
                m_HitTargets.append({ RegionColumn, i, rect });
            }
            y += buttonHeight + columnSpacing;
        }

        // Only the part of a row button scrolled into view
        const QRectF rowArea(rowLeft, top, rowVisible, imageHeight);
        for (int i = 0; i < m_Row.size(); i++) {
            QRectF rect = QRectF(rowLeft + rowX[i] - m_RowScroll, top + bottom - buttonHeight, rowWidths[i], buttonHeight) & rowArea;
            if (!rect.isEmpty()) {
                m_HitTargets.append({ RegionRow, i, rect });
            }
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
        // Drawn once for each size and color, since an image icon is read from its file
        int iconPixels = qMax(1, qRound(iconSize));
        QString key = QString("%1|%2|%3").arg(item.icon).arg(iconPixels).arg(contentColor.rgba());
        auto cached = m_IconCache.find(key);
        if (cached == m_IconCache.end()) {
            cached = m_IconCache.insert(key, OverlayMenuCommand::renderIcon(item.icon, iconPixels, contentColor));
        }
        painter.drawImage(QRectF(x, contentRect.top(), iconSize, iconSize), cached.value());

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
