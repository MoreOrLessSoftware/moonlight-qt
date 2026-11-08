#include "streaming/session.h"

#include <Limelight.h>

#include <vector>
#include "SDL_compat.h"

#define VK_0 0x30
#define VK_A 0x41

// These are real Windows VK_* codes
#ifndef VK_F1
#define VK_F1 0x70
#define VK_F13 0x7C
#define VK_NUMPAD0 0x60
#endif

#define MAKE_KEYPRESS_STATE(code, modifiers, flags) \
    ((uint32_t)(uint16_t)(code) | \
    ((uint32_t)(uint8_t)((modifiers) & MODIFIER_EXTENDED) << 16) | \
    ((uint32_t)(uint8_t)(flags) << 24))

#define GET_KEYPRESS_CODE(x) ((short)((x) & 0xFFFF))
#define GET_KEYPRESS_EXTENDED_MODIFIER(x) ((char)(((x) >> 16) & 0xFF))
#define GET_KEYPRESS_FLAGS(x) ((char)(((x) >> 24) & 0xFF))

void SdlInputHandler::performSpecialKeyCombo(KeyCombo combo)
{
    switch (combo) {
    case KeyComboQuit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quit key combo");

        // Push a quit event to the main loop
        SDL_Event event;
        event.type = SDL_QUIT;
        event.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
        break;

    case KeyComboUngrabInput:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse capture toggle combo");

        // Stop handling future input
        setCaptureActive(!isCaptureActive());

        // Force raise all keys to ensure they aren't stuck,
        // since we won't get their key up events.
        raiseAllKeys();
        break;

    case KeyComboToggleFullScreen:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected full-screen toggle combo");
        Session::s_ActiveSession->toggleFullscreen();

        // Force raise all keys just be safe across this full-screen/windowed
        // transition just in case key events get lost.
        raiseAllKeys();
        break;

    case KeyComboToggleStatsOverlay: {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected stats toggle combo");

        // Cycle the stats overlay: off -> on -> latency measurement mode -> off
        Overlay::OverlayManager& overlays = Session::get()->getOverlayManager();
        if (!overlays.isOverlayEnabled(Overlay::OverlayDebug)) {
            overlays.setOverlayState(Overlay::OverlayDebug, true);
        }
        else if (!overlays.isOverlayLatencyMeasurementEnabled(Overlay::OverlayDebug)) {
            overlays.setOverlayLatencyMeasurement(Overlay::OverlayDebug, true);
        }
        else {
            // Disabling also leaves latency measurement mode
            overlays.setOverlayState(Overlay::OverlayDebug, false);
        }
        break;
    }

    case KeyComboToggleMouseMode:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected mouse mode toggle combo");

        // Uncapture input
        setCaptureActive(false);

        // Toggle mouse mode
        m_AbsoluteMouseMode = !m_AbsoluteMouseMode;

        // Recapture input
        setCaptureActive(true);
        break;

    case KeyComboToggleCursorHide:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected show mouse combo");

        if (!SDL_GetRelativeMouseMode()) {
            m_MouseCursorCapturedVisibilityState = !m_MouseCursorCapturedVisibilityState;
            SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Cursor can only be shown in remote desktop mouse mode");
        }
        break;

    case KeyComboToggleMinimize:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected minimize combo");
        SDL_MinimizeWindow(m_Window);
        break;

    case KeyComboPasteText:
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected type clipboard text combo");

        // Force raise all keys to ensure that none of them interfere
        // with the text we're going to type.
        raiseAllKeys();

        char* text;
        if (SDL_HasClipboardText() && (text = SDL_GetClipboardText()) != nullptr) {
            // Sending both CR and LF will lead to two newlines in the destination for
            // each newline in the source, so we fix up any CRLFs into just a single LF.
            for (char* c = text; *c != 0; c++) {
                if (*c == '\r' && *(c + 1) == '\n') {
                    // We're using strlen() rather than strlen() - 1 since we need to add 1
                    // to copy the null terminator which is not included in strlen()'s count.
                    memmove(c, c + 1, strlen(c));
                }
            }

            // Send this text to the PC
            LiSendUtf8TextEvent(text, (unsigned int)strlen(text));

            // SDL_GetClipboardText() allocates, so we must free
            SDL_free((void*)text);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "No text in clipboard to paste!");
        }
        break;
    }

    case KeyComboTogglePointerRegionLock:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected pointer region lock toggle combo");
        m_PointerRegionLockActive = !m_PointerRegionLockActive;

        // Remember that the user changed this manually, so we don't mess with it anymore
        // during windowed <-> full-screen transitions.
        m_PointerRegionLockToggledByUser = true;

        // Apply the new region lock
        updatePointerRegionLock();
        break;

    case KeyComboQuitAndExit:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected quitAndExit key combo");

        // Indicate that we want to exit afterwards
        Session::get()->setShouldExit(true);

        // Push a quit event to the main loop
        SDL_Event quitExitEvent;
        quitExitEvent.type = SDL_QUIT;
        quitExitEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&quitExitEvent);
        break;

    case KeyComboDisconnectOnly:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected disconnect-only key combo");

        // Leave the app running on the host even if quitAppAfter is set
        Session::get()->setLeaveHostAppRunning();

        // Push a quit event to the main loop
        SDL_Event disconnectEvent;
        disconnectEvent.type = SDL_QUIT;
        disconnectEvent.quit.timestamp = SDL_GetTicks();
        SDL_PushEvent(&disconnectEvent);
        break;

    case KeyComboOverlayMenu:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected overlay menu combo");

        if (s_OverlayMenuOpen) {
            Session::get()->closeOverlayMenu();
        }
        else {
            // Not from a gamepad, so its gamepad actions go to the first one
            Session::get()->openOverlayMenu(-1);
        }
        break;

    case KeyComboToggleKeyboardGrab:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected keyboard grab toggle combo");

        // Toggle the system key capture mode
        if (isSystemKeyCaptureActive()) {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_OFF;
        }
        else {
            m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
        }

        updateKeyboardGrabState();
        break;

    default:
        Q_UNREACHABLE();
    }
}

