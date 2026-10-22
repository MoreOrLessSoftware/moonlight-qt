#include "overlaymanager.h"
#include "path.h"

#include <Limelight.h>

using namespace Overlay;

OverlayManager::OverlayManager() :
    m_LatencyMarkerLit(false),
    m_LatencyMarkerFlipUs(0),
    m_LatencyTestDone(0),
    m_LatencyTestTotal(0),
    m_Renderer(nullptr),
    m_FontData(Path::readDataFile("ModeSeven.ttf"))
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    m_Overlays[OverlayType::OverlayDebug].color = {0xD0, 0xD0, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xCC, 0x00, 0x00, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    m_Overlays[OverlayType::OverlayLatencyTimer].color = {0xFF, 0xFF, 0xFF, 0xFF};
    m_Overlays[OverlayType::OverlayLatencyTimer].fontSize = 48;

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

        // The latency marker starts black, like the latency-test app's trigger patch
        if (type == OverlayType::OverlayLatencyMarker) {
            m_LatencyMarkerLit = false;
            m_LatencyMarkerFlipUs = 0;
        }

        notifyOverlayUpdated(type);

        // The timer comes and goes with the marker
        if (type == OverlayType::OverlayLatencyMarker) {
            setOverlayState(OverlayType::OverlayLatencyTimer, enabled);
        }
    }
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

void OverlayManager::flipLatencyMarker()
{
    if (m_Overlays[OverlayType::OverlayLatencyMarker].enabled) {
        m_LatencyMarkerFlipUs = LiGetMicroseconds();
        m_LatencyMarkerLit = !m_LatencyMarkerLit;
        notifyOverlayUpdated(OverlayType::OverlayLatencyMarker);
    }
}

void OverlayManager::updateLatencyTimer()
{
    if (!m_Overlays[OverlayType::OverlayLatencyTimer].enabled) {
        return;
    }

    char text[32];
    uint64_t flipUs = m_LatencyMarkerFlipUs;
    if (flipUs == 0) {
        SDL_strlcpy(text, "----.- ms", sizeof(text));
    }
    else {
        double sinceMs = (double)(LiGetMicroseconds() - flipUs) / 1000.0;
        SDL_snprintf(text, sizeof(text), "%6.1f ms", SDL_min(sinceMs, 9999.9));
    }

    int total = m_LatencyTestTotal;
    if (total > 0) {
        size_t length = SDL_strlen(text);
        SDL_snprintf(text + length, sizeof(text) - length, "  %d/%d", (int)m_LatencyTestDone, total);
    }

    // Only redraw the text when the number shown changes
    if (SDL_strcmp(text, m_Overlays[OverlayType::OverlayLatencyTimer].text) != 0) {
        updateOverlayText(OverlayType::OverlayLatencyTimer, text);
    }
}

void OverlayManager::setLatencyTestProgress(int done, int total)
{
    m_LatencyTestDone = done;
    m_LatencyTestTotal = total;
}

SDL_Point OverlayManager::getLatencyTimerPosition(int displayWidth, int displayHeight)
{
    SDL_Rect marker = getLatencyMarkerRect(displayWidth, displayHeight);

    SDL_Point point;
    point.x = marker.x;
    point.y = marker.y + marker.h + marker.h / 8;
    return point;
}

SDL_Rect OverlayManager::getLatencyMarkerRect(int displayWidth, int displayHeight)
{
    // Same sizing as compute_layout() in Sunshine's tools/latency_test/pattern.h: cells are an
    // eighth of the short side, on a 64 px grid (16 px on small screens), and the trigger patch
    // is two cells square, centered on the grid.
    int rawCell = SDL_min(displayWidth, displayHeight) / 8;
    int grid = rawCell >= 64 ? 64 : 16;
    int cellAlign = SDL_max(grid, 32);
    int cell = SDL_max(rawCell - rawCell % cellAlign, 32);

    int x = displayWidth / 4 - cell;
    int y = displayHeight / 2 - cell;

    SDL_Rect rect;
    rect.x = SDL_max(x - x % grid, 0);
    rect.y = SDL_max(y - y % grid, 0);
    rect.w = 2 * cell;
    rect.h = 2 * cell;
    return rect;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // The latency marker is a solid square, not text. Renderers stretch it to
    // getLatencyMarkerRect(), so a small surface is enough.
    if (type == OverlayType::OverlayLatencyMarker) {
        SDL_Surface* newSurface = nullptr;
        if (m_Overlays[type].enabled) {
            newSurface = SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
            if (newSurface != nullptr) {
                Uint8 level = m_LatencyMarkerLit ? 0xFF : 0x00;
                SDL_FillRect(newSurface, nullptr, SDL_MapRGBA(newSurface->format, level, level, level, 0xFF));
            }
        }

        SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, newSurface);
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

    SDL_Surface* newSurface = nullptr;
    if (m_Overlays[type].enabled) {
        // The _Wrapped variant is required for line breaks to work
        newSurface = TTF_RenderText_Blended_Wrapped(m_Overlays[type].font,
                                                    m_Overlays[type].text,
                                                    m_Overlays[type].color,
                                                    1024);

        // The latency timer sits on the noisy latency-test pattern, so give it an opaque
        // black background to keep it readable on camera
        if (newSurface != nullptr && type == OverlayType::OverlayLatencyTimer) {
            SDL_Surface* backed = SDL_CreateRGBSurfaceWithFormat(0, newSurface->w, newSurface->h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (backed != nullptr) {
                SDL_FillRect(backed, nullptr, SDL_MapRGBA(backed->format, 0, 0, 0, 0xFF));
                SDL_BlitSurface(newSurface, nullptr, backed, nullptr);
                SDL_FreeSurface(newSurface);
                newSurface = backed;
            }
        }
    }

    // Exchange the old surface with the new one
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, newSurface);

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}
