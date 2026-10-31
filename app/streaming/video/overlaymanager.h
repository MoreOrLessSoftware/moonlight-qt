#pragma once

#include <QFile>
#include <QMutex>
#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

#include <atomic>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayLatencyMarker,
    OverlayLatencyTimer,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    void setOverlayColor(OverlayType type, SDL_Color color);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // Flips the latency marker between black and white, if it is enabled. testPress is the
    // number of the automatic latency test's press that flipped it, or 0 for any other press.
    void flipLatencyMarker(int testPress = 0);

    // Starts a new file for logLatencyTestPress(), as an automatic latency test starts
    void startLatencyTestLog();

    // Sets the latency timer to the milliseconds since the marker last flipped. Called just
    // before each frame is drawn, so the number on screen is as of that frame.
    void updateLatencyTimer();

    // Shows how far an automatic latency test has got next to the timer; a total of 0 hides it
    void setLatencyTestProgress(int done, int total);

    // How many times the latency marker has flipped since it was turned on, counting only
    // flips renderers already draw, and when the latest of them happened and which test
    // press made it (0 for none). 0 while the marker is off. Called just before each frame is
    // drawn, so the frame shows that flip.
    uint32_t getLatencyMarkerFlip(uint64_t* flipUs, int* testPress);

    // Records how long the marker flip of an automatic latency test's press took to be
    // presented, in a CSV of its own next to the log: Moonlight-latency-test-<time>.csv, a
    // new one for each test. drawMs is how long until drawing its frame started.
    void logLatencyTestPress(int testPress, uint64_t flipUs, double presentMs, double drawMs);

    // Where the latency marker goes on a display of the given size, in pixels from the top left.
    // Its rows match the trigger patch of Sunshine's latency-test app, so a camera sees both
    // scanned out at the same time, and it sits a quarter of the way across, clear of the app's
    // other patches.
    static SDL_Rect getLatencyMarkerRect(int displayWidth, int displayHeight);

    // Where the top left of the latency timer goes: just below the marker
    static SDL_Point getLatencyTimerPosition(int displayWidth, int displayHeight);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    bool m_LatencyMarkerLit;
    std::atomic<uint64_t> m_LatencyMarkerFlipUs;
    std::atomic<uint32_t> m_LatencyMarkerFlips;
    std::atomic<uint64_t> m_LatencyMarkerDrawnFlipUs;
    std::atomic<int> m_LatencyMarkerDrawnTestPress;
    QFile* m_LatencyMarkerFile;
    QMutex m_LatencyMarkerFileLock;
    std::atomic<int> m_LatencyTestDone;
    std::atomic<int> m_LatencyTestTotal;
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