void SdlInputHandler::handleKeyEvent(SDL_KeyboardEvent* event)
{
    short keyCode;
    char modifiers;
    char flags;
    bool shouldNotConvertToScanCodeOnServer = false;

    if (event->repeat) {
        // Ignore repeat key down events
        SDL_assert(event->state == SDL_PRESSED);
        return;
    }

    // Check for our special key combos
    if ((event->state == SDL_PRESSED) &&
            (event->keysym.mod & KMOD_CTRL) &&
            (event->keysym.mod & KMOD_ALT) &&
            (event->keysym.mod & KMOD_SHIFT)) {
        // First we test the SDLK combos for matches,
        // that way we ensure that latin keyboard users
        // can match to the key they see on their keyboards.
        // If nothing matches that, we'll then go on to
        // checking scancodes so non-latin keyboard users
        // can have working hotkeys (though possibly in
        // odd positions). We must do all SDLK tests before
        // any scancode tests to avoid issues in cases
        // where the SDLK for one shortcut collides with
        // the scancode of another.

        for (int i = 0; i < KeyComboMax; i++) {
            if (m_SpecialKeyCombos[i].enabled && event->keysym.sym == m_SpecialKeyCombos[i].keyCode) {
                performSpecialKeyCombo(m_SpecialKeyCombos[i].keyCombo);
                return;
            }
        }

        for (int i = 0; i < KeyComboMax; i++) {
            if (m_SpecialKeyCombos[i].enabled && event->keysym.scancode == m_SpecialKeyCombos[i].scanCode) {
                performSpecialKeyCombo(m_SpecialKeyCombos[i].keyCombo);
                return;
            }
        }
    }

    // The overlay menu takes the keyboard while it's open: the arrow keys move, Enter or Space
    // picks, and Escape or Backspace goes back. Nothing goes to the host.
    if (s_OverlayMenuOpen) {
        if (event->state == SDL_PRESSED) {
            int menuButton = 0;
            switch (event->keysym.scancode) {
            case SDL_SCANCODE_UP: menuButton = UP_FLAG; break;
            case SDL_SCANCODE_DOWN: menuButton = DOWN_FLAG; break;
            case SDL_SCANCODE_LEFT: menuButton = LEFT_FLAG; break;
            case SDL_SCANCODE_RIGHT: menuButton = RIGHT_FLAG; break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE:
                menuButton = A_FLAG;
                break;
            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_BACKSPACE:
                menuButton = B_FLAG;
                break;
            default:
                break;
            }

            if (menuButton != 0) {
                Session::get()->handleOverlayMenuInput(menuButton, -1);
            }
        }
        return;
    }

    // Set modifier flags
    modifiers = 0;
    if (event->keysym.mod & KMOD_CTRL) {
        modifiers |= MODIFIER_CTRL;
    }
    if (event->keysym.mod & KMOD_ALT) {
        modifiers |= MODIFIER_ALT;
    }
    if (event->keysym.mod & KMOD_SHIFT) {
        modifiers |= MODIFIER_SHIFT;
    }
    if (event->keysym.mod & KMOD_GUI) {
        if (isSystemKeyCaptureActive()) {
            modifiers |= MODIFIER_META;
        }
    }

    // Set keycode. We explicitly use scancode here because GFE will try to correct
    // for AZERTY layouts on the host but it depends on receiving VK_ values matching
    // a QWERTY layout to work.
    if (event->keysym.scancode >= SDL_SCANCODE_1 && event->keysym.scancode <= SDL_SCANCODE_9) {
        // SDL defines SDL_SCANCODE_0 > SDL_SCANCODE_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_1) + VK_0 + 1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_A && event->keysym.scancode <= SDL_SCANCODE_Z) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_A) + VK_A;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F1 && event->keysym.scancode <= SDL_SCANCODE_F12) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F1) + VK_F1;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_F13 && event->keysym.scancode <= SDL_SCANCODE_F24) {
        keyCode = (event->keysym.scancode - SDL_SCANCODE_F13) + VK_F13;
    }
    else if (event->keysym.scancode >= SDL_SCANCODE_KP_1 && event->keysym.scancode <= SDL_SCANCODE_KP_9) {
        // SDL defines SDL_SCANCODE_KP_0 > SDL_SCANCODE_KP_9, so we need to handle that manually
        keyCode = (event->keysym.scancode - SDL_SCANCODE_KP_1) + VK_NUMPAD0 + 1;
    }
    else {
        switch (event->keysym.scancode) {
            case SDL_SCANCODE_BACKSPACE:
                keyCode = 0x08;
                break;
            case SDL_SCANCODE_TAB:
                keyCode = 0x09;
                break;
            case SDL_SCANCODE_CLEAR:
                keyCode = 0x0C;
                break;
            case SDL_SCANCODE_KP_ENTER:
                modifiers |= MODIFIER_EXTENDED;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_RETURN:
                keyCode = 0x0D;
                break;
            case SDL_SCANCODE_PAUSE:
                keyCode = 0x13;
                break;
            case SDL_SCANCODE_CAPSLOCK:
                keyCode = 0x14;
                break;
            case SDL_SCANCODE_ESCAPE:
                keyCode = 0x1B;
                break;
            case SDL_SCANCODE_SPACE:
                keyCode = 0x20;
                break;
            case SDL_SCANCODE_PAGEUP:
                keyCode = 0x21;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_PAGEDOWN:
                keyCode = 0x22;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_END:
                keyCode = 0x23;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_HOME:
                keyCode = 0x24;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_LEFT:
                keyCode = 0x25;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_UP:
                keyCode = 0x26;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_RIGHT:
                keyCode = 0x27;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_DOWN:
                keyCode = 0x28;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_SELECT:
                keyCode = 0x29;
                break;
            case SDL_SCANCODE_EXECUTE:
                keyCode = 0x2B;
                break;
            case SDL_SCANCODE_PRINTSCREEN:
                keyCode = 0x2C;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_INSERT:
                keyCode = 0x2D;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_DELETE:
                keyCode = 0x2E;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_HELP:
                keyCode = 0x2F;
                break;
            case SDL_SCANCODE_KP_0:
                // See comment above about why we only handle SDL_SCANCODE_KP_0 here
                keyCode = VK_NUMPAD0;
                break;
            case SDL_SCANCODE_0:
                // See comment above about why we only handle SDL_SCANCODE_0 here
                keyCode = VK_0;
                break;
            case SDL_SCANCODE_KP_MULTIPLY:
                keyCode = 0x6A;
                break;
            case SDL_SCANCODE_KP_PLUS:
                keyCode = 0x6B;
                break;
            case SDL_SCANCODE_KP_COMMA:
                keyCode = 0x6C;
                break;
            case SDL_SCANCODE_KP_MINUS:
                keyCode = 0x6D;
                break;
            case SDL_SCANCODE_KP_PERIOD:
                keyCode = 0x6E;
                break;
            case SDL_SCANCODE_KP_DIVIDE:
                keyCode = 0x6F;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_NUMLOCKCLEAR:
                keyCode = 0x90;
                break;
            case SDL_SCANCODE_SCROLLLOCK:
                keyCode = 0x91;
                break;
            case SDL_SCANCODE_LSHIFT:
                keyCode = 0xA0;
                break;
            case SDL_SCANCODE_RSHIFT:
                keyCode = 0xA1;
                break;
            case SDL_SCANCODE_LCTRL:
                keyCode = 0xA2;
                break;
            case SDL_SCANCODE_RCTRL:
                keyCode = 0xA3;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_LALT:
                keyCode = 0xA4;
                break;
            case SDL_SCANCODE_RALT:
                keyCode = 0xA5;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_LGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = 0x5B;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_RGUI:
                if (!isSystemKeyCaptureActive()) {
                    return;
                }
                keyCode = 0x5C;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_APPLICATION:
                keyCode = 0x5D;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_BACK:
                keyCode = 0xA6;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_FORWARD:
                keyCode = 0xA7;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_REFRESH:
                keyCode = 0xA8;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_STOP:
                keyCode = 0xA9;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_SEARCH:
                keyCode = 0xAA;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_BOOKMARKS:
                keyCode = 0xAB;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_AC_HOME:
                keyCode = 0xAC;
                modifiers |= MODIFIER_EXTENDED;
                break;
            case SDL_SCANCODE_SEMICOLON:
                keyCode = 0xBA;
                break;
            case SDL_SCANCODE_EQUALS:
                keyCode = 0xBB;
                break;
            case SDL_SCANCODE_COMMA:
                keyCode = 0xBC;
                break;
            case SDL_SCANCODE_MINUS:
                keyCode = 0xBD;
                break;
            case SDL_SCANCODE_PERIOD:
                keyCode = 0xBE;
                break;
            case SDL_SCANCODE_SLASH:
                keyCode = 0xBF;
                break;
            case SDL_SCANCODE_GRAVE:
                keyCode = 0xC0;
                break;
            case SDL_SCANCODE_LEFTBRACKET:
                keyCode = 0xDB;
                break;
            case SDL_SCANCODE_INTERNATIONAL3:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_BACKSLASH:
                keyCode = 0xDC;
                break;
            case SDL_SCANCODE_RIGHTBRACKET:
                keyCode = 0xDD;
                break;
            case SDL_SCANCODE_APOSTROPHE:
                keyCode = 0xDE;
                break;
            case SDL_SCANCODE_INTERNATIONAL1:
                shouldNotConvertToScanCodeOnServer = true;
                Q_FALLTHROUGH();
            case SDL_SCANCODE_NONUSBACKSLASH:
                keyCode = 0xE2;
                break;
            case SDL_SCANCODE_LANG1:
                keyCode = 0x1C;
                break;
            case SDL_SCANCODE_LANG2:
                keyCode = 0x1D;
                break;
            default:
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Unhandled button event: %d",
                             event->keysym.scancode);
                return;
        }
    }

    keyCode |= 0x8000;
    flags = shouldNotConvertToScanCodeOnServer ? SS_KBE_FLAG_NON_NORMALIZED : 0;

    // Track the key state so we always know which keys are down
    if (event->state == SDL_PRESSED) {
        m_KeysDown.insert(MAKE_KEYPRESS_STATE(keyCode, modifiers, flags));
    }
    else {
        m_KeysDown.remove(MAKE_KEYPRESS_STATE(keyCode, modifiers, flags));
    }

    LiSendKeyboardEvent2(keyCode,
                         event->state == SDL_PRESSED ?
                             KEY_ACTION_DOWN : KEY_ACTION_UP,
                         modifiers,
                         flags);
}

