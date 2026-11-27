#include "launchwindow.h"

#include "session.h"
#include "streamutils.h"
#include "settings/streamingpreferences.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <SDL_syswm.h>

#ifdef Q_OS_WIN32
#include <Windows.h>
#endif

// In units of the scale, like the overlay menu: a window this many pixels tall draws at a
// scale of 1, and taller windows scale it up
#define REFERENCE_HEIGHT 720.0
#define SPINNER_SIZE 32
#define SPINNER_GAP 20
#define TEXT_SIZE 16
#define LINE_SIZE 12
#define LINE_GAP 12
#define HINT_SIZE 11
#define HINT_MARGIN 28
#define BUTTON_TEXT_SIZE 12
#define BUTTON_HEIGHT 36
#define BUTTON_MIN_WIDTH 96
#define BUTTON_PADDING 16
#define BUTTON_GAP 12
#define BUTTON_CORNER_RADIUS 8
#define BUTTON_STROKE 2
#define BUTTONS_GAP 28
#define MAX_TEXT_WIDTH 560

#define ICON_SIZE 64

static const QColor k_BackgroundColor(0, 0, 0);
static const QColor k_LineColor(0xC8, 0xC8, 0xC8);
static const QColor k_HintColor(0x90, 0x90, 0x90);

LaunchWindow::LaunchWindow(QObject* parent)
    : QObject(parent),
      m_Window(nullptr),
      m_Suspended(false),
      m_RendererUsedWindow(false),
      m_NextWindow(nullptr),
      m_Mode(ModeStatus),
      m_Selected(0),
      m_Scale(1),
      m_NeedsRender(true),
      m_QueuedWindowEvents(0),
      m_GamepadInput(false)
{
    m_Timer.setInterval(16);
    connect(&m_Timer, &QTimer::timeout, this, &LaunchWindow::tick);
    m_SpinnerTimer.start();
}

LaunchWindow::~LaunchWindow()
{
    setGamepadInput(false);

    // The stream's window first, since the window behind it is what shows when it goes
    if (m_Window != nullptr) {
        destroyWindow(m_Window);
    }
    if (m_NextWindow != nullptr) {
        destroyWindow(m_NextWindow);
    }
}

void LaunchWindow::destroyWindow(SDL_Window* window)
{
#ifdef Q_OS_WIN32
    // SDL takes a full-screen window out of full screen before it hides it, which shows
    // the window at its windowed size for a moment, with the desktop around it. Hidden
    // first, that happens out of sight.
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info)) {
        ShowWindow(info.info.win.window, SW_HIDE);
    }
#endif
    SDL_DestroyWindow(window);
}

bool LaunchWindow::create(StreamingPreferences* preferences, const QPoint* displayOrigin)
{
    int displayIndex = displayOrigin != nullptr ? Session::findDisplayIndex(*displayOrigin) : 0;

    int x, y, width, height;
    Session::getWindowDimensions(displayIndex, preferences->width, preferences->height,
                                 x, y, width, height);

    // The session switches to its own full-screen mode (and display mode) once the stream
    // starts. Until then, a full-screen window that doesn't change the display mode.
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (preferences->windowMode != StreamingPreferences::WM_WINDOWED) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    m_Window = createWindow("Moonlight", x, y, width, height, flags);
    if (m_Window == nullptr) {
        return false;
    }

    SDL_RaiseWindow(m_Window);

    m_Timer.start();
    tick();
    return true;
}

