#include "overlaymanager.h"
#include "path.h"
#include "settings/streamingpreferences.h"

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_Window(nullptr),
    m_DebugOverlayMode(StreamingPreferences::POM_DETAILED),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].image != nullptr) {
            SDL_FreeSurface(m_Overlays[i].image);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    SDL_utf8strlcpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

int OverlayManager::getDebugOverlayMode()
{
    return m_DebugOverlayMode;
}

void OverlayManager::setDebugOverlayMode(int mode)
{
    if (mode <= StreamingPreferences::POM_OFF || mode > StreamingPreferences::POM_DETAILED_OFFSET) {
        mode = StreamingPreferences::POM_DETAILED;
    }

    if (m_DebugOverlayMode != mode) {
        m_DebugOverlayMode = mode;

        // The old layout's text is dropped rather than drawn in the new place. The stats
        // are written in the new layout with the next stats window, within a second.
        m_Overlays[OverlayDebug].text[0] = 0;
        setOverlayTextUpdated(OverlayDebug);
    }
}

bool OverlayManager::isOverlayLatencyMeasurementEnabled(OverlayType type)
{
    return type == OverlayDebug && m_DebugOverlayMode == StreamingPreferences::POM_DETAILED_OFFSET;
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayColor(OverlayType type, SDL_Color color)
{
    m_Overlays[type].color = color;
    setOverlayTextUpdated(type);
}

void OverlayManager::updateOverlayImage(OverlayType type, SDL_Surface* image)
{
    SDL_assert(isImageOverlay(type));

    if (m_Overlays[type].image != nullptr) {
        SDL_FreeSurface(m_Overlays[type].image);
    }
    m_Overlays[type].image = image;

    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

bool OverlayManager::isOverlaySupported(OverlayType type)
{
    return m_Renderer != nullptr && m_Renderer->isOverlaySupported(type);
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;

    // A new renderer (the first one, or after a full-screen toggle) starts without any
    // overlays. The stats are updated often enough to show again on their own, but the
    // status text (like the launch warnings, set before there's a renderer) and the menu's
    // image may not be updated again.
    if (m_Renderer != nullptr) {
        for (int i = 0; i < OverlayType::OverlayMax; i++) {
            if (m_Overlays[i].enabled) {
                notifyOverlayUpdated((OverlayType)i);
            }
        }
    }
}

void OverlayManager::setOverlayWindow(SDL_Window* window)
{
    m_Window = window;
}

bool OverlayManager::isImageOverlay(OverlayType type)
{
    return type == OverlayMenu;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    if (isImageOverlay(type)) {
        if (!m_Renderer->isOverlaySupported(type)) {
            return;
        }

        // The renderer frees the surface it takes, so it's handed a copy of the image
        SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
            (void**)&m_Overlays[type].surface,
            m_Overlays[type].enabled && m_Overlays[type].image != nullptr ?
                SDL_DuplicateSurface(m_Overlays[type].image) : nullptr);

        m_Renderer->notifyOverlayUpdated(type);

        if (oldSurface != nullptr) {
            SDL_FreeSurface(oldSurface);
        }
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    // Lines wrap at the window's width, in the pixels the renderers draw overlays in, so the
    // compact stats can run along the top in one line where they fit. The wrap length must be
    // more than 0 for line breaks to work with SDL_ttf before 2.0.18.
    int wrapLength = 1024;
    if (m_Window != nullptr) {
        int windowWidth, windowHeight;
#if SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(m_Window, &windowWidth, &windowHeight);
#else
        SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
#endif
        if (windowWidth > 0) {
            wrapLength = windowWidth;
        }
    }

    // Exchange the old surface with the new one
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
        (void**)&m_Overlays[type].surface,
        m_Overlays[type].enabled ?
            // The _Wrapped variant is required for line breaks to work
            TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                           m_Overlays[type].text,
                                           m_Overlays[type].color,
                                           wrapLength)
            : nullptr);

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}
