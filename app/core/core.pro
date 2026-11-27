# Everything Moonlight.exe and MoonlightStream.exe are built from, besides their main()
# and the GUI: the backend, settings, command line and streaming code. Its sources are in
# the app directory, and they mustn't need QtQuick or QtQml.

TEMPLATE = lib
CONFIG += staticlib
TARGET = moonlight-core

include(../common.pri)

# The executables link the libraries
LIBS =

CORE_SOURCES += \
    backend/nvaddress.cpp \
    backend/nvapp.cpp \
    cli/pair.cpp \
    backend/computerseeker.cpp \
    backend/identitymanager.cpp \
    backend/nvcomputer.cpp \
    backend/nvhttp.cpp \
    backend/nvpairingmanager.cpp \
    backend/computermanager.cpp \
    backend/boxartmanager.cpp \
    backend/richpresencemanager.cpp \
    cli/commandlineparser.cpp \
    cli/listapps.cpp \
    cli/quitstream.cpp \
    cli/startstream.cpp \
    settings/compatfetcher.cpp \
    settings/mappingfetcher.cpp \
    settings/streamingpreferences.cpp \
    settings/overlaymenucommands.cpp \
    settings/gamepadsettings.cpp \
    materialsymbols.cpp \
    streaming/input/abstouch.cpp \
    streaming/input/gamepad.cpp \
    streaming/input/gyrofilter.cpp \
    streaming/input/input.cpp \
    streaming/input/keyboard.cpp \
    streaming/input/mouse.cpp \
    streaming/input/reltouch.cpp \
    streaming/session.cpp \
    streaming/streamprocess.cpp \
    streaming/streamprocessmain.cpp \
    streaming/launchwindow.cpp \
    streaming/audio/audio.cpp \
    streaming/audio/renderers/sdlaud.cpp \
    streaming/bandwidth.cpp \
    streaming/streamutils.cpp \
    backend/autoupdatechecker.cpp \
    path.cpp \
    settings/mappingmanager.cpp \
    streaming/video/overlaymanager.cpp \
    streaming/overlaymenu.cpp \
    streaming/sessionoverlaymenu.cpp \
    streaming/hostcommands.cpp \
    backend/systemproperties.cpp \
    wm.cpp \
    appmain.cpp

CORE_HEADERS += \
    SDL_compat.h \
    backend/nvaddress.h \
    backend/nvapp.h \
    cli/pair.h \
    settings/compatfetcher.h \
    settings/mappingfetcher.h \
    utils.h \
    backend/computerseeker.h \
    backend/identitymanager.h \
    backend/nvcomputer.h \
    backend/nvhttp.h \
    backend/nvpairingmanager.h \
    backend/computermanager.h \
    backend/boxartmanager.h \
    backend/richpresencemanager.h \
    cli/commandlineparser.h \
    cli/listapps.h \
    cli/quitstream.h \
    cli/startstream.h \
    settings/streamingpreferences.h \
    settings/overlaymenucommands.h \
    settings/gamepadsettings.h \
    materialsymbols.h \
    streaming/input/input.h \
    streaming/input/gyrofilter.h \
    streaming/session.h \
    streaming/streamprocess.h \
    streaming/streamprocessmain.h \
    streaming/launchwindow.h \
    streaming/audio/renderers/renderer.h \
    streaming/audio/renderers/sdl.h \
    streaming/video/decoder.h \
    streaming/bandwidth.h \
    streaming/streamutils.h \
    backend/autoupdatechecker.h \
    path.h \
    settings/mappingmanager.h \
    streaming/video/overlaymanager.h \
    streaming/overlaymenu.h \
    streaming/hostcommands.h \
    backend/systemproperties.h \
    appmain.h

for(file, CORE_SOURCES): SOURCES += $$PWD/../$$file
for(file, CORE_HEADERS): HEADERS += $$PWD/../$$file
