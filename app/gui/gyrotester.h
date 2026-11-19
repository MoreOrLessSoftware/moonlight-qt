#pragma once

#include "settings/gamepadsettings.h"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVector>

// The live gyro test on the gyro settings page: reads one gamepad's gyro and accelerometer through
// the same GyroFilter a stream uses, with that gamepad's saved settings, and exposes raw and
// filtered traces, a crosshair moved by the filtered gyro, and how noisy it is at rest.
//
// Readings come from SdlGamepadKeyNavigation (which owns the SDL event queue in the GUI), so they
// only arrive while the window has focus.
class GyroTester : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString deviceKey READ deviceKey WRITE setDeviceKey NOTIFY deviceKeyChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

    Q_PROPERTY(bool receiving READ receiving NOTIFY updated)
    Q_PROPERTY(double crosshairX READ crosshairX NOTIFY updated)        // degrees, right = positive
    Q_PROPERTY(double crosshairY READ crosshairY NOTIFY updated)        // degrees, down = positive
    Q_PROPERTY(double crosshairRange READ crosshairRange CONSTANT)      // degrees from center to edge
    Q_PROPERTY(double restRawDps READ restRawDps NOTIFY updated)        // largest reading over the last second
    Q_PROPERTY(double restFilteredDps READ restFilteredDps NOTIFY updated)
    Q_PROPERTY(QString offsetText READ offsetText NOTIFY updated)
    Q_PROPERTY(QString accelText READ accelText NOTIFY updated)
    Q_PROPERTY(int traceLength READ traceLength CONSTANT)

public:
    explicit GyroTester(QObject* parent = nullptr);
    ~GyroTester();

    // The last traceLength readings for one axis (0 = X/pitch, 1 = Y/yaw, 2 = Z/roll), deg/s,
    // oldest first
    Q_INVOKABLE QVariantList trace(int axis, bool filtered) const;

    Q_INVOKABLE void recenter();

    // The gamepad's settings changed (say, on the same page): use them, keeping the offset found
    Q_INVOKABLE void reloadSettings();

    QString deviceKey() const { return m_DeviceKey; }
    void setDeviceKey(const QString& key);
    bool active() const { return m_Active; }
    void setActive(bool active);

    bool receiving() const { return m_Receiving; }
    double crosshairX() const { return m_CrosshairX; }
    double crosshairY() const { return m_CrosshairY; }
    double crosshairRange() const;
    double restRawDps() const { return m_RestRawDps; }
    double restFilteredDps() const { return m_RestFilteredDps; }
    QString offsetText() const;
    QString accelText() const;
    int traceLength() const;

signals:
    void deviceKeyChanged();
    void activeChanged();
    void updated();

private:
    void start();
    void stop();
    void onSensorEvent(const SDL_ControllerSensorEvent& event);
    void onPublishTimer();

    QString m_DeviceKey;
    bool m_Active;

    SDL_GameController* m_Gamepad;
    SDL_JoystickID m_JoystickId;
    GyroFilter m_Filter;

    // Ring buffers of readings, deg/s
    QVector<float> m_Raw[3];
    QVector<float> m_Filtered[3];
    int m_TraceNext;

    double m_CrosshairX, m_CrosshairY;
    uint64_t m_LastGyroTimeUs;

    // The current second's largest readings, and the last whole second's
    uint32_t m_RestWindowStart;
    float m_RestRawMax, m_RestFilteredMax;
    double m_RestRawDps, m_RestFilteredDps;

    uint32_t m_LastReadingTime;
    bool m_Receiving;
    bool m_Dirty;
    QTimer m_PublishTimer;
};
