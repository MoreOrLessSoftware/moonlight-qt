// MoonlightStream.exe: streams one app without the QML GUI, for the GUI's handoff (see
// StreamProcess) or from the command line, with "stream <host> <app>" as Moonlight.exe takes.

#include <QGuiApplication>

#define SDL_MAIN_HANDLED
#include "SDL_compat.h"

#include "appmain.h"
#include "streaming/streamprocessmain.h"

#include <cstdio>

static int runStream(QGuiApplication& app, GlobalCommandLineParser&,
                     GlobalCommandLineParser::ParseResult parseResult)
{
    if (parseResult != GlobalCommandLineParser::StreamRequested) {
        fprintf(stderr, "MoonlightStream only streams: MoonlightStream stream <host> <app> [options]. "
                        "Use Moonlight for anything else.\n");
        return 1;
    }

    return StreamProcessController::run(app);
}

int main(int argc, char *argv[])
{
    return MoonlightApp::run(argc, argv, "MoonlightStream", runStream);
}