SDL_Window* LaunchWindow::createWindow(const char* title, int x, int y, int width, int height, Uint32 flags)
{
    // SDL starts text input on new windows, which can show the IME
    SDL_StopTextInput();

    SDL_Window* window = SDL_CreateWindow(title, x, y, width, height,
                                          flags | StreamUtils::getPlatformWindowFlags());
    if (window == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "SDL_CreateWindow() failed with platform flags: %s",
                    SDL_GetError());

        window = SDL_CreateWindow(title, x, y, width, height, flags);
        if (window == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow() failed: %s",
                         SDL_GetError());
            return nullptr;
        }
    }

    // The icon has to be set before going full-screen to stay when going windowed,
    // so the session can't set it on this window
    QSvgRenderer svgIconRenderer(QString(":/res/moonlight.svg"));
    QImage svgImage(ICON_SIZE, ICON_SIZE, QImage::Format_RGBA8888);
    svgImage.fill(0);
    {
        QPainter svgPainter(&svgImage);
        svgIconRenderer.render(&svgPainter);
    }
    SDL_Surface* iconSurface = SDL_CreateRGBSurfaceWithFormatFrom((void*)svgImage.constBits(),
                                                                  svgImage.width(),
                                                                  svgImage.height(),
                                                                  32,
                                                                  4 * svgImage.width(),
                                                                  SDL_PIXELFORMAT_RGBA32);
    if (iconSurface != nullptr) {
        SDL_SetWindowIcon(window, iconSurface);
        SDL_FreeSurface(iconSurface);
    }

    return window;
}

void LaunchWindow::prepareWindowRelease()
{
    if (m_NextWindow != nullptr || m_Window == nullptr) {
        return;
    }

    // The same place and size. In full screen, a borderless window over the display does,
    // since it never changes the display mode. It's on top of everything but the stream
    // until it has the foreground.
    Uint32 oldFlags = SDL_GetWindowFlags(m_Window);
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;
    int x, y, width, height;
    if (oldFlags & SDL_WINDOW_FULLSCREEN) {
        int displayIndex = qMax(0, SDL_GetWindowDisplayIndex(m_Window));
        SDL_Rect bounds;
        SDL_GetDisplayBounds(displayIndex, &bounds);
        x = bounds.x;
        y = bounds.y;
        width = bounds.w;
        height = bounds.h;
        flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP;
    }
    else {
        SDL_GetWindowPosition(m_Window, &x, &y);
        SDL_GetWindowSize(m_Window, &width, &height);
        flags |= SDL_WINDOW_RESIZABLE;
        if (oldFlags & SDL_WINDOW_MAXIMIZED) {
            flags |= SDL_WINDOW_MAXIMIZED;
        }
    }

    m_NextWindow = createWindow(SDL_GetWindowTitle(m_Window), x, y, width, height, flags);
    if (m_NextWindow == nullptr) {
        return;
    }

#ifdef Q_OS_WIN32
    // It goes just behind the stream's window, without the foreground. Anything over a
    // full-screen stream makes the compositor take the screen back from it, which shows
    // the desktop for a moment, but this doesn't. When the renderer lets go of the stream's
    // window, it shows what's behind it, which is now this.
    SDL_SysWMinfo streamInfo, info;
    SDL_VERSION(&streamInfo.version);
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_Window, &streamInfo) && SDL_GetWindowWMInfo(m_NextWindow, &info)) {
        SetWindowPos(info.info.win.window, streamInfo.info.win.window, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // It takes the foreground when it replaces the stream's window, but we can lose it
    // when the renderer goes, so we keep the right to take it back while we have it
    AllowSetForegroundWindow(GetCurrentProcessId());
#endif

    // Blank until we know what to show. It's drawn while the stream still covers it.
    m_Mode = ModeBlank;
    m_Image = QImage(width, height, QImage::Format_RGB32);
    m_Scale = height / REFERENCE_HEIGHT;
    render();
    blit(m_Image.rect(), m_NextWindow);
}

void LaunchWindow::replaceWindow()
{
    if (m_NextWindow == nullptr) {
        // We weren't told before the renderer went
        prepareWindowRelease();
        if (m_NextWindow == nullptr) {
            return;
        }
    }

#ifdef Q_OS_WIN32
    // The stream's window had the foreground, so we can give it to this one before it goes
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_NextWindow, &info)) {
        SetForegroundWindow(info.info.win.window);
    }
