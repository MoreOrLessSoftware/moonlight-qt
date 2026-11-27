# Shared by Moonlight.exe (app.pro), MoonlightStream.exe (stream/stream.pro) and the static
# library with everything else they're built from (core/core.pro): Qt modules, defines,
# include paths and libraries. Platform-specific sources are listed in CORE_SOURCES and
# CORE_HEADERS, relative to this directory, for the library to build.

QT += core gui network svg
CONFIG += c++17

include($$PWD/../globaldefs.pri)

# The build directory of moonlight-qt.pro, where the dependencies' libraries are
ML_BUILD_ROOT = $$shadowed($$PWD/..)

# Sources include headers relative to this directory
INCLUDEPATH += $$PWD
# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

win32 {
    !exists($$PWD/../libs/windows) {
        error("Missing dependencies. Please run 'powershell .\setup-deps.ps1' to fetch prebuilt libraries.")
    }

    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../libs/windows/include/x64 $$PWD/../libs/windows/include/x64/SDL2
    }
    contains(QT_ARCH, arm64) {
        LIBS += -L$$PWD/../libs/windows/lib/arm64
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64 $$PWD/../libs/windows/include/arm64/SDL2
    }

    INCLUDEPATH += $$PWD/../libs/windows/include
    LIBS += ws2_32.lib winmm.lib dxva2.lib ole32.lib gdi32.lib user32.lib d3d9.lib dwmapi.lib dbghelp.lib
}
macx:!disable-prebuilts {
    !exists($$PWD/../libs/mac) {
        error("Missing dependencies. Please run 'python3 setup-deps.py' to fetch prebuilt libraries.")
    }

    INCLUDEPATH += $$PWD/../libs/mac/include $$PWD/../libs/mac/include/SDL2
    LIBS += -L$$PWD/../libs/mac/lib
}

unix:if(!macx|disable-prebuilts) {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl sdl2 SDL2_ttf

    # We have our own optimized libopus.a for Steam Link
    if(!config_SL|disable-prebuilts) {
        PKGCONFIG += opus
    }

    !disable-ffmpeg {
        packagesExist(libavcodec) {
            PKGCONFIG += libavcodec libavutil libswscale
            CONFIG += ffmpeg

            !disable-libva {
                packagesExist(libva) {
                    !disable-x11 {
                        packagesExist(libva-x11) {
                            CONFIG += libva-x11
                        }
                    }
                    !disable-wayland {
                        packagesExist(libva-wayland) {
                            CONFIG += libva-wayland
                        }
                    }
                    !disable-libdrm {
                        packagesExist(libva-drm) {
                            CONFIG += libva-drm
                        }
                    }
                    CONFIG += libva
                }
            }

            !disable-libvdpau {
                packagesExist(vdpau) {
                    CONFIG += libvdpau
                }
            }

            !disable-mmal {
                packagesExist(mmal) {
                    PKGCONFIG += mmal
                    CONFIG += mmal
                }
            }

            !disable-libdrm {
                packagesExist(libdrm) {
                    PKGCONFIG += libdrm
                    CONFIG += libdrm
                }
            }

            # Disabled by default due to reliability issues. See #1314.
            # CUDA interop is superseded by VDPAU and Vulkan Video.
            enable-cuda {
                packagesExist(ffnvcodec) {
                    PKGCONFIG += ffnvcodec
                    CONFIG += cuda
                }
            }

            !disable-libplacebo {
                packagesExist(libplacebo) {
                    PKGCONFIG += libplacebo
                    CONFIG += libplacebo
                }
            }
        }

        !disable-wayland {
            packagesExist(wayland-client) {
                CONFIG += wayland
                PKGCONFIG += wayland-client
            }
        }

        !disable-x11 {
            packagesExist(x11) {
                DEFINES += HAS_X11
                PKGCONFIG += x11
            }
        }
    }
}
win32 {
    LIBS += -llibssl -llibcrypto -lSDL2 -lSDL2_ttf -lavcodec -lavutil -lswscale -lopus -ldxgi -ld3d11 -llibplacebo
    CONFIG += ffmpeg libplacebo
}
win32:!winrt {
    CONFIG += discord-rpc
}
macx {
    !disable-prebuilts {
        LIBS += -lssl.3 -lcrypto.3 -lavcodec.63 -lavutil.61 -lswscale.10 -lopus.0 -lSDL2 -lSDL2_ttf -lplacebo
        CONFIG += discord-rpc libplacebo
    }

    LIBS += -lobjc -framework VideoToolbox -framework AVFoundation -framework CoreVideo -framework CoreGraphics -framework CoreMedia -framework AppKit -framework Metal -framework QuartzCore
    CONFIG += ffmpeg
}



