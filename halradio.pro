
load(configure)

TARGET = qtmedia_halradio
QT += multimedia-private
CONFIG += link_pkgconfig

PKGCONFIG += android-headers libhardware

PLUGIN_TYPE = mediaservice
PLUGIN_CLASS_NAME = FMRadioServicePlugin
load(qt_plugin)

qtCompileTest(radio_event_ea)
qtCompileTest(ld_gold)

config_radio_event_ea {
    DEFINES += SUPPORT_RADIO_EVENT_EA
}

config_ld_gold {
    QMAKE_LFLAGS += -fuse-ld=gold
}

SOURCES += fmradioserviceplugin.cpp \
           fmradiodatacontrol.cpp \
           fmradioservice.cpp \
           fmradiotunercontrol.cpp \
           fmradiohalcontrol.cpp

HEADERS += fmradioserviceplugin.h \
           fmradiodatacontrol.h \
           fmradioservice.h \
           fmradiotunercontrol.h \
           fmradiohalcontrol.h

QMAKE_LFLAGS += -lhybris-common
