#include "gameinput.h"

#include "input.h"
#include "streaming/session.h"

#include <Limelight.h>

using namespace GameInput::v4;

// Keep in sync with gamepad.cpp
#define MOUSE_EMULATION_LONG_PRESS_TIME 750
#define MOUSE_EMULATION_POLLING_INTERVAL 50

static constexpr uint16_t k_MicrosoftVendorId = 0x045E;

static short stickToShort(float value)
{
    if (value >= 1.0f) {
        return 32767;
    }
    if (value <= -1.0f) {
        return -32767;
    }
    return (short)(value * 32767.0f);
}

static unsigned char triggerToByte(float value)
{
    if (value >= 1.0f) {
        return 255;
    }
    if (value <= 0.0f) {
        return 0;
    }
    return (unsigned char)(value * 255.0f + 0.5f);
}

static int mapGamepadButtons(GameInputGamepadButtons buttons, bool swapFaceButtons)
{
    int flags = 0;

    if (buttons & GameInputGamepadA) flags |= swapFaceButtons ? B_FLAG : A_FLAG;
    if (buttons & GameInputGamepadB) flags |= swapFaceButtons ? A_FLAG : B_FLAG;
    if (buttons & GameInputGamepadX) flags |= swapFaceButtons ? Y_FLAG : X_FLAG;
    if (buttons & GameInputGamepadY) flags |= swapFaceButtons ? X_FLAG : Y_FLAG;
    if (buttons & GameInputGamepadMenu) flags |= PLAY_FLAG;
    if (buttons & GameInputGamepadView) flags |= BACK_FLAG;
    if (buttons & GameInputGamepadLeftShoulder) flags |= LB_FLAG;
    if (buttons & GameInputGamepadRightShoulder) flags |= RB_FLAG;
    if (buttons & GameInputGamepadLeftThumbstick) flags |= LS_CLK_FLAG;
    if (buttons & GameInputGamepadRightThumbstick) flags |= RS_CLK_FLAG;
    if (buttons & GameInputGamepadDPadUp) flags |= UP_FLAG;
    if (buttons & GameInputGamepadDPadDown) flags |= DOWN_FLAG;
    if (buttons & GameInputGamepadDPadLeft) flags |= LEFT_FLAG;
    if (buttons & GameInputGamepadDPadRight) flags |= RIGHT_FLAG;
    // Elite controller paddles (same order as SDL: upper right, upper left, lower right, lower left)
    if (buttons & GameInputGamepadPaddleRight1) flags |= PADDLE1_FLAG;
    if (buttons & GameInputGamepadPaddleLeft1) flags |= PADDLE2_FLAG;
    if (buttons & GameInputGamepadPaddleRight2) flags |= PADDLE3_FLAG;
    if (buttons & GameInputGamepadPaddleLeft2) flags |= PADDLE4_FLAG;

    return flags;
}

GameInputGamepads::GameInputGamepads(SdlInputHandler& handler, bool backgroundInput, bool swapFaceButtons)
    : m_Handler(handler),
      m_SwapFaceButtons(swapFaceButtons),
      m_BackgroundInput(backgroundInput),
      m_GameInput(nullptr),
      m_DeviceToken(0)
{
    HRESULT hr = GameInputCreate(&m_GameInput);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameInput is unavailable (0x%08lx); Xbox controllers will use SDL",
                    hr);
        m_GameInput = nullptr;
        return;
    }

    m_GameInput->EnableBackgroundInput(backgroundInput);

    // Existing devices are reported asynchronously through the same callback
    hr = m_GameInput->RegisterDeviceCallback(nullptr, GameInputKindGamepad, this, onDevice, &m_DeviceToken);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "GameInput RegisterDeviceCallback() failed (0x%08lx); Xbox controllers will use SDL",
                    hr);
        m_GameInput->Release();
        m_GameInput = nullptr;
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Using GameInput for Xbox controllers");
}

GameInputGamepads::~GameInputGamepads()
{
    if (m_GameInput == nullptr) {
        return;
    }

    m_ShuttingDown = true;
    m_GameInput->UnregisterCallback(m_DeviceToken);

    // Unregister reading callbacks without holding the lock (a callback may be waiting for it)
    std::map<int, Pad> pads;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        pads = m_Pads;
    }
    for (auto& [slot, pad] : pads) {
        m_GameInput->UnregisterCallback(pad.gamepadToken);
        m_GameInput->UnregisterCallback(pad.systemButtonToken);
    }

    // Wait for any callback that was already running when we unregistered
    while (m_CallbacksInFlight.load() > 0) {
        SDL_Delay(1);
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& [slot, pad] : m_Pads) {
            GameInputRumbleParams off {};
            pad.device->SetRumbleState(&off);
            pad.device->Release();
        }
        m_Pads.clear();
    }

    m_GameInput->Release();
}