#endif

    destroyWindow(m_Window);
    m_Window = m_NextWindow;
    m_NextWindow = nullptr;

    // It covered the stream's window, but the cards needn't stay on top of everything
    SDL_SetWindowAlwaysOnTop(m_Window, SDL_FALSE);

    // The old window's events, which nothing else will take now
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_WINDOWEVENT);
    m_QueuedWindowEvents = 0;
}

SDL_Window* LaunchWindow::getWindow() const
{
    return m_Window;
}

void LaunchWindow::showStatus(const QString& text, const QStringList& lines, const QString& hint)
{
    m_Text = text;
    m_Lines = lines;
    m_Hint = hint;
    m_Buttons.clear();
    setMode(ModeStatus);
}

void LaunchWindow::showMessage(const QString& text)
{
    m_Text = text;
    m_Lines.clear();
    m_Hint.clear();
    m_Buttons = { { tr("OK"), ChoiceAccept, QRect() } };
    m_Selected = 0;
    setMode(ModeMessage);
}

void LaunchWindow::showQuestion(const QString& text)
{
    m_Text = text;
    m_Lines.clear();
    m_Hint.clear();
    m_Buttons = { { tr("Yes"), ChoiceAccept, QRect() }, { tr("No"), ChoiceReject, QRect() } };
    m_Selected = 0;
    setMode(ModeQuestion);
}

void LaunchWindow::suspend(bool forRenderer)
{
    if (forRenderer) {
        m_RendererUsedWindow = true;
    }

    // Draw the card we were asked to show last, since nothing draws it while suspended
    if (!m_Suspended && m_Window != nullptr && m_NeedsRender) {
        updateSize();
        render();
        blit(m_Image.rect());
    }

    m_Suspended = true;
    m_Timer.stop();
    setGamepadInput(false);
}

void LaunchWindow::resume()
{
    if (!m_Suspended) {
        return;
    }

    m_Suspended = false;

    // Once a renderer has presented to the window with a flip model swap chain, what we
    // draw with GDI doesn't show, so the cards after the stream need a new window
    if (m_RendererUsedWindow) {
        m_RendererUsedWindow = false;
        replaceWindow();
    }

    setMode(m_Mode);
    m_Timer.start();
    tick();
}

void LaunchWindow::setMode(Mode mode)
{
    m_Mode = mode;
    m_NeedsRender = true;

    // Gamepads answer the cards, but the session's input handler opens them for the
    // stream, and it has to be the one to start SDL's gamepad support
    setGamepadInput(!m_Suspended && mode != ModeStatus && mode != ModeBlank);
}

void LaunchWindow::setGamepadInput(bool enabled)
{
    if (enabled == m_GamepadInput) {
        return;
    }

    m_GamepadInput = enabled;
    if (enabled) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                        SDL_GetError());
            m_GamepadInput = false;
            return;
        }

        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* gamepad = SDL_GameControllerOpen(i);
                if (gamepad != nullptr) {
                    m_Gamepads.append(gamepad);
                }
            }
        }
    }
    else {
        for (SDL_GameController* gamepad : std::as_const(m_Gamepads)) {
            SDL_GameControllerClose(gamepad);
        }
        m_Gamepads.clear();
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
}

void LaunchWindow::tick()
{
    if (m_Window == nullptr || m_Suspended) {
        return;
    }

    SDL_PumpEvents();
    processEvents();

    // processEvents() may have suspended us or changed the card
    if (m_Suspended) {
        return;
    }

    // Window events stay queued for the session, but more of them means we've been
    // shown, uncovered or resized, so we draw it all again
    int windowEvents = SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT);
    bool exposed = windowEvents != m_QueuedWindowEvents;
    m_QueuedWindowEvents = windowEvents;

    if (updateSize() || m_NeedsRender) {
        render();
        blit(m_Image.rect());
    }
    else if (exposed) {
        blit(m_Image.rect());
    }
    else if (m_Mode == ModeStatus) {
        QPainter painter(&m_Image);
        drawSpinner(painter);
        painter.end();
        blit(m_SpinnerRect);
    }
}

