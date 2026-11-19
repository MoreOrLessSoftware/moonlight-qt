#include "gamepadsettings.h"

#include <QRegularExpression>
#include <QSettings>

#define SER_GAMEPADS        "gamepads"
#define SER_NAME            "name"          // SDL's name for it, to recognize the entry by
#define SER_CUSTOMNAME      "customName"    // the name it was given, if any
#define SER_SWAPFACEBUTTONS "swapFaceButtons"
// The setting for all gamepads that swapping A/B and X/Y used to be (in StreamingPreferences)
#define SER_LEGACY_SWAPFACEBUTTONS "swapfacebuttons"
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

QString GamepadSettings::deviceKeyForIndex(int deviceIndex)
{
    // SDL counts opens, so this doesn't disturb anyone who already has it open
    SDL_GameController* gamepad = SDL_GameControllerOpen(deviceIndex);
    if (gamepad == nullptr) {
        return QString();
    }
    QString key = deviceKey(gamepad);
    SDL_GameControllerClose(gamepad);
    return key;
}

QString GamepadSettings::defaultName(const QString& deviceKey, const QString& sdlName)
{
    if (!deviceKey.startsWith(QStringLiteral("serial:"))) {
        return sdlName;
    }

    // A Bluetooth address (12 hex digits, however they're separated) ends up as "0F:1F";
    // any other serial contributes its last 4 characters
    QString serial = deviceKey.mid(7);
    QString digits = serial;
    digits.remove('-').remove(':');
    static const QRegularExpression hexAddress(QStringLiteral("^[0-9A-Fa-f]{12}$"));
    QString suffix;
    if (hexAddress.match(digits).hasMatch()) {
        suffix = digits.right(4).toUpper();
        suffix.insert(2, ':');
    }
    else {
        suffix = serial.right(4);
    }

    return QStringLiteral("%1 (%2)").arg(sdlName, suffix);
}

QString GamepadSettings::displayName(const QString& deviceKey, const QString& sdlName)
{
    QSettings store;
    QString name = store.value(deviceGroup(deviceKey) + "/" SER_CUSTOMNAME).toString();
    return name.isEmpty() ? defaultName(deviceKey, sdlName) : name;
}

void GamepadSettings::saveCustomName(const QString& deviceKey, const QString& sdlName, const QString& name)
{
    QSettings store;
    store.beginGroup(deviceGroup(deviceKey));
    store.setValue(SER_NAME, sdlName);
    if (name.trimmed().isEmpty()) {
        store.remove(SER_CUSTOMNAME);
    }
    else {
        store.setValue(SER_CUSTOMNAME, name.trimmed());
    }
    store.endGroup();
}

void GamepadSettings::remove(const QString& deviceKey)
{
    QSettings store;
    store.remove(deviceGroup(deviceKey));
}

bool GamepadSettings::loadSwapFaceButtons(const QString& deviceKey)
{
    QSettings store;
    bool legacy = store.value(SER_LEGACY_SWAPFACEBUTTONS, false).toBool();
    return store.value(deviceGroup(deviceKey) + "/" SER_SWAPFACEBUTTONS, legacy).toBool();
}

void GamepadSettings::saveSwapFaceButtons(const QString& deviceKey, const QString& name, bool swap)
{
    QSettings store;
    store.beginGroup(deviceGroup(deviceKey));
    store.setValue(SER_NAME, name);
    store.setValue(SER_SWAPFACEBUTTONS, swap);
    store.endGroup();
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
