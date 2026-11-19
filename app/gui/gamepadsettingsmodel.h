#pragma once

#include "settings/gamepadsettings.h"

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

// The connected gamepads and the settings of the one selected (see GamepadSettings), for the
// settings page. Settings are saved as soon as they change. Gyro settings are only meaningful
// when the selected gamepad has a gyro (hasGyro).
class GamepadSettingsModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QStringList gamepadNames READ gamepadNames NOTIFY gamepadsChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    Q_PROPERTY(QString currentKey READ currentKey NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    Q_PROPERTY(bool hasGyro READ hasGyro NOTIFY currentChanged)

    Q_PROPERTY(bool swapFaceButtons READ swapFaceButtons WRITE setSwapFaceButtons NOTIFY swapFaceButtonsChanged)

    Q_PROPERTY(bool gyroFilterEnabled READ gyroFilterEnabled WRITE setGyroFilterEnabled NOTIFY gyroSettingsChanged)
    Q_PROPERTY(bool gyroRemoveOffset READ gyroRemoveOffset WRITE setGyroRemoveOffset NOTIFY gyroSettingsChanged)
    Q_PROPERTY(int gyroSmoothSamples READ gyroSmoothSamples WRITE setGyroSmoothSamples NOTIFY gyroSettingsChanged)
    Q_PROPERTY(int gyroSmoothBelowDps READ gyroSmoothBelowDps WRITE setGyroSmoothBelowDps NOTIFY gyroSettingsChanged)
    Q_PROPERTY(int gyroDeadzoneDps READ gyroDeadzoneDps WRITE setGyroDeadzoneDps NOTIFY gyroSettingsChanged)
    Q_PROPERTY(bool gyroCorrectAccel READ gyroCorrectAccel WRITE setGyroCorrectAccel NOTIFY gyroSettingsChanged)
    Q_PROPERTY(int gyroMaxSmoothSamples READ gyroMaxSmoothSamples CONSTANT)

public:
    explicit GamepadSettingsModel(QObject* parent = nullptr);

    // Looks for gamepads that came or went and reloads the selected one's settings, which
    // another page may have changed
    Q_INVOKABLE void refresh();

    // Puts the selected gamepad's gyro settings back to the defaults, keeping it on or off
    Q_INVOKABLE void resetGyroSettings();

    QStringList gamepadNames() const;
    int currentIndex() const { return m_CurrentIndex; }
    void setCurrentIndex(int index);
    QString currentKey() const;
    QString currentName() const;
    bool hasGyro() const;

    bool swapFaceButtons() const { return m_SwapFaceButtons; }
    void setSwapFaceButtons(bool value);

    bool gyroFilterEnabled() const { return m_Gyro.enabled; }
    bool gyroRemoveOffset() const { return m_Gyro.removeOffset; }
    int gyroSmoothSamples() const { return m_Gyro.smoothSamples; }
    int gyroSmoothBelowDps() const { return m_Gyro.smoothBelowDps; }
    int gyroDeadzoneDps() const { return m_Gyro.deadzoneDps; }
    bool gyroCorrectAccel() const { return m_Gyro.correctAccel; }
    int gyroMaxSmoothSamples() const { return GyroFilter::MaxSmoothSamples; }

    void setGyroFilterEnabled(bool value);
    void setGyroRemoveOffset(bool value);
    void setGyroSmoothSamples(int value);
    void setGyroSmoothBelowDps(int value);
    void setGyroDeadzoneDps(int value);
    void setGyroCorrectAccel(bool value);

signals:
    void gamepadsChanged();
    void currentChanged();
    void swapFaceButtonsChanged();
    void gyroSettingsChanged();

private:
    struct Gamepad {
        QString key;
        QString name;
        bool hasGyro;

        bool operator==(const Gamepad& other) const {
            return key == other.key && name == other.name && hasGyro == other.hasGyro;
        }
    };

    static QVector<Gamepad> connectedGamepads();
    void loadCurrent();
    void saveGyro();

    QVector<Gamepad> m_Gamepads;
    int m_CurrentIndex;
    bool m_SwapFaceButtons;
    GyroFilterSettings m_Gyro;
    QTimer m_RefreshTimer;
};