void LaunchWindow::processEvents()
{
    SDL_Event event;

    // Closing the window
    while (!m_Suspended && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        activate(m_Mode == ModeQuestion ? ChoiceReject : ChoiceAccept);
    }

    // Input isn't for the host, so all of it goes, but window and device arrival events
    // stay for the session
    static const Uint32 k_InputEventRanges[][2] = {
        { SDL_KEYDOWN, SDL_MOUSEWHEEL },
        { SDL_JOYAXISMOTION, SDL_JOYBUTTONUP },
        { SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP },
        { SDL_CONTROLLERTOUCHPADDOWN, SDL_CONTROLLERSENSORUPDATE },
        { SDL_FINGERDOWN, SDL_MULTIGESTURE },
    };

    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    for (const auto& range : k_InputEventRanges) {
        while (!m_Suspended && SDL_PeepEvents(&event, 1, SDL_GETEVENT, range[0], range[1]) > 0) {
            switch (event.type) {
            case SDL_KEYDOWN:
                if (event.key.repeat) {
                    break;
                }
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_BACKSPACE:
                case SDLK_AC_BACK:
                    activate(m_Mode == ModeQuestion ? ChoiceReject : ChoiceAccept);
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    if (m_Mode != ModeStatus) {
                        activate(m_Buttons[m_Selected].choice);
                    }
                    break;
                case SDLK_y:
                    if (m_Mode == ModeQuestion) {
                        activate(ChoiceAccept);
                    }
                    break;
                case SDLK_n:
                    if (m_Mode == ModeQuestion) {
                        activate(ChoiceReject);
                    }
                    break;
                case SDLK_LEFT:
                    moveSelection(-1);
                    break;
                case SDLK_RIGHT:
                case SDLK_TAB:
                    moveSelection(1);
                    break;
                }
                break;

            case SDL_CONTROLLERBUTTONDOWN:
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    if (m_Mode != ModeStatus) {
                        activate(m_Buttons[m_Selected].choice);
                    }
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    activate(m_Mode == ModeQuestion ? ChoiceReject : ChoiceAccept);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                    moveSelection(-1);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                    moveSelection(1);
                    break;
                }
                break;

            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONUP: {
                // Window coordinates to the image's pixels
                QPoint point(event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x,
                             event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y);
                if (windowWidth > 0 && windowHeight > 0) {
                    point = QPoint(point.x() * m_Image.width() / windowWidth,
                                   point.y() * m_Image.height() / windowHeight);
                }

                for (int i = 0; i < m_Buttons.size(); i++) {
                    if (m_Buttons[i].rect.contains(point)) {
                        if (event.type == SDL_MOUSEBUTTONUP) {
                            if (event.button.button == SDL_BUTTON_LEFT) {
                                activate(m_Buttons[i].choice);
                            }
                        }
                        else if (m_Selected != i) {
                            m_Selected = i;
                            m_NeedsRender = true;
                        }
                        break;
                    }
                }
                break;
            }
            }
        }
    }
}

void LaunchWindow::activate(Choice choice)
{
    if (m_Mode == ModeStatus || m_Mode == ModeBlank) {
        // Only backing out does anything while we wait
        emit canceled();
    }
    else {
        // The handler shows the next card, or ends the process
        emit finished(choice);
    }
}

void LaunchWindow::moveSelection(int delta)
{
    if (m_Buttons.size() < 2) {
        return;
    }

    int selected = qBound(0, m_Selected + delta, (int)m_Buttons.size() - 1);
    if (selected != m_Selected) {
        m_Selected = selected;
        m_NeedsRender = true;
    }
}

