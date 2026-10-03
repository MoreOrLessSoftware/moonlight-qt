#include "streamingpreferences.h"
#include "utils.h"

#include <QSettings>
#include <QTranslator>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QtMath>
#include <QList>

#include <QtDebug>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_PACINGTEARPCT "pacingtearpct"
#define SER_PACINGSMOOTHING "pacingsmoothing"
#define SER_PACINGPERCENTILE "pacingpercentile"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_RICHPRESENCE "richpresence"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_DETECTNETBLOCKING "detectnetblocking"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_LANGUAGE "language"
#define SER_RENDERER "renderer"
#define SER_VIDEOPRESETS "videopresets"
#define SER_PRESET_NAME "name"

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine)
    : m_QmlEngine(qmlEngine)
{
    reload();
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::reload()
{
    QSettings settings;

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#elif defined(Q_OS_WIN32)
    // The D3D11 renderer presents through a windowed flip-model swapchain either way,
    // so borderless gets the same path to the display without changing display modes.
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless we have a slow GPU (which can take advantage of wp_viewporter
    // to reduce GPU load with lower resolution video streams).
    if (WMUtils::isRunningWayland() && !WMUtils::isGpuSlow()) {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
    }
    else {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN;
    }
#endif

    width = settings.value(SER_WIDTH, 1280).toInt();
    height = settings.value(SER_HEIGHT, 720).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    bitrateKbps = settings.value(SER_BITRATE, getDefaultBitrate(width, height, fps, enableYUV444)).toInt();
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    pacingTearPercent = settings.value(SER_PACINGTEARPCT, 0).toInt();
    pacingSmoothingPercent = settings.value(SER_PACINGSMOOTHING, 50).toInt();
    pacingArrivalPercentile = settings.value(SER_PACINGPERCENTILE, 97).toInt();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    richPresence = settings.value(SER_RICHPRESENCE, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    detectNetworkBlocking = settings.value(SER_DETECTNETBLOCKING, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    enableHdr = settings.value(SER_HDR, false).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    rendererSelection = static_cast<RendererSelection>(settings.value(SER_RENDERER,
                                                  static_cast<int>(RendererSelection::RS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        // Try to load from the old preference value too
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());


    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }
}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::save()
{
    QSettings settings;

    settings.setValue(SER_WIDTH, width);
    settings.setValue(SER_HEIGHT, height);
    settings.setValue(SER_FPS, fps);
    settings.setValue(SER_BITRATE, bitrateKbps);
    settings.setValue(SER_UNLOCK_BITRATE, unlockBitrate);
    settings.setValue(SER_AUTOADJUSTBITRATE, autoAdjustBitrate);
    settings.setValue(SER_VSYNC, enableVsync);
    settings.setValue(SER_GAMEOPTS, gameOptimizations);
    settings.setValue(SER_HOSTAUDIO, playAudioOnHost);
    settings.setValue(SER_MULTICONT, multiController);
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_QUITAPPAFTER, quitAppAfter);
    settings.setValue(SER_ABSMOUSEMODE, absoluteMouseMode);
    settings.setValue(SER_ABSTOUCHMODE, absoluteTouchMode);
    settings.setValue(SER_FRAMEPACING, framePacing);
    settings.setValue(SER_PACINGTEARPCT, pacingTearPercent);
    settings.setValue(SER_PACINGSMOOTHING, pacingSmoothingPercent);
    settings.setValue(SER_PACINGPERCENTILE, pacingArrivalPercentile);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_RICHPRESENCE, richPresence);
    settings.setValue(SER_GAMEPADMOUSE, gamepadMouse);
    settings.setValue(SER_PACKETSIZE, packetSize);
    settings.setValue(SER_DETECTNETBLOCKING, detectNetworkBlocking);
    settings.setValue(SER_SHOWPERFOVERLAY, showPerformanceOverlay);
    settings.setValue(SER_AUDIOCFG, static_cast<int>(audioConfig));
    settings.setValue(SER_HDR, enableHdr);
    settings.setValue(SER_YUV444, enableYUV444);
    settings.setValue(SER_VIDEOCFG, static_cast<int>(videoCodecConfig));
    settings.setValue(SER_VIDEODEC, static_cast<int>(videoDecoderSelection));
    settings.setValue(SER_RENDERER, static_cast<int>(rendererSelection));
    settings.setValue(SER_WINDOWMODE, static_cast<int>(windowMode));
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_SWAPMOUSEBUTTONS, swapMouseButtons);
    settings.setValue(SER_MUTEONFOCUSLOSS, muteOnFocusLoss);
    settings.setValue(SER_BACKGROUNDGAMEPAD, backgroundGamepad);
    settings.setValue(SER_REVERSESCROLL, reverseScrollDirection);
    settings.setValue(SER_SWAPFACEBUTTONS, swapFaceButtons);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444)
{
    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}

namespace {

struct VideoPreset
{
    QString name;
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool unlockBitrate;
    bool autoAdjustBitrate;
    bool enableVsync;
    bool framePacing;
    int videoCodecConfig;
    bool enableHdr;
    bool enableYUV444;
    int windowMode;

    // autoAdjustBitrate is deliberately not compared. It only decides whether the
    // bitrate follows the resolution and frame rate later, not what is streamed.
    bool sameSettingsAs(const VideoPreset& other) const
    {
        return width == other.width && height == other.height && fps == other.fps &&
               bitrateKbps == other.bitrateKbps && unlockBitrate == other.unlockBitrate &&
               enableVsync == other.enableVsync && framePacing == other.framePacing &&
               videoCodecConfig == other.videoCodecConfig && enableHdr == other.enableHdr &&
               enableYUV444 == other.enableYUV444 && windowMode == other.windowMode;
    }
};

QList<VideoPreset> loadVideoPresets()
{
    QList<VideoPreset> presets;
    QSettings settings;

    int count = settings.beginReadArray(SER_VIDEOPRESETS);
    for (int i = 0; i < count; i++) {
        settings.setArrayIndex(i);

        VideoPreset preset;
        preset.name = settings.value(SER_PRESET_NAME).toString();
        if (preset.name.isEmpty()) {
            continue;
        }

        preset.width = settings.value(SER_WIDTH, 1280).toInt();
        preset.height = settings.value(SER_HEIGHT, 720).toInt();
        preset.fps = settings.value(SER_FPS, 60).toInt();
        preset.bitrateKbps = settings.value(SER_BITRATE, 10000).toInt();
        preset.unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
        preset.autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
        preset.enableVsync = settings.value(SER_VSYNC, true).toBool();
        preset.framePacing = settings.value(SER_FRAMEPACING, false).toBool();
        preset.videoCodecConfig = settings.value(SER_VIDEOCFG, 0).toInt();
        preset.enableHdr = settings.value(SER_HDR, false).toBool();
        preset.enableYUV444 = settings.value(SER_YUV444, false).toBool();
        preset.windowMode = settings.value(SER_WINDOWMODE, 0).toInt();
        presets.append(preset);
    }
    settings.endArray();

    return presets;
}

void storeVideoPresets(const QList<VideoPreset>& presets)
{
    QSettings settings;

    // Drop the old array first so a shorter list doesn't leave stale entries behind
    settings.remove(SER_VIDEOPRESETS);

    settings.beginWriteArray(SER_VIDEOPRESETS, presets.size());
    for (int i = 0; i < presets.size(); i++) {
        const VideoPreset& preset = presets[i];

        settings.setArrayIndex(i);
        settings.setValue(SER_PRESET_NAME, preset.name);
        settings.setValue(SER_WIDTH, preset.width);
        settings.setValue(SER_HEIGHT, preset.height);
        settings.setValue(SER_FPS, preset.fps);
        settings.setValue(SER_BITRATE, preset.bitrateKbps);
        settings.setValue(SER_UNLOCK_BITRATE, preset.unlockBitrate);
        settings.setValue(SER_AUTOADJUSTBITRATE, preset.autoAdjustBitrate);
        settings.setValue(SER_VSYNC, preset.enableVsync);
        settings.setValue(SER_FRAMEPACING, preset.framePacing);
        settings.setValue(SER_VIDEOCFG, preset.videoCodecConfig);
        settings.setValue(SER_HDR, preset.enableHdr);
        settings.setValue(SER_YUV444, preset.enableYUV444);
        settings.setValue(SER_WINDOWMODE, preset.windowMode);
    }
    settings.endArray();
}

int indexOfVideoPreset(const QList<VideoPreset>& presets, const QString& name)
{
    for (int i = 0; i < presets.size(); i++) {
        if (presets[i].name == name) {
            return i;
        }
    }
    return -1;
}

}

QStringList StreamingPreferences::videoPresetNames()
{
    QStringList names;
    for (const VideoPreset& preset : loadVideoPresets()) {
        names.append(preset.name);
    }
    return names;
}

QString StreamingPreferences::suggestedVideoPresetName()
{
    QStringList parts;

    if (width == 1280 && height == 720) {
        parts.append("720p");
    }
    else if (width == 1920 && height == 1080) {
        parts.append("1080p");
    }
    else if (width == 2560 && height == 1440) {
        parts.append("1440p");
    }
    else if (width == 3840 && height == 2160) {
        parts.append("4K");
    }
    else {
        parts.append(QString("%1x%2").arg(width).arg(height));
    }
    parts.last() += QString(" @ %1 FPS").arg(fps);

    switch (videoCodecConfig) {
    case VCC_FORCE_H264:
        parts.append("H.264");
        break;
    case VCC_FORCE_HEVC:
    case VCC_FORCE_HEVC_HDR_DEPRECATED:
        parts.append("HEVC");
        break;
    case VCC_FORCE_AV1:
        parts.append("AV1");
        break;
    case VCC_FORCE_PYROWAVE:
        parts.append("PyroWave");
        break;
    case VCC_AUTO:
    default:
        parts.append("Auto codec");
        break;
    }

    // Drops the decimal for whole Mbps values, like "200 Mbps" rather than "200.0 Mbps"
    parts.append(QString("%1 Mbps").arg(bitrateKbps / 1000.0, 0, 'f', bitrateKbps % 1000 == 0 ? 0 : 1));

    if (framePacing && enableVsync) {
        parts.append("Frame pacing");
    }
    if (enableHdr) {
        parts.append("HDR");
    }
    if (enableYUV444) {
        parts.append("4:4:4");
    }

    return parts.join(" / ");
}

QString StreamingPreferences::matchingVideoPreset()
{
    VideoPreset current;
    current.width = width;
    current.height = height;
    current.fps = fps;
    current.bitrateKbps = bitrateKbps;
    current.unlockBitrate = unlockBitrate;
    current.enableVsync = enableVsync;
    current.framePacing = framePacing;
    current.videoCodecConfig = videoCodecConfig;
    current.enableHdr = enableHdr;
    current.enableYUV444 = enableYUV444;
    current.windowMode = windowMode;

    for (const VideoPreset& preset : loadVideoPresets()) {
        if (preset.sameSettingsAs(current)) {
            return preset.name;
        }
    }

    return QString();
}

bool StreamingPreferences::saveVideoPreset(const QString& name)
{
    VideoPreset preset;
    preset.name = name.trimmed();
    if (preset.name.isEmpty()) {
        return false;
    }

    preset.width = width;
    preset.height = height;
    preset.fps = fps;
    preset.bitrateKbps = bitrateKbps;
    preset.unlockBitrate = unlockBitrate;
    preset.autoAdjustBitrate = autoAdjustBitrate;
    preset.enableVsync = enableVsync;
    preset.framePacing = framePacing;
    preset.videoCodecConfig = videoCodecConfig;
    preset.enableHdr = enableHdr;
    preset.enableYUV444 = enableYUV444;
    preset.windowMode = windowMode;

    QList<VideoPreset> presets = loadVideoPresets();
    int index = indexOfVideoPreset(presets, preset.name);
    if (index >= 0) {
        // Saving over an existing name replaces it in place
        presets[index] = preset;
    }
    else {
        presets.append(preset);
    }

    storeVideoPresets(presets);
    emit videoPresetsChanged();
    return true;
}

bool StreamingPreferences::applyVideoPreset(const QString& name)
{
    QList<VideoPreset> presets = loadVideoPresets();
    int index = indexOfVideoPreset(presets, name);
    if (index < 0) {
        return false;
    }

    const VideoPreset& preset = presets[index];

    width = preset.width;
    height = preset.height;
    fps = preset.fps;
    bitrateKbps = preset.bitrateKbps;
    unlockBitrate = preset.unlockBitrate;
    autoAdjustBitrate = preset.autoAdjustBitrate;
    enableVsync = preset.enableVsync;
    framePacing = preset.framePacing;
    videoCodecConfig = static_cast<VideoCodecConfig>(preset.videoCodecConfig);
    enableHdr = preset.enableHdr;
    enableYUV444 = preset.enableYUV444;
    windowMode = static_cast<WindowMode>(preset.windowMode);

    // Notify only after every field is set, so bindings never see a half-applied preset
    emit displayModeChanged();
    emit unlockBitrateChanged();
    emit autoAdjustBitrateChanged();
    emit bitrateChanged();
    emit enableVsyncChanged();
    emit framePacingChanged();
    emit videoCodecConfigChanged();
    emit enableHdrChanged();
    emit enableYUV444Changed();
    emit windowModeChanged();
    return true;
}

bool StreamingPreferences::deleteVideoPreset(const QString& name)
{
    QList<VideoPreset> presets = loadVideoPresets();
    int index = indexOfVideoPreset(presets, name);
    if (index < 0) {
        return false;
    }

    presets.removeAt(index);
    storeVideoPresets(presets);
    emit videoPresetsChanged();
    return true;
}

bool StreamingPreferences::renameVideoPreset(const QString& oldName, const QString& newName)
{
    QString trimmedName = newName.trimmed();
    if (trimmedName.isEmpty()) {
        return false;
    }

    QList<VideoPreset> presets = loadVideoPresets();
    int index = indexOfVideoPreset(presets, oldName);
    if (index < 0) {
        return false;
    }

    // Refuse to clobber a different preset that already has this name
    int existing = indexOfVideoPreset(presets, trimmedName);
    if (existing >= 0 && existing != index) {
        return false;
    }

    presets[index].name = trimmedName;
    storeVideoPresets(presets);
    emit videoPresetsChanged();
    return true;
}

bool StreamingPreferences::moveVideoPreset(const QString& name, int offset)
{
    QList<VideoPreset> presets = loadVideoPresets();
    int index = indexOfVideoPreset(presets, name);
    if (index < 0) {
        return false;
    }

    // Stop at either end of the list rather than wrapping around
    int newIndex = qBound(0, index + offset, presets.size() - 1);
    if (newIndex == index) {
        return false;
    }

    presets.move(index, newIndex);
    storeVideoPresets(presets);
    emit videoPresetsChanged();
    return true;
}