void SdlInputHandler::pasteClipboardText()
{
    performSpecialKeyCombo(KeyComboPasteText);
}

namespace {

// How long each key of a sequence waits for the next, as on the Android client, and how long
// one chord waits for the next
#define KEY_SEQUENCE_KEY_DELAY_MS 50
#define KEY_SEQUENCE_CHORD_DELAY_MS 100

struct KeySequenceEvent
{
    short keyCode;
    char action;
    char modifiers;
    Uint32 delayAfterMs;
};

struct KeySequence
{
    std::vector<KeySequenceEvent> events;
    size_t next = 0;
    SDL_Event done;
};

int modifierForKey(int vk)
{
    switch (vk) {
    case 0xA0: case 0xA1: return MODIFIER_SHIFT;
    case 0xA2: case 0xA3: return MODIFIER_CTRL;
    case 0xA4: case 0xA5: return MODIFIER_ALT;
    case 0x5B: case 0x5C: return MODIFIER_META;
    default: return 0;
    }
}

// The keys sent as extended, as handleKeyEvent() sends them
bool isExtendedKey(int vk)
{
    switch (vk) {
    case 0x21: case 0x22: case 0x23: case 0x24:     // Page Up, Page Down, End, Home
    case 0x25: case 0x26: case 0x27: case 0x28:     // Arrows
    case 0x2C: case 0x2D: case 0x2E:                // Print Screen, Insert, Delete
    case 0x5B: case 0x5C: case 0x5D:                // Windows keys, Menu
    case 0x6F: case 0x90:                           // Num /, Num Lock
    case 0xA3: case 0xA5:                           // Right Ctrl, Right Alt
        return true;
    default:
        return false;
    }
}

Uint32 keySequenceCallback(Uint32, void* param)
{
    auto sequence = static_cast<KeySequence*>(param);

    const KeySequenceEvent& event = sequence->events[sequence->next++];
    LiSendKeyboardEvent2(event.keyCode, event.action, event.modifiers, 0);

    if (sequence->next < sequence->events.size()) {
        return event.delayAfterMs;
    }

    SDL_PushEvent(&sequence->done);
    delete sequence;
    return 0;
}

}

