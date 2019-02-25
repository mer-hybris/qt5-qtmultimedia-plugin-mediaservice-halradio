/*
  Copyright (C) 2018 Jolla Ltd.
  Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>
  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "fmradiohalcontrol.h"

#include <QDebug>
#include <QRegExp>
#include <QLoggingCategory>

#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>

#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <android-config.h>

#include <hardware/radio.h>
#include <system/radio.h>
#include <system/radio_metadata.h>

extern "C" {
#include <hybris/common/binding.h>
}

Q_LOGGING_CATEGORY(log, "radio.fm", QtWarningMsg)

// HAL uses unsigned int kHz, Qt uses int Hz
#define FREQ_HAL_TO_QT(f)       (static_cast<int>(f) * 1000)
#define FREQ_QT_TO_HAL(f)       (static_cast<unsigned int>(f) / 1000)

#define SEARCH_SCAN_TIMEOUT_MS  (10 * 1000)

typedef int (*libradio_metadata_check)(const radio_metadata_t*);
typedef int (*libradio_metadata_get_count)(const radio_metadata_t*);
typedef int (*libradio_metadata_get_at_index)(const radio_metadata_t*,
                                              const unsigned int,
                                              radio_metadata_key_t*,
                                              radio_metadata_type_t*,
                                              void**,
                                              unsigned int*);

struct HalPrivate {
    HalPrivate() : hwmod(0)
                 , radiohw(0)
                 , tuner(0)
                 , libradio_metadata_handle(0)
                 , metadata_check(0)
                 , metadata_get_count(0)
                 , metadata_get_at_index(0)
    {}

    struct hw_module_t *hwmod;
    radio_hw_device_t *radiohw;
    const struct radio_tuner *tuner;
    radio_hal_properties_t properties;
    radio_hal_band_config_t config;

    // metadata handling
    void *libradio_metadata_handle;
    libradio_metadata_check metadata_check;
    libradio_metadata_get_count metadata_get_count;
    libradio_metadata_get_at_index metadata_get_at_index;
};

FMRadioHalControl::FMRadioHalControl()
    : QObject()
    , m_hal(new HalPrivate)
    , m_error(QRadioTuner::NoError)
    , m_rdsError(QRadioData::NoError)
    , m_tunerReady(false)
    , m_antennaConnected(true)
    , m_stereoEnabled(true)
    , m_currentFreq(0)
    , m_searchMode(QRadioTuner::SearchFast)
    , m_seekTimer(new QTimer(this))
    , m_searching(false)
    , m_searchAll(false)
    , m_searchAllLast(false)
    , m_searchWaitForRDS(false)
    , m_firstFoundFrequency(0)
    , m_searchRange(0)
    , m_lastFrequency(0)
    , m_stationId()
    , m_stationName()
    , m_programType(0)  // Undefined
    , m_radioText()
{
    m_seekTimer->setInterval(SEARCH_SCAN_TIMEOUT_MS);
    m_seekTimer->setSingleShot(false);
    connect(m_seekTimer, SIGNAL(timeout()),
            this, SLOT(handleSeekTimeout()));

    // tuner calls shouldn't be made from the event callback
    // thread, so handle SearchGetStationId seeking using signals
    connect(this, SIGNAL(seekNextChannel()),
            this, SLOT(searchForward()));

    connect(this, SIGNAL(eventHwFailure()),
            this, SLOT(handleHwFailure()));
    connect(this, SIGNAL(eventConfig(int, bool)),
            this, SLOT(handleConfig(int, bool)));
    connect(this, SIGNAL(eventAntenna(bool)),
            this, SLOT(handleAntenna(bool)));
    connect(this, SIGNAL(eventTuned(unsigned, bool)),
            this, SLOT(handleTuned(unsigned, bool)));
    connect(this, SIGNAL(eventTA(bool)),
            this, SLOT(handleTA(bool)));
    connect(this, SIGNAL(eventAFSwitch(bool)),
            this, SLOT(handleAFSwitch(bool)));
    connect(this, SIGNAL(eventEA(bool)),
            this, SLOT(handleEA(bool)));

    openRadioMetadata();
    openRadio();
}

FMRadioHalControl::~FMRadioHalControl()
{
    closeRadio();
    if (m_hal->libradio_metadata_handle)
        android_dlclose(m_hal->libradio_metadata_handle);
    delete m_hal;
}

void FMRadioHalControl::openRadioMetadata()
{
    if (!m_hal || m_hal->libradio_metadata_handle)
        return;

    static const char *lib_paths[] = {
        "/vendor/lib64/libradio_metadata.so",
        "/system/lib64/libradio_metadata.so",
        "/vendor/lib/libradio_metadata.so",
        "/system/lib/libradio_metadata.so",
        NULL
    };

    qCDebug(log) << "Open radio metadata library.";
    for (int i = 0; lib_paths[i]; ++i) {
        if ((m_hal->libradio_metadata_handle = android_dlopen(lib_paths[i], RTLD_LAZY)))
            break;
    }

    if (!m_hal->libradio_metadata_handle) {
        qCWarning(log) << "Failed to open metadata library.";
        setRdsError(QRadioData::ResourceError);
        return;
    }

    m_hal->metadata_check = reinterpret_cast<libradio_metadata_check>(android_dlsym(m_hal->libradio_metadata_handle,
                                                                                    "radio_metadata_check"));
    m_hal->metadata_get_count = reinterpret_cast<libradio_metadata_get_count>(android_dlsym(m_hal->libradio_metadata_handle,
                                                                                            "radio_metadata_get_count"));
    m_hal->metadata_get_at_index = reinterpret_cast<libradio_metadata_get_at_index>(android_dlsym(m_hal->libradio_metadata_handle,
                                                                                                  "radio_metadata_get_at_index"));

    if (m_hal->metadata_check &&
        m_hal->metadata_get_count &&
        m_hal->metadata_get_at_index) {
        qCDebug(log) << "Radio metadata enabled.";
    } else {
        qCDebug(log) << "Failed to enable metadata.";
        setRdsError(QRadioData::ResourceError);
    }
}

void FMRadioHalControl::openRadio()
{
    qCDebug(log) << "Open radio HAL.";
    hw_get_module_by_class(RADIO_HARDWARE_MODULE_ID,
                           RADIO_HARDWARE_MODULE_ID_FM,
                           (const hw_module_t**) &m_hal->hwmod);
    if (!m_hal->hwmod) {
        qCWarning(log) << "Failed to get " RADIO_HARDWARE_MODULE_ID "." RADIO_HARDWARE_MODULE_ID_FM;
        return;
    }

    int ret;

    if ((ret = radio_hw_device_open(m_hal->hwmod, &m_hal->radiohw)) != 0) {
        qCWarning(log) << "Failed to open radio device:" << ret;
        return;
    }

    m_hal->radiohw->get_properties(m_hal->radiohw, &m_hal->properties);

    // TODO we should probably get from somewhere what region is really used.
    // For now hard-code so we first try to set config based on region ITU-1
    // and if not found then ITU-2.

    if (!setRadioConfig(RADIO_BAND_FM, radio_demephasis_for_region(RADIO_REGION_ITU_1))) {
        if (!setRadioConfig(RADIO_BAND_FM, radio_demephasis_for_region(RADIO_REGION_ITU_2))) {
            qCWarning(log) << "Failed to get configuration for tuner, using default ITU-1 FM.";
            setRadioConfigFallback();
        }
    }
}

bool FMRadioHalControl::setRadioConfig(radio_band_t band, radio_deemphasis_t deemphasis)
{
    for (unsigned i = 0; i < m_hal->properties.num_bands; ++i) {
        if (m_hal->properties.bands[i].type == band &&
            m_hal->properties.bands[i].fm.deemphasis == deemphasis) {

            m_hal->config.type              = m_hal->properties.bands[i].type;
            m_hal->config.antenna_connected = m_hal->properties.bands[i].antenna_connected;
            m_hal->config.lower_limit       = m_hal->properties.bands[i].lower_limit;
            m_hal->config.upper_limit       = m_hal->properties.bands[i].upper_limit;
            m_hal->config.num_spacings      = m_hal->properties.bands[i].num_spacings;
            memcpy(&m_hal->config.spacings, &m_hal->properties.bands[i].spacings, sizeof(m_hal->config.spacings));
            m_hal->config.fm.deemphasis     = m_hal->properties.bands[i].fm.deemphasis;
            m_hal->config.fm.stereo         = m_hal->properties.bands[i].fm.stereo;
            m_hal->config.fm.rds            = m_hal->properties.bands[i].fm.rds;
            m_hal->config.fm.ta             = m_hal->properties.bands[i].fm.ta;
            m_hal->config.fm.af             = m_hal->properties.bands[i].fm.af;
            m_hal->config.fm.ea             = false;

            return true;
        }
    }

    return false;
}

void FMRadioHalControl::setRadioConfigFallback()
{
    // Fallback configs for ITU-1 FM.
    m_hal->config.type              = RADIO_BAND_FM;
    m_hal->config.antenna_connected = true;
    m_hal->config.lower_limit       = 87500;
    m_hal->config.upper_limit       = 108000;
    m_hal->config.num_spacings      = 1;
    m_hal->config.spacings[0]       = 100;
    m_hal->config.fm.deemphasis     = radio_demephasis_for_region(RADIO_REGION_ITU_1);
    m_hal->config.fm.stereo         = true;
    m_hal->config.fm.rds            = radio_rds_for_region(true, RADIO_REGION_ITU_1);
    m_hal->config.fm.ta             = true;
    m_hal->config.fm.af             = false;
    m_hal->config.fm.ea             = false;
}

void FMRadioHalControl::closeRadio()
{
    stop();

    if (!m_hal || !m_hal->radiohw)
        return;

    qCDebug(log) << "Close HAL.";
    radio_hw_device_close(m_hal->radiohw);
    m_hal->radiohw = 0;
}

bool FMRadioHalControl::tunerEnabled() const
{
    return m_hal && m_hal->tuner && m_tunerReady;
}

QRadioTuner::State FMRadioHalControl::tunerState() const
{
    return tunerEnabled() ? QRadioTuner::ActiveState : QRadioTuner::StoppedState;
}

bool FMRadioHalControl::isRdsAvailable() const
{
    return m_hal->libradio_metadata_handle && m_hal->config.fm.rds != RADIO_RDS_NONE;
}

QRadioTuner::Band FMRadioHalControl::band() const
{
    return QRadioTuner::FM;
}

void FMRadioHalControl::setBand(QRadioTuner::Band /* unused */)
{
}

