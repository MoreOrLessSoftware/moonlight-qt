TEMPLATE = subdirs
SUBDIRS = \
    moonlight-common-c \
    qmdnsengine \
    appcore \
    app \
    h264bitstream

# Everything but the executables' main() and the GUI, which they link
appcore.subdir = app/core

# Build the dependencies in parallel before the final app
app.depends = appcore qmdnsengine moonlight-common-c h264bitstream
win32:!winrt {
    SUBDIRS += AntiHooking
    app.depends += AntiHooking

    # MoonlightStream.exe, which the portable package runs streams in
    SUBDIRS += appstream
    appstream.subdir = app/stream
    appstream.depends = appcore qmdnsengine moonlight-common-c h264bitstream AntiHooking
}

# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Run our compile tests
load(configure)
qtCompileTest(SL)
qtCompileTest(EGL)
