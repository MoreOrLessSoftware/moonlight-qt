#pragma once

#include <QSemaphore>
#include <QPoint>
#include <QWindow>

#include <Limelight.h>
#include <opus_multistream.h>
#include "settings/streamingpreferences.h"
#include "input/input.h"
#include "video/decoder.h"
#include "audio/renderers/renderer.h"
#include "video/overlaymanager.h"
#include "overlaymenu.h"
#include "hostcommands.h"
#include "settings/overlaymenucommands.h"

// A custom command's keys all sent (data1: its post action, data2: the session's token)
#define SDL_CODE_OVERLAY_MENU_COMMAND_DONE 108
// A message on one of the overlay menu's buttons timing out (data1: which one)
#define SDL_CODE_OVERLAY_MENU_MESSAGE_TIMEOUT 111
// Time to turn the spinner on a busy overlay menu button (data1: the session's token, data2:
// which run of a host command)
#define SDL_CODE_OVERLAY_MENU_SPINNER 114

class SupportedVideoFormatList : public QList<int>
{
public:
    operator int() const
    {
        int value = 0;

        for (const int v : *this) {
            value |= v;
        }

        return value;
    }

    void
    removeByMask(int mask)
    {
        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                this->removeAt(i);
            }
            else {
                i++;
            }
        }
    }

    void
    deprioritizeByMask(int mask)
    {
        QList<int> deprioritizedList;

        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                deprioritizedList.append(this->takeAt(i));
            }
            else {
                i++;
            }
        }

        this->append(std::move(deprioritizedList));
    }

    int maskByServerCodecModes(int serverCodecModes)
    {
        int mask = 0;

        const QMap<int, int> mapping = {
            {SCM_H264, VIDEO_FORMAT_H264},
            {SCM_H264_HIGH8_444, VIDEO_FORMAT_H264_HIGH8_444},
            {SCM_HEVC, VIDEO_FORMAT_H265},
            {SCM_HEVC_MAIN10, VIDEO_FORMAT_H265_MAIN10},
            {SCM_HEVC_REXT8_444, VIDEO_FORMAT_H265_REXT8_444},
            {SCM_HEVC_REXT10_444, VIDEO_FORMAT_H265_REXT10_444},
            {SCM_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN8},
            {SCM_AV1_MAIN10, VIDEO_FORMAT_AV1_MAIN10},
            {SCM_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH8_444},
            {SCM_AV1_HIGH10_444, VIDEO_FORMAT_AV1_HIGH10_444},
            {SCM_PYROWAVE, VIDEO_FORMAT_PYROWAVE},
            {SCM_PYROWAVE_10BIT, VIDEO_FORMAT_PYROWAVE_10BIT},
            {SCM_PYROWAVE_444, VIDEO_FORMAT_PYROWAVE_444},
            {SCM_PYROWAVE_10BIT_444, VIDEO_FORMAT_PYROWAVE_10BIT_444},
        };

        for (QMap<int, int>::const_iterator it = mapping.cbegin(); it != mapping.cend(); ++it) {
            if (serverCodecModes & it.key()) {
                mask |= it.value();
                serverCodecModes &= ~it.key();
            }
        }

        // Make sure nobody forgets to update this for new SCM values
        SDL_assert(serverCodecModes == 0);

        int val = *this;
        return val & mask;
    }
};

class Session : public QObject
{
    Q_OBJECT

    friend class SdlInputHandler;
    friend class GameInputGamepads;
    friend class DeferredSessionCleanupTask;
    friend class AsyncConnectionStartThread;

public:
    explicit Session(NvComputer* computer, NvApp& app, StreamingPreferences *preferences = nullptr);
    virtual ~Session();

    Q_INVOKABLE bool initialize(QWindow* qtWindow);

    // Puts the stream window on the display whose top left corner is here, instead of
    // the one the Qt window is on. Set before initialize().
    void setDisplayOrigin(const QPoint& origin);

    NvComputer* getComputer() const
    {
        return m_Computer;
    }

    const NvApp& getApp() const
    {
        return m_App;
    }
    Q_INVOKABLE void start();
    Q_INVOKABLE void interrupt();
    Q_PROPERTY(QStringList launchWarnings MEMBER m_LaunchWarnings NOTIFY launchWarningsChanged);

