#include "gamepadsettingsmodel.h"

// How often to look for gamepads that came or went
#define REFRESH_INTERVAL_MS 1000

GamepadSettingsModel::GamepadSettingsModel(QObject* parent)
    : QObject(parent),
      m_CurrentIndex(-1),
      m_Gyro(GyroFilterSettings::defaults())
{
    connect(&m_RefreshTimer, &QTimer::timeout, this, &GamepadSettingsModel::refresh);
    m_RefreshTimer.start(REFRESH_INTERVAL_MS);
    refresh();
}

QVector<GamepadSettingsModel::Gamepad> GamepadSettingsModel::connectedGamepads()
{
    QVector<Gamepad> gamepads;

    // SdlGamepadKeyNavigation keeps the subsystem up while the GUI is showing
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        return gamepads;
    }

    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }

        // SDL counts opens, so this doesn't disturb SdlGamepadKeyNavigation's handle
        SDL_GameController* gamepad = SDL_GameControllerOpen(i);
        if (gamepad == nullptr) {
            continue;
        }

        Gamepad entry;
        entry.key = GamepadSettings::deviceKey(gamepad);
        const char* name = SDL_GameControllerName(gamepad);
        entry.name = name != nullptr ? QString::fromUtf8(name) : QObject::tr("Gamepad");
#if SDL_VERSION_ATLEAST(2, 0, 14)
        entry.hasGyro = SDL_GameControllerHasSensor(gamepad, SDL_SENSOR_GYRO);
#else
        entry.hasGyro = false;
#endif
        SDL_GameControllerClose(gamepad);

        // The same gamepad can show up twice (say, over USB and Bluetooth at once)
        bool duplicate = false;
        for (const Gamepad& existing : std::as_const(gamepads)) {
            if (existing.key == entry.key) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            gamepads.append(entry);
        }
    }

    return gamepads;
}

void GamepadSettingsModel::refresh()
{
    QVector<Gamepad> gamepads = connectedGamepads();

    if (gamepads != m_Gamepads) {
        QString previousKey = currentKey();
        m_Gamepads = gamepads;
        emit gamepadsChanged();

        // Keep the same gamepad selected if it's still here
        int index = m_Gamepads.isEmpty() ? -1 : 0;
        for (int i = 0; i < m_Gamepads.size(); i++) {
            if (m_Gamepads[i].key == previousKey) {
                index = i;
                break;
            }
        }
        m_CurrentIndex = index;
        emit currentChanged();
    }

    loadCurrent();
}

void GamepadSettingsModel::loadCurrent()
{
    GyroFilterSettings settings = m_CurrentIndex >= 0 ?
                GamepadSettings::loadGyro(m_Gamepads[m_CurrentIndex].key) :
                GyroFilterSettings::defaults();
    if (settings.enabled != m_Gyro.enabled ||
            settings.removeOffset != m_Gyro.removeOffset ||
            settings.smoothSamples != m_Gyro.smoothSamples ||
            settings.smoothBelowDps != m_Gyro.smoothBelowDps ||
            settings.deadzoneDps != m_Gyro.deadzoneDps ||
            settings.correctAccel != m_Gyro.correctAccel) {
        m_Gyro = settings;
        emit gyroSettingsChanged();
    }
}

void GamepadSettingsModel::saveGyro()
{
    if (m_CurrentIndex >= 0) {
        GamepadSettings::saveGyro(m_Gamepads[m_CurrentIndex].key, m_Gamepads[m_CurrentIndex].name, m_Gyro);
    }
    emit gyroSettingsChanged();
}

void GamepadSettingsModel::resetGyroSettings()
{
    bool enabled = m_Gyro.enabled;
    m_Gyro = GyroFilterSettings::defaults();
    m_Gyro.enabled = enabled;
    saveGyro();
}

QStringList GamepadSettingsModel::gamepadNames() const
{
    QStringList names;
    for (const Gamepad& gamepad : m_Gamepads) {
        names.append(gamepad.name);
    }
    return names;
}

void GamepadSettingsModel::setCurrentIndex(int index)
{
    if (index < -1 || index >= m_Gamepads.size() || index == m_CurrentIndex) {
        return;
    }
    m_CurrentIndex = index;
    emit currentChanged();
    loadCurrent();
}

QString GamepadSettingsModel::currentKey() const
{
    return m_CurrentIndex >= 0 ? m_Gamepads[m_CurrentIndex].key : QString();
}

QString GamepadSettingsModel::currentName() const
{
    return m_CurrentIndex >= 0 ? m_Gamepads[m_CurrentIndex].name : QString();
}

bool GamepadSettingsModel::hasGyro() const
{
    return m_CurrentIndex >= 0 && m_Gamepads[m_CurrentIndex].hasGyro;
}

void GamepadSettingsModel::setGyroFilterEnabled(bool value)
{
    if (m_Gyro.enabled != value) {
        m_Gyro.enabled = value;
        saveGyro();
    }
}

void GamepadSettingsModel::setGyroRemoveOffset(bool value)
{
    if (m_Gyro.removeOffset != value) {
        m_Gyro.removeOffset = value;
        saveGyro();
    }
}

void GamepadSettingsModel::setGyroSmoothSamples(int value)
{
    value = qBound(1, value, (int)GyroFilter::MaxSmoothSamples);
    if (m_Gyro.smoothSamples != value) {
        m_Gyro.smoothSamples = value;
        saveGyro();
    }
}

void GamepadSettingsModel::setGyroSmoothBelowDps(int value)
{
    value = qMax(value, 0);
    if (m_Gyro.smoothBelowDps != value) {
        m_Gyro.smoothBelowDps = value;
        saveGyro();
    }
}

void GamepadSettingsModel::setGyroDeadzoneDps(int value)
{
    value = qMax(value, 0);
    if (m_Gyro.deadzoneDps != value) {
        m_Gyro.deadzoneDps = value;
        saveGyro();
    }
}

void GamepadSettingsModel::setGyroCorrectAccel(bool value)
{
    if (m_Gyro.correctAccel != value) {
        m_Gyro.correctAccel = value;
        saveGyro();
    }
}