bool LaunchWindow::updateSize()
{
#ifdef Q_OS_WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(m_Window, &info)) {
        return false;
    }

    RECT clientRect;
    if (!GetClientRect(info.info.win.window, &clientRect)) {
        return false;
    }

    QSize size(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
    if (size.isEmpty() || size == m_Image.size()) {
        return false;
    }

    m_Image = QImage(size, QImage::Format_RGB32);
    m_Scale = size.height() / REFERENCE_HEIGHT;
    return true;
#else
    return false;
#endif
}

void LaunchWindow::render()
{
    m_NeedsRender = false;
    if (m_Image.isNull()) {
        return;
    }

    const qreal s = m_Scale;
    const int width = m_Image.width();
    const int height = m_Image.height();
    const int textWidth = qMin((int)(width * 0.85), (int)(MAX_TEXT_WIDTH * s));

    QPainter painter(&m_Image);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.fillRect(m_Image.rect(), k_BackgroundColor);

    if (m_Mode == ModeBlank) {
        m_SpinnerRect = QRect();
        return;
    }

    QFont textFont = QGuiApplication::font();
    textFont.setPixelSize(qMax(1, qRound(TEXT_SIZE * s)));
    QFont lineFont = QGuiApplication::font();
    lineFont.setPixelSize(qMax(1, qRound(LINE_SIZE * s)));

    // Everything but the hint is centered as one block, so measure it first
    QRect textBounds = QFontMetrics(textFont).boundingRect(QRect(0, 0, textWidth, height),
                                                           Qt::AlignHCenter | Qt::TextWordWrap, m_Text);
    QString lines = m_Lines.join('\n');
    QRect lineBounds;
    if (!lines.isEmpty()) {
        lineBounds = QFontMetrics(lineFont).boundingRect(QRect(0, 0, textWidth, height),
                                                         Qt::AlignHCenter | Qt::TextWordWrap, lines);
    }

    int blockHeight = textBounds.height();
    if (m_Mode == ModeStatus) {
        blockHeight += qRound((SPINNER_SIZE + SPINNER_GAP) * s);
    }
    if (!lines.isEmpty()) {
        blockHeight += qRound(LINE_GAP * s) + lineBounds.height();
    }
    if (!m_Buttons.isEmpty()) {
        blockHeight += qRound((BUTTONS_GAP + BUTTON_HEIGHT) * s);
    }

    int y = (height - blockHeight) / 2;

    if (m_Mode == ModeStatus) {
        int spinnerSize = qRound(SPINNER_SIZE * s);
        m_SpinnerRect = QRect((width - spinnerSize) / 2, y, spinnerSize, spinnerSize);
        drawSpinner(painter);
        y += spinnerSize + qRound(SPINNER_GAP * s);
    }
    else {
        m_SpinnerRect = QRect();
    }

    painter.setPen(Qt::white);
    painter.setFont(textFont);
    painter.drawText(QRect((width - textWidth) / 2, y, textWidth, textBounds.height()),
                     Qt::AlignHCenter | Qt::TextWordWrap, m_Text);
    y += textBounds.height();

    if (!lines.isEmpty()) {
        y += qRound(LINE_GAP * s);
        painter.setPen(k_LineColor);
        painter.setFont(lineFont);
        painter.drawText(QRect((width - textWidth) / 2, y, textWidth, lineBounds.height()),
                         Qt::AlignHCenter | Qt::TextWordWrap, lines);
        y += lineBounds.height();
    }

    if (!m_Buttons.isEmpty()) {
        y += qRound(BUTTONS_GAP * s);

        QFont buttonFont = QGuiApplication::font();
        buttonFont.setPixelSize(qMax(1, qRound(BUTTON_TEXT_SIZE * s)));
        QFontMetrics buttonMetrics(buttonFont);

        int buttonHeight = qRound(BUTTON_HEIGHT * s);
        int gap = qRound(BUTTON_GAP * s);
        int totalWidth = -gap;
        for (Button& button : m_Buttons) {
            int buttonWidth = qMax(qRound(BUTTON_MIN_WIDTH * s),
                                   buttonMetrics.horizontalAdvance(button.label) + qRound(2 * BUTTON_PADDING * s));
            button.rect = QRect(0, y, buttonWidth, buttonHeight);
            totalWidth += buttonWidth + gap;
        }

        int x = (width - totalWidth) / 2;
        painter.setFont(buttonFont);
        for (int i = 0; i < m_Buttons.size(); i++) {
            Button& button = m_Buttons[i];
            button.rect.moveLeft(x);
            x += button.rect.width() + gap;

            // The overlay menu's buttons: translucent white with a white border and black
            // text when selected. Their translucent black is just black on our background.
            bool selected = i == m_Selected;
            qreal stroke = BUTTON_STROKE * s;
            QPainterPath path;
            path.addRoundedRect(QRectF(button.rect).adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2),
                                BUTTON_CORNER_RADIUS * s, BUTTON_CORNER_RADIUS * s);
            if (selected) {
                painter.fillPath(path, QColor(0xFF, 0xFF, 0xFF, 0xC0));
                painter.strokePath(path, QPen(Qt::white, stroke));
                painter.setPen(Qt::black);
            }
            else {
                painter.fillPath(path, QColor(0x30, 0x30, 0x30));
                painter.setPen(Qt::white);
            }
            painter.drawText(button.rect, Qt::AlignCenter, button.label);
        }
    }

    if (!m_Hint.isEmpty()) {
        QFont hintFont = QGuiApplication::font();
        hintFont.setPixelSize(qMax(1, qRound(HINT_SIZE * s)));
        painter.setFont(hintFont);
        painter.setPen(k_HintColor);

        int hintWidth = (int)(width * 0.9);
        QRect hintBounds = QFontMetrics(hintFont).boundingRect(QRect(0, 0, hintWidth, height),
                                                               Qt::AlignHCenter | Qt::TextWordWrap, m_Hint);
        painter.drawText(QRect((width - hintWidth) / 2, height - qRound(HINT_MARGIN * s) - hintBounds.height(),
                               hintWidth, hintBounds.height()),
                         Qt::AlignHCenter | Qt::TextWordWrap, m_Hint);
    }
}