# Skip the check for a newer upstream release, such as in a custom fork
disable-update-check {
    message(Update check disabled)

    DEFINES += DISABLE_UPDATE_CHECK
}

# Platform-specific renderers and decoders
ffmpeg {
    message(FFmpeg decoder selected)

    DEFINES += HAVE_FFMPEG
    CORE_SOURCES += \
        streaming/video/ffmpeg.cpp \
        streaming/video/ffmpeg-renderers/genhwaccel.cpp \
        streaming/video/ffmpeg-renderers/sdlvid.cpp \
        streaming/video/ffmpeg-renderers/swframemapper.cpp \
        streaming/video/ffmpeg-renderers/pacer/pacer.cpp \
        streaming/video/ffmpeg-renderers/pacer/pacertrace.cpp \
        streaming/video/ffmpeg-renderers/pacer/gputimer.cpp

    CORE_HEADERS += \
        streaming/video/ffmpeg.h \
        streaming/video/ffmpeg-renderers/renderer.h \
        streaming/video/ffmpeg-renderers/genhwaccel.h \
        streaming/video/ffmpeg-renderers/sdlvid.h \
        streaming/video/ffmpeg-renderers/swframemapper.h \
        streaming/video/ffmpeg-renderers/pacer/pacer.h \
        streaming/video/ffmpeg-renderers/pacer/pacertrace.h \
        streaming/video/ffmpeg-renderers/pacer/gputimer.h
}
libva {
    message(VAAPI renderer selected)

    PKGCONFIG += libva
    DEFINES += HAVE_LIBVA
    CORE_SOURCES += streaming/video/ffmpeg-renderers/vaapi.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/vaapi.h
}
libva-x11 {
    message(VAAPI X11 support enabled)

    PKGCONFIG += libva-x11
    DEFINES += HAVE_LIBVA_X11
}
libva-wayland {
    message(VAAPI Wayland support enabled)

    PKGCONFIG += libva-wayland
    DEFINES += HAVE_LIBVA_WAYLAND
}
libva-drm {
    message(VAAPI DRM support enabled)

    PKGCONFIG += libva-drm
    DEFINES += HAVE_LIBVA_DRM
}
libvdpau {
    message(VDPAU renderer selected)

    DEFINES += HAVE_LIBVDPAU
    CORE_SOURCES += streaming/video/ffmpeg-renderers/vdpau.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/vdpau.h
}
mmal {
    message(MMAL renderer selected)

    DEFINES += HAVE_MMAL
    CORE_SOURCES += streaming/video/ffmpeg-renderers/mmal.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/mmal.h

    # We suppress EGL usage when MMAL is available because MMAL has
    # significantly better performance than EGL on the Pi. Setting
    # this option allows EGL usage even if built with MMAL support.
    #
    # It is highly recommended to also build with 'gpuslow' to avoid
    # EGL being preferred if direct DRM rendering is available.
    allow-egl-with-mmal {
        message(Allowing EGL usage with MMAL enabled)

        DEFINES += ALLOW_EGL_WITH_MMAL
    }
}
libdrm {
    message(DRM renderer selected)

    DEFINES += HAVE_DRM
    CORE_SOURCES += streaming/video/ffmpeg-renderers/drm.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/drm.h

    linux {
        !disable-masterhooks {
            message(Master hooks enabled)
            DEFINES += HAVE_DRM_MASTER_HOOKS
            MASTERHOOK_SOURCES = masterhook.c masterhook_internal.c
            LIBS += -ldl -pthread
        }
    }
}
cuda {
    message(CUDA support enabled)

    DEFINES += HAVE_CUDA
    CORE_SOURCES += streaming/video/ffmpeg-renderers/cuda.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/cuda.h

    # ffnvcodec uses libdl in cuda_load_functions()/cuda_free_functions()
    LIBS += -ldl
}
libplacebo {
    message(Vulkan support enabled via libplacebo)

    DEFINES += HAVE_LIBPLACEBO_VULKAN
    CORE_SOURCES += \
        streaming/video/ffmpeg-renderers/plvk.cpp \
        streaming/video/ffmpeg-renderers/plvk_c.c
    CORE_HEADERS += \
        streaming/video/ffmpeg-renderers/plvk.h

    macx {
        CORE_SOURCES += streaming/video/ffmpeg-renderers/plvk_objc.mm
    }

    # PyroWave decodes into frames for the Vulkan renderer. The library is loaded at
    # runtime (libpyrowave-shared.so.1), so it isn't needed to build.
    linux {
        message(PyroWave support enabled)

        DEFINES += HAVE_PYROWAVE
        CORE_SOURCES += \
            streaming/video/pyrowave/pyrowavedecoder.cpp \
            streaming/video/pyrowave/pyrowavevkdecoder.cpp \
            streaming/video/pyrowave/pyrowavestagetimer.cpp
        CORE_HEADERS += \
            streaming/video/pyrowave/pyrowavedecoder.h \
            streaming/video/pyrowave/pyrowavevkdecoder.h \
            streaming/video/pyrowave/pyrowavestagetimer.h \
            streaming/video/pyrowave/pyrowaveapi.h \
            streaming/video/pyrowave/pyrowave.h
        LIBS += -ldl
    }
}
config_EGL {
    message(EGL renderer selected)

    CONFIG += egl
    DEFINES += HAVE_EGL
    CORE_SOURCES += \
        streaming/video/ffmpeg-renderers/eglvid.cpp \
        streaming/video/ffmpeg-renderers/egl_extensions.cpp \
        streaming/video/ffmpeg-renderers/eglimagefactory.cpp
    CORE_HEADERS += \
        streaming/video/ffmpeg-renderers/eglvid.h \
        streaming/video/ffmpeg-renderers/eglimagefactory.h
}
config_SL {
    message(Steam Link build configuration selected)

    !disable-prebuilts {
        # Link against our NEON-optimized libopus build
        LIBS += -L$$PWD/../libs/steamlink/lib
        INCLUDEPATH += $$PWD/../libs/steamlink/include
        LIBS += -lopus -larmasm -lNE10
    }

    DEFINES += EMBEDDED_BUILD STEAM_LINK HAVE_SLVIDEO HAVE_SLAUDIO
    LIBS += -lSLVideo -lSLAudio

    CORE_SOURCES += \
        streaming/video/slvid.cpp \
        streaming/audio/renderers/slaud.cpp
    CORE_HEADERS += \
        streaming/video/slvid.h \
        streaming/audio/renderers/slaud.h
}
win32 {
    CORE_HEADERS += streaming/video/ffmpeg-renderers/dxutil.h
}
win32:!winrt {
    message(GameInput gamepad support selected)

    # GameInput v4 (vendored MIT header + loader; the runtime is the GameInput redistributable)
    DEFINES += HAVE_GAMEINPUT
    CORE_SOURCES += \
        streaming/input/gameinput.cpp \
        streaming/input/gameinput/GameInputLoader.cpp
    CORE_HEADERS += \
        streaming/input/gameinput.h \
        streaming/input/gameinput/GameInput.h
}
win32:!winrt {
    message(DXVA2 and D3D11VA renderers selected)

    CORE_SOURCES += \
        streaming/video/ffmpeg-renderers/dxva2.cpp \
        streaming/video/ffmpeg-renderers/d3d11va.cpp \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.cpp \
        streaming/video/pyrowave/pyrowavedecoder.cpp \
        streaming/video/pyrowave/pyrowaved3d11decoder.cpp \
        streaming/video/pyrowave/pyrowavestagetimer.cpp

    CORE_HEADERS += \
        streaming/video/ffmpeg-renderers/dxva2.h \
        streaming/video/ffmpeg-renderers/d3d11va.h \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.h \
        streaming/video/pyrowave/pyrowavedecoder.h \
        streaming/video/pyrowave/pyrowaved3d11decoder.h \
        streaming/video/pyrowave/pyrowavestagetimer.h \
        streaming/video/pyrowave/pyrowaveapi.h \
        streaming/video/pyrowave/pyrowave.h

    # PyroWave decodes into frames for the D3D11 renderer
    DEFINES += HAVE_PYROWAVE

    # The library is loaded at runtime from Moonlight's directory, so copy it next to the
    # built exe for running from Qt Creator. Like build-arch.bat, it's taken from PYROWAVE_DLL
    # or a pyrowave build next to this repo. Only x64 is built for now.
    moonlight_gui:contains(QT_ARCH, x86_64) {
        PYROWAVE_DLL = $$(PYROWAVE_DLL)
        isEmpty(PYROWAVE_DLL): PYROWAVE_DLL = $$PWD/../../pyrowave/output/bin/libpyrowave-shared-1.dll
        exists($$PYROWAVE_DLL) {
            CONFIG(debug, debug|release): PYROWAVE_DEST = $$OUT_PWD/debug
            else: PYROWAVE_DEST = $$OUT_PWD/release
            !isEmpty(QMAKE_POST_LINK): QMAKE_POST_LINK += $$escape_expand(\\n\\t)
            QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$shell_path($$PYROWAVE_DLL)) \
                               $$shell_quote($$shell_path($$PYROWAVE_DEST/libpyrowave-shared-1.dll))
        } else {
            message(PyroWave library not found at $$PYROWAVE_DLL, so it is not copied next to Moonlight)
        }
    }
}
macx {
    message(VideoToolbox renderer selected)

    CORE_SOURCES += \
        streaming/video/ffmpeg-renderers/vt_base.mm \
        streaming/video/ffmpeg-renderers/vt_avsamplelayer.mm \
        streaming/video/ffmpeg-renderers/vt_metal.mm

    CORE_HEADERS += \
        streaming/video/ffmpeg-renderers/vt.h
}
discord-rpc {
    message(Discord integration enabled)

    LIBS += -ldiscord-rpc
    DEFINES += HAVE_DISCORD
}
embedded {
    message(Embedded build)

    DEFINES += EMBEDDED_BUILD
}
glslow {
    message(GL slow build)

    DEFINES += GL_IS_SLOW
}
vkslow {
    message(Vulkan slow build)

    DEFINES += VULKAN_IS_SLOW
}
gpuslow {
    message(GPU slow build)

    DEFINES += GL_IS_SLOW VULKAN_IS_SLOW
}
wayland {
    message(Wayland extensions enabled)

    DEFINES += HAS_WAYLAND
    CORE_SOURCES += streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.cpp
    CORE_HEADERS += streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.h
}