bool FMRadioHalControl::isBandSupported(QRadioTuner::Band b) const
{
    if (b == QRadioTuner::FM)
        return true;

    return false;
}

int FMRadioHalControl::frequency() const
{
    return FREQ_HAL_TO_QT(m_currentFreq);
}

void FMRadioHalControl::setFrequency(int newFrequency)
{
    unsigned frequency = FREQ_QT_TO_HAL(newFrequency);

    if (frequency == m_currentFreq)
        return;

    if (frequency < m_hal->config.lower_limit)
        frequency = m_hal->config.lower_limit;
    if (frequency > m_hal->config.upper_limit)
        frequency = m_hal->config.upper_limit;

    m_currentFreq = frequency;

    qCDebug(log) << "Set frequency" << m_currentFreq;

    if (!tunerEnabled())
        return;

    resetRDS();
    setTuning();
}

bool FMRadioHalControl::isStereo() const
{
    return m_stereoEnabled;
}

QRadioTuner::StereoMode FMRadioHalControl::stereoMode() const
{
    return QRadioTuner::Auto;
}

void FMRadioHalControl::setStereoMode(QRadioTuner::StereoMode /* unused */)
{
}

int FMRadioHalControl::signalStrength() const
{
    return 100;
}

int FMRadioHalControl::volume() const
{
    return 100;
}

