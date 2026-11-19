#include "gamepadsettings.h"

#include <QSettings>

#define SER_GAMEPADS        "gamepads"
#define SER_NAME            "name"
#define SER_GYRO            "gyro"
#define SER_ENABLED         "enabled"
#define SER_REMOVEOFFSET    "removeOffset"
#define SER_SMOOTHSAMPLES   "smoothSamples"
#define SER_SMOOTHBELOWDPS  "smoothBelowDps"
#define SER_DEADZONEDPS     "deadzoneDps"
#define SER_CORRECTACCEL    "correctAccel"

namespace {

// A QSettings group for the device: '/' and '\' would split it into more groups
QString deviceGroup(const QString& deviceKey)
{
    QString group = deviceKey;
    group.replace('/', '_').replace('\\', '_');
    return QStringLiteral(SER_GAMEPADS "/") + group;
}

}

QString GamepadSettings::deviceKey(SDL_GameController* gamepad)
{
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char* serial = SDL_GameControllerGetSerial(gamepad);
    if (serial != nullptr && serial[0] != 0) {
        return QStringLiteral("serial:") + QString::fromUtf8(serial);
    }
#endif

    const char* name = SDL_GameControllerName(gamepad);
    return QStringLiteral("id:%1:%2:%3")
            .arg(SDL_GameControllerGetVendor(gamepad), 4, 16, QChar('0'))
            .arg(SDL_GameControllerGetProduct(gamepad), 4, 16, QChar('0'))
            .arg(name != nullptr ? QString::fromUtf8(name) : QString());
}

GyroFilterSettings GamepadSettings::loadGyro(const QString& deviceKey)
{
    GyroFilterSettings defaults = GyroFilterSettings::defaults();
    GyroFilterSettings settings;

    QSettings store;
    store.beginGroup(deviceGroup(deviceKey) + "/" SER_GYRO);
    settings.enabled = store.value(SER_ENABLED, defaults.enabled).toBool();
    settings.removeOffset = store.value(SER_REMOVEOFFSET, defaults.removeOffset).toBool();
    settings.smoothSamples = store.value(SER_SMOOTHSAMPLES, defaults.smoothSamples).toInt();
    settings.smoothBelowDps = store.value(SER_SMOOTHBELOWDPS, defaults.smoothBelowDps).toInt();
    settings.deadzoneDps = store.value(SER_DEADZONEDPS, defaults.deadzoneDps).toInt();
    settings.correctAccel = store.value(SER_CORRECTACCEL, defaults.correctAccel).toBool();
    store.endGroup();

    return settings;
}

void GamepadSettings::saveGyro(const QString& deviceKey, const QString& name, const GyroFilterSettings& settings)
{
    QSettings store;
    store.beginGroup(deviceGroup(deviceKey));
    store.setValue(SER_NAME, name);
    store.beginGroup(SER_GYRO);
    store.setValue(SER_ENABLED, settings.enabled);
    store.setValue(SER_REMOVEOFFSET, settings.removeOffset);
    store.setValue(SER_SMOOTHSAMPLES, settings.smoothSamples);
    store.setValue(SER_SMOOTHBELOWDPS, settings.smoothBelowDps);
    store.setValue(SER_DEADZONEDPS, settings.deadzoneDps);
    store.setValue(SER_CORRECTACCEL, settings.correctAccel);
    store.endGroup();
    store.endGroup();
}