void LaunchWindow::drawSpinner(QPainter& painter)
{
    if (m_SpinnerRect.isEmpty()) {
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(m_SpinnerRect, k_BackgroundColor);

    // The overlay menu's spinner: a three-quarter ring, a turn a second
    qreal size = m_SpinnerRect.width();
    qreal inset = size * 0.1;
    QPen pen(Qt::white, size * 0.09);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    int degrees = (int)(m_SpinnerTimer.elapsed() % 1000 * 360 / 1000);
    painter.drawArc(QRectF(m_SpinnerRect).adjusted(inset, inset, -inset, -inset), -degrees * 16, 270 * 16);
}

void LaunchWindow::blit(const QRect& rect, SDL_Window* window)
{
#ifdef Q_OS_WIN32
    if (m_Image.isNull() || rect.isEmpty()) {
        return;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window != nullptr ? window : m_Window, &info)) {
        return;
    }

    HWND hwnd = info.info.win.window;
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) {
        return;
    }

    // A top-down DIB of the whole image (Format_RGB32 is BGRX in memory), of which we
    // copy the rectangle. StretchDIBits() still measures the source rectangle from the
    // bottom, as it would for a bottom-up DIB.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = m_Image.width();
    bmi.bmiHeader.biHeight = -m_Image.height();
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(dc,
                  rect.x(), rect.y(), rect.width(), rect.height(),
                  rect.x(), m_Image.height() - rect.y() - rect.height(), rect.width(), rect.height(),
                  m_Image.constBits(), &bmi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(hwnd, dc);
#else
    Q_UNUSED(rect);
    Q_UNUSED(window);
#endif
}
