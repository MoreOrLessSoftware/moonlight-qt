#pragma once

#include "cli/commandlineparser.h"

#include <QString>

#include <functional>

class QGuiApplication;

// The startup and shutdown that Moonlight.exe and MoonlightStream.exe share: logging, the
// SDL and Qt setup, QGuiApplication and the command line, around what each of them runs
namespace MoonlightApp
{
    // Runs the app with the QGuiApplication and the parsed command line, returning its exit code
    using Runner = std::function<int(QGuiApplication& app, GlobalCommandLineParser& parser,
                                     GlobalCommandLineParser::ParseResult parseResult)>;

    // Log files are named <logPrefix>-<time>.log, and the oldest of them pruned
    int run(int argc, char* argv[], const QString& logPrefix, const Runner& runner);
}