void FMRadioHalControl::setVolume(int /* unused */)
{
}

bool FMRadioHalControl::isMuted() const
{
    return false;
}

void FMRadioHalControl::setMuted(bool /* unused */)
{
}

bool FMRadioHalControl::isSearching() const
{
    return m_searching;
}

bool FMRadioHalControl::isAntennaConnected() const
{
    return m_antennaConnected;
}

void FMRadioHalControl::seek(radio_direction_t direction)
{
    if (!tunerEnabled())
        return;

    if (!m_searchAll)
        resetRDS();

    m_seekTimer->start();

    int ret = m_hal->tuner->scan(m_hal->tuner, direction, false);

    if (ret == 0) {
        if (!m_searchAll)
            setSearching(true);
    } else
        qCWarning(log) << "Failed to scan" << (direction == RADIO_DIRECTION_UP ? "forward:" : "backward:") << ret;
}

void FMRadioHalControl::resetRDS()
{
    if (!m_radioText.isEmpty()) {
        m_radioText.clear();
        emit radioTextChanged(m_radioText);
    }

    if (!m_stationName.isEmpty()) {
        m_stationName.clear();
        emit stationNameChanged(m_stationName);
    }

    if (m_programType != 0) {
        m_programType = 0; // Undefined
        emit programTypeChanged(programTypeValue(0, m_programType));
        emit programTypeNameChanged(programTypeNameString(0, 0));
    }

    if (!m_stationId.isEmpty()) {
        m_stationId.clear();
        emit stationIdChanged(m_stationId);
    }
}

