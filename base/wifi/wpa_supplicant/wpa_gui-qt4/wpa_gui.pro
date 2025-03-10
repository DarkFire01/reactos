TEMPLATE	= app
LANGUAGE	= C++
TRANSLATIONS	= lang/wpa_gui_de.ts
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG	+= qt warn_on release

DEFINES += CONFIG_CTRL_IFACE

win32 {
  LIBS += -lws2_32 -lcrypt32 -lgdi32 -ladvapi32 -luser32 -static
  DEFINES += NOMINMAX CONFIG_NATIVE_WINDOWS CONFIG_CTRL_IFACE_NAMED_PIPE
  SOURCES += ../../src/utils/os_win32.c
  RC_FILE = wpa_gui.rc
} else:win32-g++ {
  # cross compilation to win32
  LIBS += -lws2_32 -lcrypt32 -lgdi32 -ladvapi32 -luser32 -static -mwindows
  DEFINES += NOMINMAX CONFIG_NATIVE_WINDOWS CONFIG_CTRL_IFACE_NAMED_PIPE
  SOURCES += ../../src/utils/os_win32.c
} else:win32-x-g++ {
  # cross compilation to win32
  LIBS += -lws2_32 -lcrypt32 -lgdi32 -ladvapi32 -luser32 -static -mwindows
  DEFINES += NOMINMAX CONFIG_NATIVE_WINDOWS CONFIG_CTRL_IFACE_NAMED_PIPE
  DEFINES += _X86_
  SOURCES += ../../src/utils/os_win32.c
} else {
  DEFINES += CONFIG_CTRL_IFACE_UNIX
  SOURCES += ../../src/utils/os_unix.c
}

INCLUDEPATH	+= . .. ../../src ../../src/utils

HEADERS	+= wpamsg.h \
	wpagui.h \
	eventhistory.h \
	scanresults.h \
	scanresultsitem.h \
	signalbar.h \
	userdatarequest.h \
	networkconfig.h \
	addinterface.h \
	peers.h \
	stringquery.h

SOURCES	+= main.cpp \
	wpagui.cpp \
	eventhistory.cpp \
	scanresults.cpp \
	scanresultsitem.cpp \
	signalbar.cpp \
	userdatarequest.cpp \
	networkconfig.cpp \
	addinterface.cpp \
	peers.cpp \
	stringquery.cpp \
	../../src/common/wpa_ctrl.c

RESOURCES += icons.qrc icons_png.qrc icons_status.qrc

FORMS	= wpagui.ui \
	eventhistory.ui \
	scanresults.ui \
	userdatarequest.ui \
	networkconfig.ui \
	peers.ui


unix {
  UI_DIR = .ui
  MOC_DIR = .moc
  OBJECTS_DIR = .obj
}