bool GameInputGamepads::isClaimedDevice(IGameInputDevice* device)
{
    const GameInputDeviceFamily family = device->GetDeviceFamily();
    return device->GetVendorId() == k_MicrosoftVendorId &&
           (device->GetSupportedInput() & GameInputKindGamepad) &&
           (family == GameInputFamilyXboxOne || family == GameInputFamilyXbox360);
}

bool GameInputGamepads::claimsSdlDevice(int deviceIndex)
{
    if (SDL_JoystickGetDeviceVendor(deviceIndex) != k_MicrosoftVendorId) {
        return false;
    }

    switch (SDL_GameControllerTypeForIndex(deviceIndex)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE:
        return true;
    default:
        return false;
    }
}

void CALLBACK GameInputGamepads::onDevice(GameInputCallbackToken, void* context,
                                          GameInputDeviceEvent deviceEvent, IGameInputDevice* device)
{
    CallbackScope scope(static_cast<GameInputGamepads*>(context));
    if (!scope.active()) {
        return;
    }
    if (deviceEvent != GameInputDeviceEventConnected && deviceEvent != GameInputDeviceEventDisconnected) {
        return;
    }
    if (!isClaimedDevice(device)) {
        return;
    }

    // Slot allocation happens on the main thread. Reading callbacks must not be registered
    // from inside a GameInput callback either (GameInput fail-fasts).
    device->AddRef();

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = deviceEvent == GameInputDeviceEventConnected ?
                          SDL_CODE_GAMEINPUT_DEVICE_CONNECTED : SDL_CODE_GAMEINPUT_DEVICE_DISCONNECTED;
    event.user.data1 = device;
    event.user.data2 = context;
    if (SDL_PushEvent(&event) <= 0) {
        device->Release();
    }
}

bool GameInputGamepads::handleUserEvent(const SDL_UserEvent& event)
{
    switch (event.code) {
    case SDL_CODE_GAMEINPUT_DEVICE_CONNECTED:
        connectDevice((IGameInputDevice*)event.data1);
        ((IGameInputDevice*)event.data1)->Release();
        return true;
    case SDL_CODE_GAMEINPUT_DEVICE_DISCONNECTED:
        disconnectDevice((IGameInputDevice*)event.data1);
        ((IGameInputDevice*)event.data1)->Release();
        return true;
    case SDL_CODE_GAMEINPUT_ACTION:
        performAction((int)(intptr_t)event.data1, (Action)(intptr_t)event.data2);
        return true;
    default:
        return false;
    }
}

void GameInputGamepads::connectDevice(IGameInputDevice* device)
{
    if (m_GameInput == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& [slot, pad] : m_Pads) {
            if (pad.device == device) {
                return;  // duplicate arrival
            }
        }
    }

    // Find a free slot (shared with SDL gamepads)
    int i;
    for (i = 0; i < MAX_GAMEPADS; i++) {
        if (m_Handler.m_GamepadState[i].controller == nullptr && m_Handler.m_GamepadState[i].gameInputDevice == nullptr) {
            break;
        }
    }
    if (i == MAX_GAMEPADS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No open gamepad slots found for GameInput controller!");
        return;
    }

    GamepadState* state = &m_Handler.m_GamepadState[i];
    SDL_zerop(state);
    state->index = m_Handler.m_MultiController ? i : 0;
    state->jsId = -1;  // never matches an SDL joystick instance
    state->gameInputDevice = device;

    if (m_Handler.m_MultiController) {
        m_Handler.m_GamepadMask |= (1 << state->index);
    }

    Pad pad;
    pad.device = device;
    pad.device->AddRef();
    pad.slot = i;

    uint32_t supportedButtons = A_FLAG | B_FLAG | X_FLAG | Y_FLAG |
                                UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                                LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG |
                                LS_CLK_FLAG | RS_CLK_FLAG;
    const GameInputSystemButtons systemButtons = device->GetSupportedSystemButtons();
    if (systemButtons & GameInputSystemButtonGuide) {
        supportedButtons |= SPECIAL_FLAG;
    }
    if (systemButtons & GameInputSystemButtonShare) {
        supportedButtons |= MISC_FLAG;
    }
    GameInputGamepadInfo gamepadInfo = {};
    if (device->GetGamepadInfo(&gamepadInfo)) {
        supportedButtons |= mapGamepadButtons(gamepadInfo.supportedLayout, false) &
                            (PADDLE1_FLAG | PADDLE2_FLAG | PADDLE3_FLAG | PADDLE4_FLAG);
    }

    uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS;
    const GameInputRumbleMotors motors = device->GetSupportedRumbleMotors();
    if (motors & (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency)) {
        capabilities |= LI_CCAP_RUMBLE;
    }
    if (motors & (GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger)) {
        capabilities |= LI_CCAP_TRIGGER_RUMBLE;
    }

    char name[256] = {};
    device->GetDisplayName(sizeof(name), name);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Gamepad %d (player %d) is: %s via GameInput (VID/PID: 0x%.4x/0x%.4x) (rumble motors: 0x%x) (system buttons: 0x%x)",
                i, state->index, name, device->GetVendorId(), device->GetProductId(), motors, systemButtons);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Pads[i] = pad;
    }

    LiSendControllerArrivalEvent(state->index, m_Handler.m_GamepadMask, LI_CTYPE_XBOX, supportedButtons, capabilities);

    // Register reading callbacks last, from the main thread
    GameInputCallbackToken gamepadToken = 0;
    GameInputCallbackToken systemButtonToken = 0;
    HRESULT hr = m_GameInput->RegisterReadingCallback(device, GameInputKindGamepad, this, onGamepadReading, &gamepadToken);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GameInput gamepad reading callback failed: 0x%08lx", hr);
    }
    hr = m_GameInput->RegisterReadingCallback(device, GameInputKindSystemButton, this, onSystemButtonReading, &systemButtonToken);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "GameInput system button reading callback failed: 0x%08lx", hr);
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Pads.find(i);
    if (it != m_Pads.end()) {
        it->second.gamepadToken = gamepadToken;
        it->second.systemButtonToken = systemButtonToken;
    }
}