void FMRadioHalControl::setSearching(bool searching)
{
    if (!searching && m_seekTimer->isActive())
        m_seekTimer->stop();

    if (m_searching != searching) {
        m_searching = searching;
        emit searchingChanged(m_searching);
    }
}

void FMRadioHalControl::setStereoEnabled(bool enabled)
{
    if (enabled != m_stereoEnabled) {
        m_stereoEnabled = enabled;
        qCDebug(log) << "Channel count changes to" << (m_stereoEnabled ? "stereo" : "mono");
        emit stereoStatusChanged(m_stereoEnabled);
    }
}

void FMRadioHalControl::searchForward()
{
    seek(RADIO_DIRECTION_UP);
}

void FMRadioHalControl::searchBackward()
{
    seek(RADIO_DIRECTION_DOWN);
}

void FMRadioHalControl::handleSeekTimeout()
{
    if (m_searchAll) {
        if (m_searchMode == QRadioTuner::SearchFast) {
            qCDebug(log) << "SearchFast timeout. Cancel search.";
            cancelSearch();
        } else {
            if (m_firstFoundFrequency == 0) {
                qCDebug(log) << "SearchGetStationId timeout. Cancel search.";
                cancelSearch();
            } else {
                qCDebug(log) << "SearchGetStationId found channel" << m_currentFreq << ": \"\" (timeout while waiting RDS).";
                emit stationFound(FREQ_HAL_TO_QT(m_currentFreq), m_stationId);
                searchForward();
            }
        }
    } else
        cancelSearch();
}

void FMRadioHalControl::searchAllStations(QRadioTuner::SearchMode searchMode)
{
    // searchAllStations QRadioTuner::SearchFast is handled as such
    //
    // 0. Start seek timer. Every time a frequency is found reset
    // the timer. If timer fires cancel search mode.
    //
    // 1. Calculate the frequency range (upper limit - lower limit)
    // 2. Store current frequency (active)
    // 3. From currently active frequency scan forward
    // 4. Calculate the diff to last active frequency
    // 5. Reduce the diff from range
    // 6. Keep going until the whole range is exhausted
    // 7. Stay with the last tuned frequency
    //
    // It is possible we will emit same frequency twice with
    // certain starting frequencies, but if that happens duplicate
    // frequencies would be the first and last one, so store
    // first tuned frequency and emit that only if it differs from
    // last found frequency.
    //
    // QRadioTuner::SearchGetStationId differs from this sequence
    // in that after every scan (3) we will wait until RDS_PI RDS
    // event is received or seek timer fires, then scan forward.
    // Search is cancelled only if no channel has been found during
    // the initial scanning.

    resetRDS();

    if (m_hal->config.fm.rds == RADIO_RDS_NONE)
        m_searchMode = QRadioTuner::SearchFast;
    else
        m_searchMode = searchMode;

    if (m_searchMode == QRadioTuner::SearchGetStationId)
        m_searchWaitForRDS = true;

    m_searchAll = true;
    m_searchAllLast = false;
    m_firstFoundFrequency = 0;
    m_searchRange = m_hal->config.upper_limit - m_hal->config.lower_limit;
    m_lastFrequency = m_currentFreq - m_hal->config.lower_limit;

    qCDebug(log) << "Search all stations, start from" << m_currentFreq << "range" << m_searchRange;
    setSearching(true);
    searchForward();
}

void FMRadioHalControl::cancelSearch()
{
    if (!m_searching || !tunerEnabled())
        return;

    int ret = m_hal->tuner->cancel(m_hal->tuner);

    qCDebug(log) << "Cancel" << (m_searchAll ? "searchAll" : "search");
    m_searchAll = false;
    m_searchAllLast = false;
    m_searchWaitForRDS = false;
    setSearching(false);

    if (ret != 0)
        qCWarning(log) << "Failed to cancel:" << ret;
}

