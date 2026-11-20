# NyxPlayUpdater.exe: a small separate program that swaps the files of an installed client
# after the client has exited. It does not use Qt.
QT -= core gui
CONFIG -= qt
CONFIG += c++17 windows
TARGET = NyxPlayUpdater
TEMPLATE = app

include(../globaldefs.pri)

# Statically linked runtime: the updater must start even when the runtime next to the client is the very file being replaced
QMAKE_CFLAGS_RELEASE ~= s/-MD/-MT/
QMAKE_CXXFLAGS_RELEASE ~= s/-MD/-MT/
QMAKE_CFLAGS_RELEASE_WITH_DEBUGINFO ~= s/-MD/-MT/
QMAKE_CXXFLAGS_RELEASE_WITH_DEBUGINFO ~= s/-MD/-MT/
QMAKE_CFLAGS_DEBUG ~= s/-MDd/-MTd/
QMAKE_CXXFLAGS_DEBUG ~= s/-MDd/-MTd/

DEFINES += UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
LIBS += -luser32 -lshell32

SOURCES += \
    main.cpp \
    applyupdate.cpp

HEADERS += \
    applyupdate.h

CONFIG -= embed_manifest_exe
QMAKE_LFLAGS += /MANIFEST:embed /MANIFESTINPUT:$${PWD}/NyxPlayUpdater.exe.manifest

VERSION = "$$cat(../app/version.txt)"
QMAKE_TARGET_PRODUCT = NyxPlay
QMAKE_TARGET_DESCRIPTION = NyxPlay updater
