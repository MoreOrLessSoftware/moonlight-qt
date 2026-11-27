#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "SDL_compat.h"

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

    // While the session's renderer owns the window, we neither draw nor take its events
    void suspend();
    void resume();

signals:
    void canceled();
    void finished(LaunchWindow::Choice choice);

private:
    enum Mode {
        ModeStatus,
        ModeMessage,
        ModeQuestion,
    };

    struct Button {
        QString label;
        Choice choice;
        QRect rect;
    };

    void tick();
    void setMode(Mode mode);
    void processEvents();
    void activate(Choice choice);
    void moveSelection(int delta);
    void setGamepadInput(bool enabled);

    bool updateSize();
    void render();
    void drawSpinner(QPainter& painter);
    void blit(const QRect& rect);

    SDL_Window* m_Window;
    QTimer m_Timer;
    bool m_Suspended;

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
    QVector<SDL_GameController*> m_Gamepads;
};