void FMRadioHalControl::handleHwFailure()
{
    qCWarning(log) << "Tuner HW Failure, reset tuner to stopped state.";
    setError(QRadioTuner::ResourceError);
    stop();
}

void FMRadioHalControl::setTuning()
{
    if (!m_hal || !m_hal->tuner || m_currentFreq == 0)
        return;

    qCDebug(log) << "Apply frequency" << m_currentFreq;

    int ret = m_hal->tuner->tune(m_hal->tuner, m_currentFreq, 0);
    if (ret != 0)
        qCWarning(log) << "Radio tune failed:" << ret;
}

void FMRadioHalControl::handleConfig(int band, bool stereo)
{
    if (!m_tunerReady) {
        qCDebug(log) << "Initial tuner config received.";
        m_tunerReady = true;
        setError(QRadioTuner::NoError);
        setTuning();
        emit stateChanged(QRadioTuner::ActiveState);
    }

    if (static_cast<radio_band_t>(band) == RADIO_BAND_FM)
        setStereoEnabled(stereo);
}

void FMRadioHalControl::handleAntenna(bool connected)
{
    if (connected != m_antennaConnected) {
        m_antennaConnected = connected;
        qCDebug(log) << "Antenna changes to " << (m_antennaConnected ? "connected" : "disconnected");
        emit antennaConnectedChanged(m_antennaConnected);
    }
}

bool FMRadioHalControl::tunedSearchAll(unsigned channel)
{
    unsigned channelRelative = channel - m_hal->config.lower_limit;
    int reduce;

    if (m_firstFoundFrequency > 0) {
        if (m_searchMode == QRadioTuner::SearchFast) {
            qCDebug(log) << "SearchFast found channel" << channel;
            emit stationFound(FREQ_HAL_TO_QT(channel), m_stationId);
        }
    } else
        m_firstFoundFrequency = channel;

    if (channelRelative >= m_lastFrequency)
        reduce = channelRelative - m_lastFrequency;
    else
        reduce = m_hal->config.upper_limit - m_hal->config.lower_limit - m_lastFrequency
               + channelRelative;

    m_searchRange -= reduce;
    m_lastFrequency = channel - m_hal->config.lower_limit;

    if (m_searchMode == QRadioTuner::SearchFast) {
        if (m_searchRange > 0) {
            searchForward();
            return true;
        }
    } else {
        if (m_searchRange > 0) {
            qCDebug(log) << "SearchGetStationId channel" << channel << "tuned, wait for RDS.";
            return true;
        }
    }

    if (m_firstFoundFrequency != channel) {
        if (m_searchMode == QRadioTuner::SearchFast) {
            qCDebug(log) << "SearchFast found channel" << m_firstFoundFrequency;
            emit stationFound(FREQ_HAL_TO_QT(m_firstFoundFrequency), m_stationId);
        } else {
            qCDebug(log) << "SearchGetStationId channel" << channel << "tuned, wait for RDS.";
            m_searchAllLast = true;
            return true;
        }
    }

    m_searchAllLast = true;

    return false;
}

void FMRadioHalControl::handleTuned(unsigned channel, bool stereo)
{
    m_currentFreq = channel;

    if (!m_searchAllLast && m_searchAll) {
        if (tunedSearchAll(channel))
            return;
    }

    if (m_searchAllLast) {
        m_searchAllLast = false;
        m_searchAll = false;
        m_searchWaitForRDS = false;
        setSearching(false);
        qCDebug(log) << "Search done.";
    }

    qCDebug(log) << "Tuned channel" << m_currentFreq << (stereo ? "stereo" : "mono");
    emit frequencyChanged(FREQ_HAL_TO_QT(m_currentFreq));

    setSearching(false);

    setStereoEnabled(stereo);
}