win32:CONFIG(release, debug|release): LIBS += -L$$ML_BUILD_ROOT/moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$ML_BUILD_ROOT/moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$ML_BUILD_ROOT/moonlight-common-c/ -lmoonlight-common-c

INCLUDEPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src
DEPENDPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src

win32:CONFIG(release, debug|release): LIBS += -L$$ML_BUILD_ROOT/qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$ML_BUILD_ROOT/qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$ML_BUILD_ROOT/qmdnsengine/ -lqmdnsengine

INCLUDEPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine
DEPENDPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine

win32:CONFIG(release, debug|release): LIBS += -L$$ML_BUILD_ROOT/h264bitstream/release/ -lh264bitstream
else:win32:CONFIG(debug, debug|release): LIBS += -L$$ML_BUILD_ROOT/h264bitstream/debug/ -lh264bitstream
else:unix: LIBS += -L$$ML_BUILD_ROOT/h264bitstream/ -lh264bitstream

INCLUDEPATH += $$PWD/../h264bitstream
DEPENDPATH += $$PWD/../h264bitstream

!winrt {
    win32:CONFIG(release, debug|release): LIBS += -L$$ML_BUILD_ROOT/AntiHooking/release/ -lAntiHooking
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$ML_BUILD_ROOT/AntiHooking/debug/ -lAntiHooking

    INCLUDEPATH += $$PWD/../AntiHooking
    DEPENDPATH += $$PWD/../AntiHooking
}


