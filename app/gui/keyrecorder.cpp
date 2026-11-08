#include "keyrecorder.h"
#include "settings/overlaymenucommands.h"

#include <QGuiApplication>

#include <algorithm>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

// Recording stops after this long with no keys
#define KEY_RECORDER_IDLE_MS 15000

#ifdef Q_OS_WIN32
namespace {

// The recorder whose hook is installed. The hook is called on the thread that installed it
// (the main thread), from its message loop.
KeyRecorder* s_ActiveRecorder = nullptr;

LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && s_ActiveRecorder != nullptr) {
        auto info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        bool pressed = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        s_ActiveRecorder->handleKey((int)info->vkCode, pressed);

        // Taken, so Windows doesn't act on it (like opening Start for Win)
        return 1;
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

}
#endif

KeyRecorder::KeyRecorder(QObject* parent) :
    QObject(parent),
    m_Recording(false)
#ifdef Q_OS_WIN32
    , m_Hook(nullptr)
#endif
{
    m_IdleTimer.setSingleShot(true);
    m_IdleTimer.setInterval(KEY_RECORDER_IDLE_MS);
    connect(&m_IdleTimer, &QTimer::timeout, this, &KeyRecorder::stop);

    // Not while the app isn't in front, where the keys aren't meant for it
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) {
            stop();
        }
    });
}

KeyRecorder::~KeyRecorder()
{
    stop();
}

QString KeyRecorder::pressedKeys() const
{
    return OverlayMenuCommand::describeKeys({ m_Chord });
}

void KeyRecorder::start()
{
    if (m_Recording) {
        return;
    }

    m_Held.clear();
    m_Chord.clear();

#ifdef Q_OS_WIN32
    // Only one recorder has the keyboard at a time
    if (s_ActiveRecorder != nullptr) {
        s_ActiveRecorder->stop();
    }

    m_Hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
    if (m_Hook != nullptr) {
        s_ActiveRecorder = this;
    }
    // Without it, the keys come from QML instead (see keyEvent())
#endif

    m_Recording = true;
    m_IdleTimer.start();
    emit recordingChanged();
    emit pressedKeysChanged();
}

void KeyRecorder::stop()
{
    if (!m_Recording) {
        return;
    }

#ifdef Q_OS_WIN32
    if (m_Hook != nullptr) {
        UnhookWindowsHookEx((HHOOK)m_Hook);
        m_Hook = nullptr;
    }
    if (s_ActiveRecorder == this) {
        s_ActiveRecorder = nullptr;
    }
#endif

    m_Recording = false;
    m_IdleTimer.stop();
    m_Held.clear();
    m_Chord.clear();
    emit recordingChanged();
    emit pressedKeysChanged();
}

void KeyRecorder::keyEvent(int qtKey, quint32 nativeVirtualKey, bool pressed, bool autoRepeat)
{
#ifdef Q_OS_WIN32
    // The hook has them already
    if (m_Hook != nullptr) {
        return;
    }
#endif

    if (autoRepeat) {
        return;
    }

#ifdef Q_OS_WIN32
    // Qt's native virtual key is Windows' own
    int vk = (int)nativeVirtualKey;
#else
    (void)nativeVirtualKey;
    int vk = virtualKeyForQtKey(qtKey);
#endif
#ifdef Q_OS_WIN32
    (void)qtKey;
#endif

    if (vk > 0 && vk < 0x100) {
        handleKey(vk, pressed);
    }
}

void KeyRecorder::handleKey(int vk, bool pressed)
{
    if (!m_Recording) {
        return;
    }

    m_IdleTimer.start();

    if (pressed) {
        // Held keys repeat their presses
        if (m_Held.contains(vk)) {
            return;
        }
        m_Held.append(vk);
        if (!m_Chord.contains(vk)) {
            m_Chord.append(vk);
        }
        emit pressedKeysChanged();
        return;
    }

    // A key held since before recording started isn't part of anything
    if (!m_Held.removeAll(vk)) {
        return;
    }

    if (m_Held.isEmpty() && !m_Chord.isEmpty()) {
        // Modifiers first, as they're sent, whatever order they were pressed in
        static const QVector<int> modifiers = {
            0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C,
        };
        std::stable_partition(m_Chord.begin(), m_Chord.end(), [](int key) {
            return modifiers.contains(key);
        });

        QVariantList keys;
        for (int key : std::as_const(m_Chord)) {
            keys.append(key);
        }
        m_Chord.clear();

        emit chordRecorded(keys);
        emit pressedKeysChanged();
    }
}

int KeyRecorder::virtualKeyForQtKey(int qtKey)
{
    if ((qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9) || (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)) {
        // The same as their ASCII
        return qtKey;
    }
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) {
        return 0x70 + (qtKey - Qt::Key_F1);
    }

    switch (qtKey) {
    case Qt::Key_Backspace: return 0x08;
    case Qt::Key_Tab: return 0x09;
    case Qt::Key_Return:
    case Qt::Key_Enter: return 0x0D;
    case Qt::Key_Shift: return 0xA0;
    case Qt::Key_Control: return 0xA2;
    case Qt::Key_Alt: return 0xA4;
    case Qt::Key_Meta:
    case Qt::Key_Super_L: return 0x5B;
    case Qt::Key_Super_R: return 0x5C;
    case Qt::Key_Menu: return 0x5D;
    case Qt::Key_Pause: return 0x13;
    case Qt::Key_CapsLock: return 0x14;
    case Qt::Key_Escape: return 0x1B;
    case Qt::Key_Space: return 0x20;
    case Qt::Key_PageUp: return 0x21;
    case Qt::Key_PageDown: return 0x22;
    case Qt::Key_End: return 0x23;
    case Qt::Key_Home: return 0x24;
    case Qt::Key_Left: return 0x25;
    case Qt::Key_Up: return 0x26;
    case Qt::Key_Right: return 0x27;
    case Qt::Key_Down: return 0x28;
    case Qt::Key_Print: return 0x2C;
    case Qt::Key_Insert: return 0x2D;
    case Qt::Key_Delete: return 0x2E;
    case Qt::Key_NumLock: return 0x90;
    case Qt::Key_ScrollLock: return 0x91;
    case Qt::Key_VolumeMute: return 0xAD;
    case Qt::Key_VolumeDown: return 0xAE;
    case Qt::Key_VolumeUp: return 0xAF;
    case Qt::Key_MediaNext: return 0xB0;
    case Qt::Key_MediaPrevious: return 0xB1;
    case Qt::Key_MediaStop: return 0xB2;
    case Qt::Key_MediaPlay:
    case Qt::Key_MediaTogglePlayPause: return 0xB3;
    case Qt::Key_Semicolon: return 0xBA;
    case Qt::Key_Equal: return 0xBB;
    case Qt::Key_Comma: return 0xBC;
    case Qt::Key_Minus: return 0xBD;
    case Qt::Key_Period: return 0xBE;
    case Qt::Key_Slash: return 0xBF;
    case Qt::Key_QuoteLeft: return 0xC0;
    case Qt::Key_BracketLeft: return 0xDB;
    case Qt::Key_Backslash: return 0xDC;
    case Qt::Key_BracketRight: return 0xDD;
    case Qt::Key_Apostrophe: return 0xDE;
    default: return 0;
    }
}
