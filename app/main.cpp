// Moonlight.exe: the GUI, and the command line actions other than streaming in the stream
// process. The startup and shutdown they share with MoonlightStream.exe are in appmain.cpp.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThreadPool>

// Don't let SDL hook our main function, since Qt is already
// doing the same thing. This needs to be before any headers
// that might include SDL.h themselves.
#define SDL_MAIN_HANDLED
#include "SDL_compat.h"

#include "appmain.h"
#include "cli/listapps.h"
#include "cli/quitstream.h"
#include "cli/startstream.h"
#include "cli/pair.h"
#include "cli/commandlineparser.h"
#include "gui/streamhandoff.h"
#include "gui/computermodel.h"
#include "gui/appmodel.h"
#include "gui/overlaymenucommandmodel.h"
#include "gui/keyrecorder.h"
#include "gui/gamepadsettingsmodel.h"
#include "gui/gyrotester.h"
#include "gui/sdlgamepadkeynavigation.h"
#include "backend/autoupdatechecker.h"
#include "backend/computermanager.h"
#include "backend/systemproperties.h"
#include "streaming/session.h"
#include "streaming/streamprocess.h"
#include "streaming/streamprocessmain.h"
#include "settings/streamingpreferences.h"

static int runGui(QGuiApplication& app, GlobalCommandLineParser&,
                  GlobalCommandLineParser::ParseResult commandLineParserResult)
{
    if (commandLineParserResult == GlobalCommandLineParser::StreamRequested) {
        // Streams from the command line go to the stream executable where there is one,
        // as they would from the GUI
        int exitCode;
        if (StreamProcess::forwardStream(app.arguments(), &exitCode)) {
            return exitCode;
        }

#ifdef Q_OS_WIN32
        // How the GUI hands streams off without the stream executable (in a build tree,
        // with MOONLIGHT_STREAM_PROCESS=1)
        if (app.arguments().contains("--stream-process")) {
            return StreamProcessController::run(app);
        }
#endif
    }

    // Register our C++ types for QML
    qmlRegisterType<ComputerModel>("ComputerModel", 1, 0, "ComputerModel");
    qmlRegisterType<AppModel>("AppModel", 1, 0, "AppModel");
    qmlRegisterType<OverlayMenuCommandModel>("OverlayMenuCommandModel", 1, 0, "OverlayMenuCommandModel");
    qmlRegisterType<KeyRecorder>("KeyRecorder", 1, 0, "KeyRecorder");
    qmlRegisterType<GamepadSettingsModel>("GamepadSettingsModel", 1, 0, "GamepadSettingsModel");
    qmlRegisterType<GyroTester>("GyroTester", 1, 0, "GyroTester");
    qmlRegisterUncreatableType<Session>("Session", 1, 0, "Session", "Session cannot be created from QML");
    qmlRegisterSingletonType<ComputerManager>("ComputerManager", 1, 0,
                                              "ComputerManager",
                                              [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                  return new ComputerManager(StreamingPreferences::get(qmlEngine));
                                              });
    qmlRegisterSingletonType<AutoUpdateChecker>("AutoUpdateChecker", 1, 0,
                                                "AutoUpdateChecker",
                                                [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                    return new AutoUpdateChecker();
                                                });
    qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0,
                                               "SystemProperties",
                                               [](QQmlEngine*, QJSEngine*) -> QObject* {
                                                   return new SystemProperties();
                                               });
    qmlRegisterSingletonType<SdlGamepadKeyNavigation>("SdlGamepadKeyNavigation", 1, 0,
                                                      "SdlGamepadKeyNavigation",
                                                      [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                          return new SdlGamepadKeyNavigation(StreamingPreferences::get(qmlEngine));
                                                      });
    // Only a normal start hands streams to the stream process, not a command line stream
    bool allowStreamHandoff = commandLineParserResult == GlobalCommandLineParser::NormalStartRequested;
    qmlRegisterSingletonType<StreamHandoff>("StreamHandoff", 1, 0,
                                            "StreamHandoff",
                                            [allowStreamHandoff](QQmlEngine*, QJSEngine*) -> QObject* {
                                                return new StreamHandoff(allowStreamHandoff);
                                            });
    qmlRegisterSingletonType<StreamingPreferences>("StreamingPreferences", 1, 0,
                                                   "StreamingPreferences",
                                                   [](QQmlEngine* qmlEngine, QJSEngine*) -> QObject* {
                                                       return StreamingPreferences::get(qmlEngine);
                                                   });

    // We require the Material theme
    QQuickStyle::setStyle("Material");

    // Our icons are styled for a dark theme, so we do not allow the user to override this
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");

    // These are defaults that we allow the user to override
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_ACCENT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "Purple");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_VARIANT")) {
        qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    }
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_MATERIAL_PRIMARY")) {
        // Qt 6.9 began to use a different shade of Material.Indigo when we use a dark theme
        // (which is all the time). The new color looks washed out, so manually specify the
        // old primary color unless the user overrides it themselves.
        qputenv("QT_QUICK_CONTROLS_MATERIAL_PRIMARY", "#3F51B5");
    }

    QQmlApplicationEngine engine;
    QString initialView;
    bool hasGUI = true;

    switch (commandLineParserResult) {
    case GlobalCommandLineParser::NormalStartRequested:
        initialView = "qrc:/gui/PcView.qml";
        break;
    case GlobalCommandLineParser::StreamRequested:
        {
            initialView = "qrc:/gui/CliStartStreamSegue.qml";
            StreamingPreferences* preferences = StreamingPreferences::get();
            StreamCommandLineParser streamParser;
            streamParser.parse(app.arguments(), preferences);
            QString host    = streamParser.getHost();
            QString appName = streamParser.getAppName();
            auto launcher   = new CliStartStream::Launcher(host, appName, preferences, &app);
            launcher->setAppId(streamParser.getAppId());
            QPoint displayOrigin;
            if (streamParser.getDisplayOrigin(&displayOrigin)) {
                launcher->setDisplayOrigin(displayOrigin);
            }
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::QuitRequested:
        {
            initialView = "qrc:/gui/CliQuitStreamSegue.qml";
            QuitCommandLineParser quitParser;
            quitParser.parse(app.arguments());
            auto launcher = new CliQuitStream::Launcher(quitParser.getHost(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::PairRequested:
        {
            initialView = "qrc:/gui/CliPair.qml";
            PairCommandLineParser pairParser;
            pairParser.parse(app.arguments());
            auto launcher = new CliPair::Launcher(pairParser.getHost(), pairParser.getPredefinedPin(), &app);
            engine.rootContext()->setContextProperty("launcher", launcher);
            break;
        }
    case GlobalCommandLineParser::ListRequested:
        {
            ListCommandLineParser listParser;
            listParser.parse(app.arguments());
            auto launcher = new CliListApps::Launcher(listParser.getHost(), listParser, &app);
            launcher->execute(new ComputerManager(StreamingPreferences::get()));
            hasGUI = false;
            break;
        }
    }

    if (hasGUI) {
        engine.rootContext()->setContextProperty("initialView", initialView);
        engine.rootContext()->setContextProperty("runConfigChecks", commandLineParserResult == GlobalCommandLineParser::NormalStartRequested);

        // Load the main.qml file
        engine.load(QUrl(QStringLiteral("qrc:/gui/main.qml")));
        if (engine.rootObjects().isEmpty())
            return -1;
    }

    int err = app.exec();

    // Remember where the window was and how big it was for next time. Only a plain
    // window counts: a maximized or full-screen window isn't the size it'll come back
    // as (that's the UI display mode setting's job), and a minimized one isn't anywhere.
    if (hasGUI && commandLineParserResult == GlobalCommandLineParser::NormalStartRequested) {
        QQuickWindow* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
        if (window != nullptr && window->windowStates() == Qt::WindowNoState) {
            StreamingPreferences::saveUiWindowGeometry(window->geometry());
        }
    }

    return err;
}

int main(int argc, char *argv[])
{
    return MoonlightApp::run(argc, argv, "Moonlight", runGui);
}