void GameInputGamepads::disconnectDevice(IGameInputDevice* device)
{
    Pad pad;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& [slot, p] : m_Pads) {
            if (p.device == device) {
                pad = p;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return;
    }

    // Stop callbacks before releasing the slot; never hold the lock while unregistering
    m_GameInput->UnregisterCallback(pad.gamepadToken);
    m_GameInput->UnregisterCallback(pad.systemButtonToken);

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Pads.erase(pad.slot);

        GamepadState* state = &m_Handler.m_GamepadState[pad.slot];
        if (state->mouseEmulationTimer != 0) {
            Session::get()->notifyMouseEmulationMode(false);
            SDL_RemoveTimer(state->mouseEmulationTimer);
        }

        if (m_Handler.m_MultiController) {
            m_Handler.m_GamepadMask &= ~(1 << state->index);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Gamepad %d (GameInput) is gone", state->index);

        // Send a final event to let the PC know this gamepad is gone
        LiSendMultiControllerEvent(state->index, m_Handler.m_GamepadMask, 0, 0, 0, 0, 0, 0, 0);

        SDL_zerop(state);
    }

    pad.device->Release();
}

void CALLBACK GameInputGamepads::onGamepadReading(GameInputCallbackToken, void* context, IGameInputReading* reading)
{
    auto self = static_cast<GameInputGamepads*>(context);
    CallbackScope scope(self);
    if (!scope.active()) {
        return;
    }

    IGameInputDevice* device = nullptr;
    reading->GetDevice(&device);

    GameInputGamepadState gamepad = {};
    if (device != nullptr && reading->GetGamepadState(&gamepad)) {
        std::lock_guard<std::mutex> lock(self->m_Mutex);
        for (auto& [slot, pad] : self->m_Pads) {
            if (pad.device == device) {
                GamepadState* state = &self->m_Handler.m_GamepadState[slot];
                pad.gamepadButtons = mapGamepadButtons(gamepad.buttons, self->m_SwapFaceButtons);
                state->lsX = stickToShort(gamepad.leftThumbstickX);
                state->lsY = stickToShort(gamepad.leftThumbstickY);  // both positive-up
                state->rsX = stickToShort(gamepad.rightThumbstickX);
                state->rsY = stickToShort(gamepad.rightThumbstickY);
                state->lt = triggerToByte(gamepad.leftTrigger);
                state->rt = triggerToByte(gamepad.rightTrigger);
                self->processReading(pad);
                break;
            }
        }
    }

    if (device != nullptr) {
        device->Release();
    }
}

