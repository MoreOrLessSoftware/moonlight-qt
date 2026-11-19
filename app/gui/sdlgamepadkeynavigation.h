#pragma once

#include <QSet>
#include <QTimer>
#include <QEvent>

#include <functional>

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

    // The instance QML uses, for C++ objects created from QML (null before it exists)
    static SdlGamepadKeyNavigation* instance() { return s_Instance; }

    // Gamepad sensor readings, which are otherwise discarded, go to this listener (one at a time;
    // an empty function removes it). While one is set, gamepads are polled every few ms instead of
    // every 50 ms so the readings arrive steadily. They only arrive while the window has focus.
    void setSensorListener(std::function<void(const SDL_ControllerSensorEvent&)> listener);

signals:
    // LB (-1) or RB (+1) was pressed, to move to the previous or next tab of a page
    void tabSwitchRequested(int delta);

private:
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    void updateTimerState();

    void addGamepad(SDL_GameController* gc);

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;

    // Gamepads whose left stick has been seen at rest. A stick only navigates once it
    // has been centred, so one that never is (like a pedal or throttle that SDL maps to
    // the left stick and rests at one end) can't keep navigating on its own.
    QSet<SDL_GameController*> m_CenteredSticks;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    std::function<void(const SDL_ControllerSensorEvent&)> m_SensorListener;

    static SdlGamepadKeyNavigation* s_Instance;
};