DEFINES += VERSION_STR=\\\"$$cat($$PWD/version.txt)\\\"

# Display version shown in the UI: the git tag if HEAD is exactly on one.
# Otherwise it's left undefined and the branch and build date are used, as
# <branch>-YYYY.MM.DD.hhmm (see systemproperties.cpp), or just the date when
# HEAD is detached. This is separate from VERSION_STR, which the update checker
# and Windows resources require to stay a plain numeric version.
GIT_TAG = $$system("git -C \"$$PWD\" describe --tags --exact-match 2>&1", lines, GIT_TAG_STATUS)
equals(GIT_TAG_STATUS, 0):!isEmpty(GIT_TAG) {
    DEFINES += DISPLAY_VERSION_STR=\\\"$$first(GIT_TAG)\\\"
} else {
    GIT_BRANCH = $$system("git -C \"$$PWD\" rev-parse --abbrev-ref HEAD 2>&1", lines, GIT_BRANCH_STATUS)
    equals(GIT_BRANCH_STATUS, 0):!isEmpty(GIT_BRANCH):!equals(GIT_BRANCH, HEAD) {
        DEFINES += DISPLAY_VERSION_BRANCH=\\\"$$first(GIT_BRANCH)\\\"
    }
}

# Both executables get the manifest (DPI awareness and the rest), the icon and a version
equals(TEMPLATE, app) {
    VERSION = "$$cat($$PWD/version.txt)"

    win32 {
        RC_ICONS = $$PWD/moonlight.ico
        QMAKE_TARGET_COMPANY = Moonlight Game Streaming Project
        QMAKE_TARGET_PRODUCT = Moonlight

        CONFIG -= embed_manifest_exe
        QMAKE_LFLAGS += /MANIFEST:embed /MANIFESTINPUT:$${PWD}/Moonlight.exe.manifest
    }
}
