#pragma once

// GameInput (v4) gamepad path for Windows.
//
// Microsoft Xbox controllers (vendor 0x045E) are read through GameInput instead of SDL:
// readings arrive on GameInput's callback thread and are sent to the host from there
// (LiSendMultiControllerEvent is thread-safe), which skips SDL's joystick polling and the
// SDL event loop. Guide and Share come from GameInput's system button readings, and the
// impulse trigger motors are driven through SetRumbleState().
//
// Gamepad slots are shared with SdlInputHandler (m_GamepadState / m_GamepadMask). Slot
// allocation, removal, and anything that touches SDL timers or the session overlay run on
// the SDL main thread via SDL user events (SDL_CODE_GAMEINPUT_*).
//
// Only include this header from Windows translation units (it pulls in <windows.h>).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "gameinput/GameInput.h"

#include "SDL_compat.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>

class SdlInputHandler;
struct GamepadState;

// SDL user event codes posted from GameInput threads to the main loop
#define SDL_CODE_GAMEINPUT_DEVICE_CONNECTED    120
#define SDL_CODE_GAMEINPUT_DEVICE_DISCONNECTED 121
#define SDL_CODE_GAMEINPUT_ACTION              122

class GameInputGamepads
{
public:
    GameInputGamepads(SdlInputHandler& handler, bool backgroundInput, bool swapFaceButtons);
    ~GameInputGamepads();

    // True when the GameInput runtime is present and device enumeration is running
    bool isAvailable() const { return m_GameInput != nullptr; }

    // Whether an SDL device (by device index) should be left to GameInput (Microsoft Xbox controllers)
    static bool claimsSdlDevice(int deviceIndex);

    // Main thread: handle an SDL_CODE_GAMEINPUT_* user event. Returns false if the code is not ours.
    bool handleUserEvent(const SDL_UserEvent& event);

    // Main thread: rumble requests from the host. Return false if the slot is not a GameInput pad.
    bool rumble(int slot, uint16_t lowFreqMotor, uint16_t highFreqMotor);
    bool rumbleTriggers(int slot, uint16_t leftTrigger, uint16_t rightTrigger);

    // Main thread: the overlay menu opened or closed (see SdlInputHandler::notifyOverlayMenuOpened())
    void notifyOverlayMenuOpened();
    void notifyOverlayMenuClosed();

    // Main thread: turns gamepad mouse mode on or off for a slot. Turning it on can be left to
    // the gamepad mouse setting, as for a long press of Start.
    void toggleMouseEmulation(int slot, bool requirePreference);

    // Main thread: the window lost focus. Without background input GameInput stops sending readings, so
    // release everything on the host instead of leaving buttons or triggers held.
    void notifyFocusLost();

private:
    enum Action {
        ActionQuit,
        ActionToggleStats,
        ActionToggleMouseEmulation,
    };

    struct Pad {
        GameInput::v4::IGameInputDevice* device = nullptr;
        int slot = -1;                      // index into SdlInputHandler::m_GamepadState
        GameInput::v4::GameInputCallbackToken gamepadToken = 0;
        GameInput::v4::GameInputCallbackToken systemButtonToken = 0;
        int gamepadButtons = 0;             // Moonlight flags from the gamepad reading
        int systemButtons = 0;              // SPECIAL_FLAG / MISC_FLAG from the system button reading
        GameInput::v4::GameInputRumbleParams rumble {};
    };

    static void CALLBACK onDevice(GameInput::v4::GameInputCallbackToken token, void* context,
                                  GameInput::v4::GameInputDeviceEvent deviceEvent,
                                  GameInput::v4::IGameInputDevice* device);
    static void CALLBACK onGamepadReading(GameInput::v4::GameInputCallbackToken token, void* context,
                                          GameInput::v4::IGameInputReading* reading);
    static void CALLBACK onSystemButtonReading(GameInput::v4::GameInputCallbackToken token, void* context,
                                               GameInput::v4::IGameInputReading* reading);

    static bool isClaimedDevice(GameInput::v4::IGameInputDevice* device);

    // Main thread
    void connectDevice(GameInput::v4::IGameInputDevice* device);
    void disconnectDevice(GameInput::v4::IGameInputDevice* device);
    void performAction(int slot, Action action);

    // GameInput callback thread, with m_Mutex held
    void processReading(Pad& pad);
    void postAction(int slot, Action action);

    SdlInputHandler& m_Handler;
    bool m_SwapFaceButtons;
    bool m_BackgroundInput;
    GameInput::v4::IGameInput* m_GameInput;
    GameInput::v4::GameInputCallbackToken m_DeviceToken;

    std::mutex m_Mutex;                 // guards m_Pads and the GamepadState slots owned by GameInput
    std::map<int, Pad> m_Pads;          // keyed by slot

    // Shutdown guard: callbacks still running after UnregisterCallback() must not touch us
    std::atomic<bool> m_ShuttingDown { false };
    std::atomic<int> m_CallbacksInFlight { 0 };

    class CallbackScope {
    public:
        explicit CallbackScope(GameInputGamepads* self) : m_Self(self) { m_Self->m_CallbacksInFlight++; }
        ~CallbackScope() { m_Self->m_CallbacksInFlight--; }
        bool active() const { return !m_Self->m_ShuttingDown; }
    private:
        GameInputGamepads* m_Self;
    };
};
