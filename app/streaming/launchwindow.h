#pragma once

#include <QElapsedTimer>
#include <QRecursiveMutex>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "SDL_compat.h"

#include <atomic>
#include <thread>

class StreamingPreferences;

// The stream process's window, from before the stream starts until after it ends. While
// the renderer doesn't own it, it shows the launch's progress, errors and questions as
// cards drawn with GDI, in the overlay menu's style. Windows only.
//
// It runs from Qt's event loop, so it only takes the SDL events it needs: input (which
// isn't meant for the host yet) and quit requests. Window events stay queued for the
// session, which creates its renderer on the window's first SDL_WINDOWEVENT_SHOWN.
class LaunchWindow : public QObject
{
    Q_OBJECT

public:
    enum Choice {
        ChoiceAccept,
        ChoiceReject,
    };
    Q_ENUM(Choice)

    explicit LaunchWindow(QObject* parent = nullptr);
    ~LaunchWindow();

    // Creates the window as the session would, for the stream size and window mode in the
    // preferences, on the display whose top left corner is displayOrigin (or the first)
    bool create(StreamingPreferences* preferences, const QPoint* displayOrigin);

    SDL_Window* getWindow() const;

    // A spinner over the text, with smaller lines under it and a hint at the bottom.
    // B, Escape or closing the window emits canceled().
    void showStatus(const QString& text, const QStringList& lines = QStringList(), const QString& hint = QString());

    // The text with an OK button. Emits finished(ChoiceAccept) when dismissed.
    void showMessage(const QString& text);

    // The text with Yes and No buttons. Emits finished() with the choice.
    void showQuestion(const QString& text);

    // While the session's renderer owns the window (forRenderer), or the session is
    // ending, we neither draw nor take its events. After a renderer, resume() replaces the
    // window, since we can't draw on it anymore.
    void suspend(bool forRenderer = false);
    void resume();

    // Called while the session still has the window, before its renderer goes: puts the
    // window resume() will use just behind it, blank, so that's what shows when the
    // renderer lets go (a window a renderer has let go of shows what's behind it)
    void prepareWindowRelease();

    // Draws the card now, rather than on the next tick, before the main thread is busy
    void paintNow();

signals:
    void canceled();
    void finished(LaunchWindow::Choice choice);

private:
    enum Mode {
        ModeStatus,
        ModeMessage,
        ModeQuestion,
        ModeBlank,          // Nothing yet (see prepareWindowRelease())
    };

    struct Button {
        QString label;
        Choice choice;
        QRect rect;
    };

    SDL_Window* createWindow(const char* title, int x, int y, int width, int height, Uint32 flags);
    void replaceWindow();
    void destroyWindow(SDL_Window* window);

    void tick();
    void runSpinnerThread();
    void setMode(Mode mode);
    void processEvents();
    void activate(Choice choice);
    void moveSelection(int delta);
    void setGamepadInput(bool enabled);

    bool updateSize();
    void render();
    void drawSpinner(QPainter& painter);
    void blit(const QRect& rect, SDL_Window* window = nullptr);

    SDL_Window* m_Window;
    QTimer m_Timer;
    bool m_Suspended;
    bool m_RendererUsedWindow;
    SDL_Window* m_NextWindow;

    Mode m_Mode;
    QString m_Text;
    QStringList m_Lines;
    QString m_Hint;
    QVector<Button> m_Buttons;
    int m_Selected;

    // The image is the window's client area. The spinner is drawn again each tick.
    QImage m_Image;
    qreal m_Scale;
    QRect m_SpinnerRect;
    QElapsedTimer m_SpinnerTimer;
    bool m_NeedsRender;
    int m_QueuedWindowEvents;

    bool m_GamepadInput;

    // Everything drawing uses, which the spinner thread draws with too
    QRecursiveMutex m_DrawLock;
    std::thread m_SpinnerThread;
    std::atomic<bool> m_SpinnerThreadRunning;
    QVector<SDL_GameController*> m_Gamepads;
};