// Called in radio event callback thread
void FMRadioHalControl::handleMetadata(const radio_hal_event_t *event)
{
    if (!event->metadata || m_rdsError != QRadioData::NoError)
        return;

    int ret;
    if ((ret = m_hal->metadata_check(event->metadata)) != 0) {
        qCDebug(log) << "Radio metadata consistency check failed:" << ret;
        return;
    }

    unsigned count = m_hal->metadata_get_count(event->metadata);
    if (count == 0)
        return;

    for (unsigned i = 0; i < count; ++i) {
        radio_metadata_key_t key;
        radio_metadata_type_t dataType;
        void *value;
        unsigned size;

        if ((ret = m_hal->metadata_get_at_index(event->metadata, i, &key, &dataType, &value, &size)) != 0) {
            qCDebug(log) << "Failed to get metadata from index" << i << ":" << ret;
            return;
        }

        switch (dataType) {
            case RADIO_METADATA_TYPE_TEXT: {
                static const QRegExp regExp("[^a-zA-Z0-9 -_,;.:!#%&/()=?@£$+]");
                QString str = QString::fromUtf8(reinterpret_cast<const char*>(value));
                qCDebug(log) << "Raw data for key" << key << ":" << str;
                str = str.remove(regExp).trimmed();

                switch (key) {
                    case RADIO_METADATA_KEY_RDS_PI:
                        qCDebug(log) << "RDS_PI:" << str;
                        if (m_stationId != str) {
                            m_stationId = str;
                            if (m_searchWaitForRDS) {
                                qCDebug(log) << "SearchGetStationId found channel" << m_currentFreq << ":" << m_stationId;
                                emit stationFound(FREQ_HAL_TO_QT(m_currentFreq), m_stationId);
                                emit seekNextChannel();
                            } else
                                emit stationIdChanged(m_stationId);
                        }
                        break;

                    case RADIO_METADATA_KEY_RDS_PS:
                        qCDebug(log) << "RDS_PS:" << str;
                        if (m_stationName != str) {
                            m_stationName = str;
                            emit stationNameChanged(m_stationName);
                        }
                        break;

                    case RADIO_METADATA_KEY_TITLE:
                        qCDebug(log) << "TITLE:" << str;
                        if (m_radioText != str) {
                            m_radioText = str;
                            emit radioTextChanged(m_radioText);
                        }
                        break;
                }
                break;
            }

            case RADIO_METADATA_TYPE_INT: {
                const unsigned int *integer = reinterpret_cast<const unsigned int*>(value);

                switch (key) {
                    case RADIO_METADATA_KEY_RDS_PTY:
                        if (m_programType != *integer) {
                            qCDebug(log) << "RDS_PTY:" << *integer;
                            m_programType = *integer;
                            emit programTypeChanged(programTypeValue(0, m_programType));
                            emit programTypeNameChanged(programTypeNameString(0, m_programType));
                        }
                        break;

                    case RADIO_METADATA_KEY_RBDS_PTY:
                        if (m_programType != *integer) {
                            qCDebug(log) << "RBDS_PTY:" << *integer;
                            m_programType = *integer;
                            emit programTypeChanged(programTypeValue(1, m_programType));
                            emit programTypeNameChanged(programTypeNameString(1, m_programType));
                        }
                        break;
                }
                break;
            }
        }
    }
}

void FMRadioHalControl::handleTA(bool enabled)
{
    qCDebug(log) << "Radio TA changes to " << (enabled ? "true" : "false");
}

// Alternative Frequency switch
void FMRadioHalControl::handleAFSwitch(bool /* unused */)
{
    qCDebug(log) << "Radio AF switch";
}

void FMRadioHalControl::handleEA(bool enabled)
{
    qCDebug(log) << "Radio EA changes to " << (enabled ? "true" : "false");
}

// Called in radio event callback thread
void FMRadioHalControl::radioEvent(const radio_hal_event_t *event)
{
    switch (event->type) {
        case RADIO_EVENT_HW_FAILURE:
            emit eventHwFailure();
            break;

        case RADIO_EVENT_CONFIG:
            emit eventConfig(static_cast<int>(event->config.type), event->config.fm.stereo);
            break;

        case RADIO_EVENT_ANTENNA:
            emit eventAntenna(event->on);
            break;

        case RADIO_EVENT_TUNED:
            emit eventTuned(event->info.channel, event->info.stereo);
            break;

        case RADIO_EVENT_METADATA:
            handleMetadata(event);
            break;

        case RADIO_EVENT_TA:
            emit eventTA(event->on);
            break;

        case RADIO_EVENT_AF_SWITCH:
            emit eventAFSwitch(event->on);
            break;

#ifdef SUPPORT_RADIO_EVENT_EA
        case RADIO_EVENT_EA:
            emit eventEA(event->on);
            break;
#endif

        // framework internal events
        default: break;
    };
}

