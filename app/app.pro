QT += quick quickcontrols2

unix:!macx {
    TARGET = moonlight
} else {
    # On macOS, this is the name displayed in the global menu bar
    TARGET = Moonlight
}

TEMPLATE = app

# The PyroWave library is copied next to us when there is one
CONFIG += moonlight_gui

include(corelib.pri)
include(common.pri)

# Precompile QML files to avoid writing qmlcache on portable versions.
# Since this binds the app against the Qt runtime version, we will only
# do this for Windows and Mac (when disable-prebuilts is not defined),
# since they always ship with the matching build of the Qt runtime.
!disable-prebuilts {
    win32|macx {
        CONFIG(release, debug|release) {
            CONFIG += qtquickcompiler
        }
    }
}

SOURCES += \
    main.cpp \
    gui/overlaymenucommandmodel.cpp \
    gui/keyrecorder.cpp \
    gui/gamepadsettingsmodel.cpp \
    gui/gyrotester.cpp \
    gui/streamhandoff.cpp \
    gui/computermodel.cpp \
    gui/appmodel.cpp \
    gui/sdlgamepadkeynavigation.cpp

HEADERS += \
    gui/overlaymenucommandmodel.h \
    gui/keyrecorder.h \
    gui/gamepadsettingsmodel.h \
    gui/gyrotester.h \
    gui/streamhandoff.h \
    gui/computermodel.h \
    gui/appmodel.h \
    gui/sdlgamepadkeynavigation.h

# The DRM master hooks replace libc functions, so they can't be in the library
SOURCES += $$MASTERHOOK_SOURCES

win32 {
    QMAKE_TARGET_DESCRIPTION = Moonlight Game Streaming Client
}

RESOURCES += \
    resources.qrc \
    qml.qrc

TRANSLATIONS += \
    languages/qml_zh_CN.ts \
    languages/qml_de.ts \
    languages/qml_fr.ts \
    languages/qml_nb_NO.ts \
    languages/qml_ru.ts \
    languages/qml_es.ts \
    languages/qml_ja.ts \
    languages/qml_vi.ts \
    languages/qml_th.ts \
    languages/qml_ko.ts \
    languages/qml_hu.ts \
    languages/qml_nl.ts \
    languages/qml_sv.ts \
    languages/qml_tr.ts \
    languages/qml_uk.ts \
    languages/qml_zh_TW.ts \
    languages/qml_el.ts \
    languages/qml_hi.ts \
    languages/qml_it.ts \
    languages/qml_pt.ts \
    languages/qml_pt_BR.ts \
    languages/qml_pl.ts \
    languages/qml_cs.ts \
    languages/qml_he.ts \
    languages/qml_ckb.ts \
    languages/qml_lt.ts \
    languages/qml_et.ts \
    languages/qml_bg.ts \
    languages/qml_eo.ts \
    languages/qml_ta.ts

# Additional import path used to resolve QML modules in Qt Creator's code model
QML_IMPORT_PATH =

# Additional import path used to resolve QML modules just for Qt Quick Designer
QML_DESIGNER_IMPORT_PATH =

unix:!macx: {
    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }
    isEmpty(DATADIR) {
        DATADIR = share
    }

    target.path = $$PREFIX/$$BINDIR/

    desktop.files = deploy/linux/com.moonlight_stream.Moonlight.desktop
    desktop.path = $$PREFIX/$$DATADIR/applications/

    icons.files = res/moonlight.svg
    icons.path = $$PREFIX/$$DATADIR/icons/hicolor/scalable/apps/

    appstream.files = deploy/linux/com.moonlight_stream.Moonlight.appdata.xml
    appstream.path = $$PREFIX/$$DATADIR/metainfo/

    INSTALLS += target desktop icons appstream
}
macx {
    # Create Info.plist in object dir with the correct version string
    system(cp $$PWD/Info.plist $$OUT_PWD/Info.plist)
    system(sed -i -e 's/VERSION/$$cat(version.txt)/g' $$OUT_PWD/Info.plist)

    QMAKE_INFO_PLIST = $$OUT_PWD/Info.plist

    APP_BUNDLE_RESOURCES.files = moonlight.icns
    APP_BUNDLE_RESOURCES.path = Contents/Resources

    APP_BUNDLE_PLIST.files = $$OUT_PWD/Info.plist
    APP_BUNDLE_PLIST.path = Contents

    QMAKE_BUNDLE_DATA += APP_BUNDLE_RESOURCES APP_BUNDLE_PLIST

    !disable-prebuilts {
        APP_BUNDLE_FRAMEWORKS.files = $$files(../libs/mac/Frameworks/*.framework, true) $$files(../libs/mac/lib/*.dylib, true)
        APP_BUNDLE_FRAMEWORKS.path = Contents/Frameworks

        QMAKE_BUNDLE_DATA += APP_BUNDLE_FRAMEWORKS

        QMAKE_RPATHDIR += @executable_path/../Frameworks
    }
}