void CALLBACK GameInputGamepads::onSystemButtonReading(GameInputCallbackToken, void* context, IGameInputReading* reading)
{
    auto self = static_cast<GameInputGamepads*>(context);
    CallbackScope scope(self);
    if (!scope.active()) {
        return;
    }

    IGameInputDevice* device = nullptr;
    reading->GetDevice(&device);

    GameInputSystemButtons buttons = GameInputSystemButtonNone;
    if (device != nullptr && reading->GetSystemButtonState(&buttons)) {
        std::lock_guard<std::mutex> lock(self->m_Mutex);
        for (auto& [slot, pad] : self->m_Pads) {
            if (pad.device == device) {
                pad.systemButtons = ((buttons & GameInputSystemButtonGuide) ? SPECIAL_FLAG : 0) |
                                    ((buttons & GameInputSystemButtonShare) ? MISC_FLAG : 0);
                self->processReading(pad);
                break;
            }
        }
    }

    if (device != nullptr) {
        device->Release();
    }
}

void GameInputGamepads::processReading(Pad& pad)
{
    GamepadState* state = &m_Handler.m_GamepadState[pad.slot];
    const int newButtons = pad.gamepadButtons | pad.systemButtons;
    const int changed = newButtons ^ state->buttons;
    const int pressed = changed & newButtons;
    const int released = changed & ~newButtons;

    // The overlay menu takes the buttons and the left stick while it's open. The buttons are
    // still tracked, so the ones held as it closes can be kept from the host.
    if (SdlInputHandler::s_OverlayMenuOpen) {
        state->buttons = newButtons;

        int menuButtons = pressed;
        int direction = SdlInputHandler::getOverlayMenuStickDirection(state->lsX, state->lsY, state->overlayMenuStickDirection);
        if (direction != state->overlayMenuStickDirection) {
            state->overlayMenuStickDirection = direction;
            menuButtons |= direction;
        }

        if (menuButtons != 0) {
            SDL_Event event = {};
            event.type = SDL_USEREVENT;
            event.user.code = SDL_CODE_OVERLAY_MENU_INPUT;
            event.user.data1 = (void*)(intptr_t)pad.slot;
            event.user.data2 = (void*)(intptr_t)menuButtons;
            SDL_PushEvent(&event);
        }

        m_Handler.updateOverlayMenuTrigger(state);
        return;
    }

    // A button held as the overlay menu closed was pressed for the menu, so its release is no
    // long press of Start and releases no mouse button
    const int hostReleased = released & ~state->overlayMenuSuppressedButtons;
    state->overlayMenuSuppressedButtons &= newButtons;

    if (pressed & PLAY_FLAG) {
        state->lastStartDownTime = SDL_GetTicks();
    }

    // Gamepad mouse mode: translate buttons into mouse/scroll input (mirrors gamepad.cpp)
    if (state->mouseEmulationTimer != 0) {
        static const struct { int flag; int button; } mouseButtons[] = {
            {A_FLAG, BUTTON_LEFT}, {B_FLAG, BUTTON_RIGHT}, {X_FLAG, BUTTON_MIDDLE},
            {LB_FLAG, BUTTON_X1}, {RB_FLAG, BUTTON_X2},
        };
        for (const auto& mapping : mouseButtons) {
            if (pressed & mapping.flag) {
                LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, mapping.button);
            }
            else if (hostReleased & mapping.flag) {
                LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, mapping.button);
            }
        }
        if (pressed & UP_FLAG) LiSendScrollEvent(1);
        if (pressed & DOWN_FLAG) LiSendScrollEvent(-1);
        if (pressed & RIGHT_FLAG) LiSendHScrollEvent(1);
        if (pressed & LEFT_FLAG) LiSendHScrollEvent(-1);
    }

    state->buttons = newButtons;

    // Holding the trigger (Select) alone opens the overlay menu
    m_Handler.updateOverlayMenuTrigger(state);

    // Long press of Start toggles gamepad mouse mode
    if ((hostReleased & PLAY_FLAG) && SDL_GetTicks() - state->lastStartDownTime > MOUSE_EMULATION_LONG_PRESS_TIME &&
        (state->mouseEmulationTimer != 0 || m_Handler.m_GamepadMouse)) {
        if (state->mouseEmulationTimer == 0) {
            // Send the Start release now; the host gets no gamepad input once mouse mode is on
            m_Handler.sendGamepadState(state);
        }
        postAction(pad.slot, ActionToggleMouseEmulation);
        return;
    }

    if (changed != 0) {
        // Start+Select+L1+R1 quits the stream
        if (newButtons == (PLAY_FLAG | BACK_FLAG | LB_FLAG | RB_FLAG) && qgetenv("NO_GAMEPAD_QUIT") != "1") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected quit gamepad button combo");
            postAction(pad.slot, ActionQuit);
            LiSendMultiControllerEvent(state->index, m_Handler.m_GamepadMask, 0, 0, 0, 0, 0, 0, 0);
            return;
        }

        // Select+L1+R1+X toggles the stats overlay
        if (newButtons == (BACK_FLAG | LB_FLAG | RB_FLAG | X_FLAG)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected stats toggle gamepad combo");
            postAction(pad.slot, ActionToggleStats);
            LiSendMultiControllerEvent(state->index, m_Handler.m_GamepadMask, 0, 0, 0, 0, 0, 0, 0);
            return;
        }
    }

    if (state->mouseEmulationTimer == 0) {
        m_Handler.sendGamepadState(state);
    }
}

