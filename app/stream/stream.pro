# MoonlightStream.exe: streams one app without the QML GUI (see streammain.cpp). It's built
# next to Moonlight.exe, and only the portable package ships it.

TARGET = MoonlightStream
TEMPLATE = app

include(../corelib.pri)
include(../common.pri)

CONFIG(debug, debug|release): DESTDIR = $$shadowed($$PWD/..)/debug
else: DESTDIR = $$shadowed($$PWD/..)/release

SOURCES += $$PWD/../streammain.cpp

RESOURCES += $$PWD/../resources.qrc

win32 {
    QMAKE_TARGET_DESCRIPTION = Moonlight Game Streaming Client (streaming)
}