    static
    void getDecoderInfo(SDL_Window* window,
                        bool& isHardwareAccelerated, bool& isFullScreenOnly,
                        bool& isHdrSupported, QSize& maxResolution);

    static Session* get()
    {
        return s_ActiveSession;
    }

    Overlay::OverlayManager& getOverlayManager()
    {
        return m_OverlayManager;
    }

    void flushWindowEvents();

    void setShouldExit(bool quitHostApp = false);

    // Disconnect without quitting the host app, regardless of the quitAppAfter preference
    void setLeaveHostAppRunning() { m_LeaveHostAppRunning = true; }

    // The overlay menu (sessionoverlaymenu.cpp). It's opened by the gamepad in the given slot,
    // or -1 for the keyboard, and takes menu buttons (D-pad, A, B and so on) from either.
    void openOverlayMenu(int slot);
    void closeOverlayMenu();
    void handleOverlayMenuInput(int buttons, int slot);
    // Mouse input in window coordinates, as SDL's events have it
    void handleOverlayMenuMouseButton(int x, int y, int button, bool pressed);
    void handleOverlayMenuMouseMotion(int x, int y);

signals:
    void stageStarting(QString stage);

    void stageFailed(QString stage, int errorCode, QString failingPorts);

    void connectionStarted();

    void displayLaunchError(QString text);

    void quitStarting();

    void sessionFinished(int portTestResult);

    // Emitted after sessionFinished() when the session is ready to be destroyed
    void readyForDeletion();

    void launchWarningsChanged();

private:
    void exec();

    bool startConnectionAsync();

    bool validateLaunch(SDL_Window* testWindow);

    void emitLaunchWarning(QString text);

    bool populateDecoderProperties(SDL_Window* window);