void SdlInputHandler::sendKeySequence(const QVector<QVector<int>>& steps, Sint32 doneCode, void* doneData1, void* doneData2)
{
    auto sequence = new KeySequence();
    sequence->done = {};
    sequence->done.type = SDL_USEREVENT;
    sequence->done.user.code = doneCode;
    sequence->done.user.data1 = doneData1;
    sequence->done.user.data2 = doneData2;

    for (const QVector<int>& step : steps) {
        QVector<int> keys;
        for (int vk : step) {
            // The generic modifiers are sent as the left ones, as from a keyboard
            switch (vk) {
            case 0x10: vk = 0xA0; break;
            case 0x11: vk = 0xA2; break;
            case 0x12: vk = 0xA4; break;
            }
            keys.append(vk);
        }

        // Each key is pressed in order with the modifiers held so far (its own included, as
        // SDL reports a modifier key's own press), then released in reverse
        int modifiers = 0;
        for (int vk : keys) {
            modifiers |= modifierForKey(vk);
            sequence->events.push_back({ (short)(0x8000 | vk), KEY_ACTION_DOWN,
                                         (char)(modifiers | (isExtendedKey(vk) ? MODIFIER_EXTENDED : 0)),
                                         KEY_SEQUENCE_KEY_DELAY_MS });
        }
        for (int i = keys.size() - 1; i >= 0; i--) {
            int vk = keys[i];
            modifiers &= ~modifierForKey(vk);
            sequence->events.push_back({ (short)(0x8000 | vk), KEY_ACTION_UP,
                                         (char)(modifiers | (isExtendedKey(vk) ? MODIFIER_EXTENDED : 0)),
                                         KEY_SEQUENCE_KEY_DELAY_MS });
        }
        sequence->events.back().delayAfterMs = KEY_SEQUENCE_CHORD_DELAY_MS;
    }

    if (sequence->events.empty()) {
        SDL_PushEvent(&sequence->done);
        delete sequence;
        return;
    }

    SDL_AddTimer(1, keySequenceCallback, sequence);
}

void SdlInputHandler::raiseAllKeys()
{
    if (m_KeysDown.isEmpty()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Raising %d keys",
                (int)m_KeysDown.count());

    for (auto keyDown : std::as_const(m_KeysDown)) {
        LiSendKeyboardEvent2(GET_KEYPRESS_CODE(keyDown),
                             KEY_ACTION_UP,
                             GET_KEYPRESS_EXTENDED_MODIFIER(keyDown),
                             GET_KEYPRESS_FLAGS(keyDown));
    }

    m_KeysDown.clear();
}
