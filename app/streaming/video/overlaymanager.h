#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    // The in-stream menu (OverlayMenu class): an image drawn by its owner rather than text.
    // Last, so it's drawn over the others.
    OverlayMenu,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;

    // Whether this renderer draws the given overlay. The menu is opt-in, since a menu that's
    // open but not drawn would still take the input.
    virtual bool isOverlaySupported(OverlayType type) {
        return type != OverlayMenu;
    }
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
    // The stats overlay's layout, a StreamingPreferences::PerfOverlayMode other than POM_OFF.
    // It stays as set while the overlay is off, for the next time it's turned on.
    int getDebugOverlayMode();
    void setDebugOverlayMode(int mode);
    // The stats overlay drawn a third of the way down instead of at the top (the renderers)
    bool isOverlayLatencyMeasurementEnabled(OverlayType type);
    SDL_Color getOverlayColor(OverlayType type);
    void setOverlayColor(OverlayType type, SDL_Color color);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    // For an image overlay (OverlayMenu): replaces its image, taking ownership of the surface
    // (SDL_PIXELFORMAT_ARGB8888, straight alpha). It's shown while the overlay is enabled.
    void updateOverlayImage(OverlayType type, SDL_Surface* image);
    // Whether the current renderer draws this overlay type
    bool isOverlaySupported(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);
    // The window text overlays are drawn in, for lines as wide as it to wrap at
    void setOverlayWindow(SDL_Window* window);

private:
    static bool isImageOverlay(OverlayType type);
    void notifyOverlayUpdated(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[1024];

        TTF_Font* font;
        SDL_Surface* surface;
        // An image overlay's current image. The renderer takes (and frees) each surface it's
        // handed, so this is kept to hand a copy to a new renderer.
        SDL_Surface* image;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    SDL_Window* m_Window;
    int m_DebugOverlayMode;
    QByteArray m_FontData;
};

}