    IAudioRenderer* createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);

    bool initializeAudioRenderer();

    bool testAudio(int audioConfiguration);

    int getAudioRendererCapabilities(int audioConfiguration);

    void getWindowDimensions(int& x, int& y,
                             int& width, int& height);

    void toggleFullscreen();

    void notifyMouseEmulationMode(bool enabled);

    void activateOverlayMenuAction(int action);

    void pickOverlayMenuOption(int option);

    void finishOverlayMenuCommand(const SDL_UserEvent& event);

    // The host's commands for the overlay menu (see HostCommandsClient)
    void fetchHostCommands();
    void handleHostCommandsFetched(const SDL_UserEvent& event);
    void runHostCommand(const HostCommand& command, int action);
    void handleHostCommandRan(const SDL_UserEvent& event);
    void showOverlayMenuMessage(int action, const QString& message);
    // Redraws the menu after a while, turning the spinner of the host command running
    void scheduleHostCommandSpinner(int delayMs);
    void handleHostCommandSpinner(const SDL_UserEvent& event);

    void updateOptimalWindowDisplayMode();

    enum class DecoderAvailability {
        None,
        Software,
        Hardware
    };

    static
    DecoderAvailability getDecoderAvailability(SDL_Window* window,
                                               StreamingPreferences::VideoDecoderSelection vds,
                                               int videoFormat, int width, int height, int frameRate);

    static
    bool chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                       StreamingPreferences::RendererSelection renderer,
                       SDL_Window* window, int videoFormat, int width, int height,
                       int frameRate, bool enableVsync, bool enableFramePacing,
                       bool testOnly,
                       IVideoDecoder*& chosenDecoder);

    static
    void clStageStarting(int stage);

    static
    void clStageFailed(int stage, int errorCode);

    static
    void clConnectionTerminated(int errorCode);

    static
    void clLogMessage(const char* format, ...);

    static
    void clRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

    static
    void clConnectionStatusUpdate(int connectionStatus);

    static
    void clSetHdrMode(bool enabled);

    static
    void clRumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    static
    void clSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    static
    void clSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    static
    void clSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

    static
    void clSetControllerPlayerLEDs(uint16_t controllerNumber, uint8_t solidMask, uint8_t flashingMask, uint8_t ds5Leds);

    static
    void clSetControllerMicLED(uint16_t controllerNumber, uint8_t state);

    static
    int arInit(int audioConfiguration,
               const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
               void* arContext, int arFlags);

    static
    void arCleanup();

    static
    void arDecodeAndPlaySample(char* sampleData, int sampleLength);

    static
    int drSetup(int videoFormat, int width, int height, int frameRate, void*, int);

    static
    void drCleanup();

    static
    int drSubmitDecodeUnit(PDECODE_UNIT du);

    StreamingPreferences* m_Preferences;
    bool m_IsFullScreen;
    SupportedVideoFormatList m_SupportedVideoFormats; // Sorted in order of descending priority
    STREAM_CONFIGURATION m_StreamConfig;
    DECODER_RENDERER_CALLBACKS m_VideoCallbacks;
    AUDIO_RENDERER_CALLBACKS m_AudioCallbacks;
    NvComputer* m_Computer;
    NvApp m_App;
    SDL_Window* m_Window;
    IVideoDecoder* m_VideoDecoder;
    SDL_mutex* m_DecoderLock;
    bool m_AudioDisabled;
    bool m_AudioMuted;
    Uint32 m_FullScreenFlag;
    QWindow* m_QtWindow;
    QPoint m_DisplayOrigin;
    bool m_HasDisplayOrigin = false;
    bool m_UnexpectedTermination;
    SdlInputHandler* m_InputHandler;
    int m_MouseEmulationRefCount;
    int m_FlushingWindowEventsRef;
    QStringList m_LaunchWarnings;
    bool m_ShouldExit;
    bool m_LeaveHostAppRunning;
    bool m_PyrowaveRejected;

    bool m_AsyncConnectionSuccess;
    int m_PortTestResults;

    int m_ActiveVideoFormat;
    int m_ActiveVideoWidth;
    int m_ActiveVideoHeight;
    int m_ActiveVideoFrameRate;

    OpusMSDecoder* m_OpusDecoder;
    IAudioRenderer* m_AudioRenderer;
    OPUS_MULTISTREAM_CONFIGURATION m_ActiveAudioConfig;
    OPUS_MULTISTREAM_CONFIGURATION m_OriginalAudioConfig;
    int m_AudioSampleCount;
    Uint32 m_DropAudioEndTime;

    Overlay::OverlayManager m_OverlayManager;
    OverlayMenu m_OverlayMenu { m_OverlayManager };
    // ML_OVERLAY_MENU_TEST opens the menu once the first renderer is up (see exec())
    bool m_OverlayMenuTestPending = qEnvironmentVariableIsSet("ML_OVERLAY_MENU_TEST");
    // The gamepad slot the menu's gamepad actions are for, or -1 for the first one
    int m_OverlayMenuSlot = -1;
    // Whether opening the menu let go of the mouse, which is captured again as it closes
    bool m_OverlayMenuReleasedMouse = false;
    // Whether the left mouse button went down on one of the menu's buttons
    bool m_OverlayMenuPressedButton = false;
    // Quit Session from the menu quits the host app whatever the settings say
    bool m_QuitHostApp = false;
    // Network jitter tolerance picked from the menu for this session, or 0 for the setting
    int m_PacingArrivalPercentileOverride = 0;
    // The custom commands in the menu, read as the stream starts
    QVector<OverlayMenuCommand> m_OverlayMenuCommands;
    // Tells this session's finished commands from those of an earlier one
    int m_OverlayMenuCommandToken = 0;
    // The host's commands, fetched as the stream starts and each time the menu opens
    HostCommandsTarget m_HostCommandsTarget;
    HostCommands m_HostCommands;
    // Cleared once the host answers without commands (it doesn't have the feature)
    bool m_HostCommandsSupported = true;
    bool m_HostCommandsFetching = false;
    // The menu button of the host command running, for its result
    QString m_HostCommandRunningId;
    int m_HostCommandRunningAction = OverlayMenu::ActionNone;
    bool m_HostCommandRunningCloses = false;
    // Counts the host commands run, so a spinner's redraws stop with the run they're for
    int m_HostCommandRuns = 0;
    // A command that keeps the menu open ran, and its spinner turns until the reloaded commands
    // arrive
    bool m_HostCommandAwaitingReload = false;
    // Counts the menu's messages, so only the latest one's timeout clears it
    int m_OverlayMenuMessages = 0;

    static CONNECTION_LISTENER_CALLBACKS k_ConnCallbacks;
    static Session* s_ActiveSession;
    static QSemaphore s_ActiveSessionSemaphore;
};
