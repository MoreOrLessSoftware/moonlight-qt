# The library with everything but main() and the GUI, before the libraries it needs
CONFIG(debug, debug|release): ML_CORE_DIR = $$shadowed($$PWD)/core/debug
else: ML_CORE_DIR = $$shadowed($$PWD)/core/release
win32 {
    LIBS += -L$$ML_CORE_DIR -lmoonlight-core
    PRE_TARGETDEPS += $$ML_CORE_DIR/moonlight-core.lib
}
else {
    ML_CORE_DIR = $$shadowed($$PWD)/core
    LIBS += -L$$ML_CORE_DIR -lmoonlight-core
    PRE_TARGETDEPS += $$ML_CORE_DIR/libmoonlight-core.a
}
