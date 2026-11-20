# Unit tests for the client backend. This project is standalone and is not
# part of the default build; build it with:  qmake tests.pro && make check
# (the main project and its dependencies must have been built first so that
# ../moonlight-common-c can be linked from the same build directory layout).
QT += core network testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = backend_tests

DEFINES += RELEASE_FIXTURE_DIR=\\\"$$PWD/fixtures/release\\\"

INCLUDEPATH += $$PWD/../app $$PWD/../app/backend $$PWD/../../common/moonlight-common-c/src

win32 {
    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../libs/windows/include/x64
    }
    INCLUDEPATH += $$PWD/../libs/windows/include
    LIBS += -llibssl -llibcrypto crypt32.lib ws2_32.lib winmm.lib
    CONFIG(release, debug|release): LIBS += -L$$MOONLIGHT_COMMON_C_DIR/release -lmoonlight-common-c
    else: LIBS += -L$$MOONLIGHT_COMMON_C_DIR/debug -lmoonlight-common-c
}
else {
    LIBS += -L$$MOONLIGHT_COMMON_C_DIR -lmoonlight-common-c -lssl -lcrypto
}

SOURCES += \
    tst_backend.cpp \
    ../app/backend/nvhttp.cpp \
    ../app/backend/nvapp.cpp \
    ../app/backend/nvaddress.cpp \
    ../app/backend/nvcomputer.cpp \
    ../app/backend/identitymanager.cpp \
    ../app/backend/identitystore.cpp \
    ../app/backend/moonlightimport.cpp \
    ../app/backend/releaseverify.cpp \
    ../app/settings/compatfetcher.cpp \
    ../app/path.cpp

HEADERS += \
    ../app/backend/nvhttp.h \
    ../app/backend/nvapp.h \
    ../app/backend/nvaddress.h \
    ../app/backend/nvcomputer.h \
    ../app/backend/identitymanager.h \
    ../app/backend/identitystore.h \
    ../app/backend/moonlightimport.h \
    ../app/backend/releaseverify.h \
    ../app/settings/compatfetcher.h \
    ../app/path.h