// Called in radio event callback thread
void FMRadioHalControl::radioEventCallback(radio_hal_event_t *event, void *cookie)
{
    static_cast<FMRadioHalControl*>(cookie)->radioEvent(event);
}

void FMRadioHalControl::start()
{
    if (!m_hal || !m_hal->radiohw || m_hal->tuner)
        return;

    m_searching = false;
    m_searchAll = false;
    m_searchAllLast = false;

    int ret = m_hal->radiohw->open_tuner(m_hal->radiohw, &m_hal->config, true,
                                         &FMRadioHalControl::radioEventCallback, this,
                                         &m_hal->tuner);

    if (ret == 0)
        qCDebug(log) << "Tuner opened.";
    else
        qCCritical(log) << "Failed to open tuner:" << ret;
}

void FMRadioHalControl::stop()
{
    if (!m_hal || !m_hal->radiohw || !m_hal->tuner)
        return;

    cancelSearch();

    m_tunerReady = false;

    int ret = m_hal->radiohw->close_tuner(m_hal->radiohw, m_hal->tuner);
    m_hal->tuner = 0;

    if (ret == 0)
        qCDebug(log) << "Tuner closed.";
    else
        qCWarning(log) << "Error when closing tuner:" << ret;

    emit stateChanged(QRadioTuner::StoppedState);
}

void FMRadioHalControl::setError(QRadioTuner::Error newError)
{
    if (newError != m_error) {
        m_error = newError;
        emit error(m_error);
    }
}

void FMRadioHalControl::setRdsError(QRadioData::Error newError)
{
    if (newError != m_rdsError) {
        m_rdsError = newError;
        emit error(m_rdsError);
    }
}

QRadioTuner::Error FMRadioHalControl::tunerError() const
{
    return m_error;
}

QString FMRadioHalControl::tunerErrorString() const
{
    switch (m_error) {
        case QRadioTuner::NoError:          return QStringLiteral();
        case QRadioTuner::ResourceError:    return QStringLiteral("Resources not available.");
        case QRadioTuner::OpenError:        return QStringLiteral("Failed to open tuner.");
        case QRadioTuner::OutOfRangeError:  return QStringLiteral("Out of range.");
    }

    return QStringLiteral("Unknown error.");
}

QMultimedia::AvailabilityStatus FMRadioHalControl::rdsAvailability() const
{
    return isRdsAvailable() ? QMultimedia::Available : QMultimedia::ServiceMissing;
}

QString FMRadioHalControl::stationId() const
{
    return m_stationId;
}

QRadioData::ProgramType FMRadioHalControl::programType() const
{
    if (m_hal && m_hal->tuner && m_hal->config.type == RADIO_BAND_FM) {
        switch (m_hal->config.fm.rds) {
            case RADIO_RDS_WORLD: return programTypeValue(0, m_programType);
            case RADIO_RDS_US:    return programTypeValue(1, m_programType);
        }
    }

    return QRadioData::Undefined;
}

QString FMRadioHalControl::programTypeName() const
{
    if (m_hal && m_hal->tuner && m_hal->config.type == RADIO_BAND_FM) {
        switch (m_hal->config.fm.rds) {
            case RADIO_RDS_WORLD: return programTypeNameString(0, m_programType);
            case RADIO_RDS_US:    return programTypeNameString(1, m_programType);
        }
    }

    // Undefined
    return programTypeNameString(0, 0);
}

QString FMRadioHalControl::stationName() const
{
    return m_stationName;
}

QString FMRadioHalControl::radioText() const
{
    return m_radioText;
}

void FMRadioHalControl::setAlternativeFrequenciesEnabled(bool /* unused */)
{
}

bool FMRadioHalControl::isAlternativeFrequenciesEnabled() const
{
    return false;
}

QRadioData::Error FMRadioHalControl::rdsError() const
{
    return m_rdsError;
}

QString FMRadioHalControl::rdsErrorString() const
{
    switch (m_rdsError) {
        case QRadioData::NoError:           return QStringLiteral();
        case QRadioData::ResourceError:     return QStringLiteral("Resources not available.");
        case QRadioData::OpenError:         return QStringLiteral("Failed to open RDS.");
        default: break;
    }

    return QStringLiteral("Unknown error.");
}

