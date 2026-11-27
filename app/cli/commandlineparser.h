#pragma once

#include "settings/streamingpreferences.h"

#include <QMap>
#include <QPoint>
#include <QString>

class GlobalCommandLineParser
{
public:
    enum ParseResult {
        NormalStartRequested,
        StreamRequested,
        QuitRequested,
        PairRequested,
        ListRequested,
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);
};

class QuitCommandLineParser
{
public:
    QuitCommandLineParser();
    virtual ~QuitCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;

private:
    QString m_Host;
};

class PairCommandLineParser
{
public:
    PairCommandLineParser();
    virtual ~PairCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    QString getPredefinedPin() const;

private:
    QString m_Host;
    QString m_PredefinedPin;
};

class StreamCommandLineParser
{
public:
    StreamCommandLineParser();
    virtual ~StreamCommandLineParser();

    void parse(const QStringList &args, StreamingPreferences *preferences);

    QString getHost() const;
    QString getAppName() const;

    // Set when the GUI hands the stream off: the app's ID (0 to find it by name), the GUI's
    // process (which waits for us, 0 if none) and the display the GUI was on
    int getAppId() const;
    qint64 getGuiPid() const;

    // Whether to stream without the QML GUI, as the stream process does
    bool isStreamProcess() const;
    bool getDisplayOrigin(QPoint* origin) const;

private:
    QString m_Host;
    QString m_AppName;
    int m_AppId = 0;
    qint64 m_GuiPid = 0;
    bool m_StreamProcess = false;
    QPoint m_DisplayOrigin;
    bool m_HasDisplayOrigin = false;
    QMap<QString, StreamingPreferences::WindowMode> m_WindowModeMap;
    QMap<QString, StreamingPreferences::AudioConfig> m_AudioConfigMap;
    QMap<QString, StreamingPreferences::VideoCodecConfig> m_VideoCodecMap;
    QMap<QString, StreamingPreferences::VideoDecoderSelection> m_VideoDecoderMap;
    QMap<QString, StreamingPreferences::CaptureSysKeysMode> m_CaptureSysKeysModeMap;
};

class ListCommandLineParser
{
public:
    ListCommandLineParser();
    virtual ~ListCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    bool isPrintCSV() const;
    bool isVerbose() const;

private:
    QString m_Host;
    bool m_PrintCSV;
    bool m_Verbose;
};
