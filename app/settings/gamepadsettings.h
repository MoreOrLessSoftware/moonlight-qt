#pragma once

#include "SDL_compat.h"
#include "streaming/input/gyrofilter.h"

#include <QString>

// Settings kept for each gamepad, stored under gamepads/<device key>/ with a group per kind
// of setting (gyro/ for now). A gamepad that has never been set up gets the defaults.
namespace GamepadSettings
{
    // Identifies one physical gamepad: its serial (the Bluetooth address for a Bluetooth
    // controller) when SDL has one, otherwise its VID/PID and name. Clones often copy a real
    // controller's VID/PID, so the serial keeps them apart from the real thing.
    QString deviceKey(SDL_GameController* gamepad);

    GyroFilterSettings loadGyro(const QString& deviceKey);

    // The name is stored with the settings so they can be recognized without the gamepad
    void saveGyro(const QString& deviceKey, const QString& name, const GyroFilterSettings& settings);
}