QRadioData::ProgramType FMRadioHalControl::programTypeValue(int rdsStandard, unsigned int type) const
{
    static const unsigned int rbdsTypes[] = {
        0,
        1,
        3,
        4,
        32,
        11,
        33,
        34,
        35,
        36,
        25,
        27,
        37,
        38,
        24,
        39,
        40,
        41,
        42,
        43,
        44,
        45,
        46,
        47,
        0,
        0,
        0,
        0,
        0,
        16,
        30,
        31
      };

    if (type >= 32)
        type = 0;

    if (rdsStandard == 0)
        return static_cast<QRadioData::ProgramType>(type);

    return static_cast<QRadioData::ProgramType>(rbdsTypes[type]);
}

QString FMRadioHalControl::programTypeNameString(int rdsStandard, unsigned int type) const
{
    static const char *rbdsTypes[] = {
        QT_TR_NOOP("No program type or undefined"),
        QT_TR_NOOP("News"),
        QT_TR_NOOP("Information"),
        QT_TR_NOOP("Sports"),
        QT_TR_NOOP("Talk"),
        QT_TR_NOOP("Rock"),
        QT_TR_NOOP("Classic rock"),
        QT_TR_NOOP("Adult hits"),
        QT_TR_NOOP("Soft rock"),
        QT_TR_NOOP("Top 40"),
        QT_TR_NOOP("Country"),
        QT_TR_NOOP("Oldies"),
        QT_TR_NOOP("Soft"),
        QT_TR_NOOP("Nostalgia"),
        QT_TR_NOOP("Jazz"),
        QT_TR_NOOP("Classical"),
        QT_TR_NOOP("Rhythm and blues"),
        QT_TR_NOOP("Soft rhythm and blues"),
        QT_TR_NOOP("Language"),
        QT_TR_NOOP("Religious music"),
        QT_TR_NOOP("Religious talk"),
        QT_TR_NOOP("Personality"),
        QT_TR_NOOP("Public"),
        QT_TR_NOOP("College"),
        QT_TR_NOOP("Spanish Talk"),
        QT_TR_NOOP("Spanish Music"),
        QT_TR_NOOP("Hip Hop"),
        QT_TR_NOOP("Unassigned"),
        QT_TR_NOOP("Unassigned"),
        QT_TR_NOOP("Weather"),
        QT_TR_NOOP("Emergency test"),
        QT_TR_NOOP("Emergency")
      };

    static const char *rdsTypes[] = {
        QT_TR_NOOP("No programme type or undefined"),
        QT_TR_NOOP("News"),
        QT_TR_NOOP("Current affairs"),
        QT_TR_NOOP("Information"),
        QT_TR_NOOP("Sport"),
        QT_TR_NOOP("Education"),
        QT_TR_NOOP("Drama"),
        QT_TR_NOOP("Culture"),
        QT_TR_NOOP("Science"),
        QT_TR_NOOP("Varied"),
        QT_TR_NOOP("Pop music"),
        QT_TR_NOOP("Rock music"),
        QT_TR_NOOP("Easy listening"),
        QT_TR_NOOP("Light classical"),
        QT_TR_NOOP("Serious classical"),
        QT_TR_NOOP("Other music"),
        QT_TR_NOOP("Weather"),
        QT_TR_NOOP("Finance"),
        QT_TR_NOOP("Children’s programmes"),
        QT_TR_NOOP("Social affairs"),
        QT_TR_NOOP("Religion"),
        QT_TR_NOOP("Phone-in"),
        QT_TR_NOOP("Travel"),
        QT_TR_NOOP("Leisure"),
        QT_TR_NOOP("Jazz music"),
        QT_TR_NOOP("Country music"),
        QT_TR_NOOP("National music"),
        QT_TR_NOOP("Oldies music"),
        QT_TR_NOOP("Folk music"),
        QT_TR_NOOP("Documentary"),
        QT_TR_NOOP("Alarm test"),
        QT_TR_NOOP("Alarm")
    };

    if (type >= 32)
        type = 0;

    if (rdsStandard == 0)
        return tr(rdsTypes[type]);
    else
        return tr(rbdsTypes[type]);
}
