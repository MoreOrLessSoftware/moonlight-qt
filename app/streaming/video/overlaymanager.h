#pragma once

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
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // Flips the latency marker between black and white, if it is enabled
    void flipLatencyMarker();

    // Sets the latency timer to the milliseconds since the marker last flipped. Called just
    // before each frame is drawn, so the number on screen is as of that frame.
    void updateLatencyTimer();

    // Shows how far an automatic latency test has got next to the timer; a total of 0 hides it
    void setLatencyTestProgress(int done, int total);

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
    std::atomic<int> m_LatencyTestDone;
    std::atomic<int> m_LatencyTestTotal;
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