void GameInputGamepads::postAction(int slot, Action action)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_GAMEINPUT_ACTION;
    event.user.data1 = (void*)(intptr_t)slot;
    event.user.data2 = (void*)(intptr_t)action;
    SDL_PushEvent(&event);
}

void GameInputGamepads::performAction(int slot, Action action)
{
    switch (action) {
    case ActionQuit: {
        SDL_Event event = {};
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        break;
    }

    case ActionToggleStats:
        Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayDebug,
                                                            !Session::get()->getOverlayManager().isOverlayEnabled(Overlay::OverlayDebug));
        break;

    case ActionToggleMouseEmulation:
        toggleMouseEmulation(slot, true);
        break;
    }
}

void GameInputGamepads::toggleMouseEmulation(int slot, bool requirePreference)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Pads.find(slot) == m_Pads.end()) {
        return;  // pad went away in the meantime
    }

    GamepadState* state = &m_Handler.m_GamepadState[slot];
    if (state->mouseEmulationTimer != 0) {
        SDL_RemoveTimer(state->mouseEmulationTimer);
        state->mouseEmulationTimer = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse emulation deactivated");
        Session::get()->notifyMouseEmulationMode(false);
    }
    else if (!requirePreference || m_Handler.m_GamepadMouse) {
        state->mouseEmulationTimer = SDL_AddTimer(MOUSE_EMULATION_POLLING_INTERVAL,
                                                  SdlInputHandler::mouseEmulationTimerCallback, state);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Mouse emulation active");
        Session::get()->notifyMouseEmulationMode(true);
    }
}

void GameInputGamepads::notifyOverlayMenuOpened()
{
    // Under the lock, so no reading sent before the menu opened can follow the neutral state
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [slot, pad] : m_Pads) {
        GamepadState* state = &m_Handler.m_GamepadState[slot];
        LiSendMultiControllerEvent(state->index, m_Handler.m_GamepadMask, 0, 0, 0, 0, 0, 0, 0);
        m_Handler.updateOverlayMenuTrigger(state);

        // A stick already pushed has to come back before it moves the selection
        state->overlayMenuStickDirection = SdlInputHandler::getOverlayMenuStickDirection(state->lsX, state->lsY, 0);
    }
}

void GameInputGamepads::notifyOverlayMenuClosed()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [slot, pad] : m_Pads) {
        GamepadState* state = &m_Handler.m_GamepadState[slot];
        state->overlayMenuSuppressedButtons = state->buttons;
        state->overlayMenuStickDirection = 0;
    }
}

void GameInputGamepads::notifyFocusLost()
{
    if (m_BackgroundInput) {
        return;  // GameInput keeps delivering readings in the background
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& [slot, pad] : m_Pads) {
        GamepadState* state = &m_Handler.m_GamepadState[slot];
        pad.gamepadButtons = 0;
        pad.systemButtons = 0;
        state->lsX = state->lsY = state->rsX = state->rsY = 0;
        state->lt = state->rt = 0;

        // Releasing a held Start here must not count as a long press (mouse mode toggle)
        state->lastStartDownTime = SDL_GetTicks();

        // Sends the neutral state, or releases mouse buttons when in gamepad mouse mode
        processReading(pad);
    }
}

bool GameInputGamepads::rumble(int slot, uint16_t lowFreqMotor, uint16_t highFreqMotor)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Pads.find(slot);
    if (it == m_Pads.end()) {
        return false;
    }

    it->second.rumble.lowFrequency = lowFreqMotor / 65535.0f;
    it->second.rumble.highFrequency = highFreqMotor / 65535.0f;
    it->second.device->SetRumbleState(&it->second.rumble);
    return true;
}

bool GameInputGamepads::rumbleTriggers(int slot, uint16_t leftTrigger, uint16_t rightTrigger)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Pads.find(slot);
    if (it == m_Pads.end()) {
        return false;
    }

    it->second.rumble.leftTrigger = leftTrigger / 65535.0f;
    it->second.rumble.rightTrigger = rightTrigger / 65535.0f;
    it->second.device->SetRumbleState(&it->second.rumble);
    return true;
}
