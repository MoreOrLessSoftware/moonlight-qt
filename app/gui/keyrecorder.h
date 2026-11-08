#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVector>

// Records chords of keys for the in-stream menu's custom commands, as Windows virtual key codes.
// A chord is the keys pressed together, ending once they're all released, with any modifiers
// first.
//
// On Windows a low-level keyboard hook takes the keys while recording, so keys Windows would
// act on (Win, Alt+Tab, Print Screen) are recorded rather than acted on. Elsewhere, QML passes
// key events in through keyEvent(). Recording stops by itself after a while with no keys, or
// when the app loses focus, so the keyboard can't be left captured.
class KeyRecorder : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    // The keys of the chord being pressed, like "Ctrl+Shift"
    Q_PROPERTY(QString pressedKeys READ pressedKeys NOTIFY pressedKeysChanged)

public:
    explicit KeyRecorder(QObject* parent = nullptr);
    ~KeyRecorder();

    bool isRecording() const { return m_Recording; }
    QString pressedKeys() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

    // A key event from QML while recording, where the hook isn't taking them
    Q_INVOKABLE void keyEvent(int qtKey, quint32 nativeVirtualKey, bool pressed, bool autoRepeat);

    // A key from either, as a Windows virtual key code
    void handleKey(int vk, bool pressed);

signals:
    void recordingChanged();
    void pressedKeysChanged();
    // A chord's keys, as Windows virtual key codes
    void chordRecorded(const QVariantList& keys);

private:
    static int virtualKeyForQtKey(int qtKey);

    bool m_Recording;
    // Keys held now, and the chord's keys so far, in the order pressed
    QVector<int> m_Held;
    QVector<int> m_Chord;
    QTimer m_IdleTimer;

#ifdef Q_OS_WIN32
    // The keyboard hook (an HHOOK) while recording
    void* m_Hook;
#endif
};
