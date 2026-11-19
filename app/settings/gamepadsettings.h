#pragma once

#include "SDL_compat.h"
#include "streaming/input/gyrofilter.h"

#include <QString>

// Settings kept for each gamepad, stored under gamepads/<device key>/, with a group per kind of
// setting where there are several (gyro/). A gamepad that has never been set up gets the defaults.
namespace GamepadSettings
{
    // Identifies one physical gamepad: its serial (the Bluetooth address for a Bluetooth
    // controller) when SDL has one, otherwise its VID/PID and name. Clones often copy a real
    // controller's VID/PID, so the serial keeps them apart from the real thing.
    QString deviceKey(SDL_GameController* gamepad);

    // The same for a gamepad SDL hasn't opened (by device index), opening it briefly if need be
    QString deviceKeyForIndex(int deviceIndex);

    // The name to show for a gamepad: the one it was given, or else SDL's name for it followed by
    // the end of its serial (for a Bluetooth controller, the last two bytes of its address), so
    // two of the same model can be told apart
    QString displayName(const QString& deviceKey, const QString& sdlName);
    QString defaultName(const QString& deviceKey, const QString& sdlName);

    // An empty name goes back to the default
    void saveCustomName(const QString& deviceKey, const QString& sdlName, const QString& name);

    // Forgets everything saved for the gamepad, including its name
    void remove(const QString& deviceKey);

    // Swap A/B and X/Y (a Nintendo-style layout). Until it's set for a gamepad, it follows what
    // was once a setting for all gamepads, so turning that on carries over.
    bool loadSwapFaceButtons(const QString& deviceKey);
    void saveSwapFaceButtons(const QString& deviceKey, const QString& name, bool swap);

    GyroFilterSettings loadGyro(const QString& deviceKey);

    // The name is stored with the settings so they can be recognized without the gamepad
    void saveGyro(const QString& deviceKey, const QString& name, const GyroFilterSettings& settings);
}
