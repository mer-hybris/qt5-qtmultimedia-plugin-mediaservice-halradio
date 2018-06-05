/*
  Copyright (C) 2018 Jolla Ltd
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

#ifndef __FMRADIOHALCONTROL_H
#define __FMRADIOHALCONTROL_H

#include <QRadioTuner>
#include <QRadioData>
#include <QTimer>
#include <QThread>
#include <QAtomicInt>
#include <QObject>
#include <QList>
#include <QLoggingCategory>

#include <system/radio.h>

typedef struct HalPrivate HalPrivate;

class FMRadioHalControl : public QObject
{
    Q_OBJECT
public:
    FMRadioHalControl();
    ~FMRadioHalControl();

    QRadioTuner::State tunerState() const;

    QRadioTuner::Band band() const;
    void setBand(QRadioTuner::Band b);
    bool isBandSupported(QRadioTuner::Band b) const;

    int frequency() const;
    void setFrequency(int frequency);

    bool isStereo() const;
    QRadioTuner::StereoMode stereoMode() const;
    void setStereoMode(QRadioTuner::StereoMode mode);

    int signalStrength() const;

    int volume() const;
    void setVolume(int volume);

    bool isMuted() const;
    void setMuted(bool m_muted);

    bool isSearching() const;

    bool isAntennaConnected() const;

    void searchBackward();
    void searchAllStations(QRadioTuner::SearchMode m_searchMode = QRadioTuner::SearchFast);
    void cancelSearch();

    void start();
    void stop();

    QRadioTuner::Error tunerError() const;
    QString tunerErrorString() const;

    bool isRdsAvailable() const;
    QMultimedia::AvailabilityStatus rdsAvailability() const;

    QString stationId() const;
    QRadioData::ProgramType programType() const;
    QString programTypeName() const;
    QString stationName() const;
    QString radioText() const;
    void setAlternativeFrequenciesEnabled(bool enabled);
    bool isAlternativeFrequenciesEnabled() const;

    QRadioData::Error rdsError() const;
    QString rdsErrorString() const;

public slots:
    void searchForward();

signals:
    void stateChanged(QRadioTuner::State state);
    void bandChanged(QRadioTuner::Band band);
    void frequencyChanged(int frequency);
    void stereoStatusChanged(bool m_stereo);
    void searchingChanged(bool searching);
    void signalStrengthChanged(int signalStrength);
    void volumeChanged(int volume);
    void mutedChanged(bool m_muted);
    void error(QRadioTuner::Error err);
    void stationFound(int frequency, QString stationId);
    void antennaConnectedChanged(bool connectionStatus);

    void stationIdChanged(QString stationId);
    void programTypeChanged(QRadioData::ProgramType programType);
    void programTypeNameChanged(QString programTypeName);
    void stationNameChanged(QString stationName);
    void radioTextChanged(QString radioText);
    void alternativeFrequenciesEnabledChanged(bool enabled);
    void error(QRadioData::Error err);

    // Signals used internally
    void eventHwFailure();
    void eventConfig(int band, bool stereo);
    void eventAntenna(bool connected);
    void eventTuned(unsigned channel, bool stereo);
    void eventTA(bool enabled);
    void eventAFSwitch(bool enabled);
#ifdef SUPPORT_RADIO_EVENT_EA
    void eventEA(bool enabled);
#endif
    void seekNextChannel();

private slots:
    void handleSeekTimeout();

    void handleHwFailure();
    void handleConfig(int band, bool stereo);
    void handleAntenna(bool connected);
    void handleTuned(unsigned channel, bool stereo);
    void handleTA(bool enabled);
    void handleAFSwitch(bool enabled);
#ifdef SUPPORT_RADIO_EVENT_EA
    void handleEA(bool enabled);
#endif

private:
    void openRadio();
    bool setRadioConfig(radio_band_t band, radio_deemphasis_t deemphasis);
    void setRadioConfigFallback();
    void closeRadio();
    void openRadioMetadata();
    void setTuning();
    void radioEvent(const radio_hal_event_t *event);
    static void radioEventCallback(radio_hal_event_t *event, void *cookie);
    void handleMetadata(const radio_hal_event_t *event);
    bool tunedSearchAll(unsigned channel);
    bool tunerEnabled() const;
    void seek(radio_direction_t direction);
    void resetRDS();
    void setSearching(bool searching);
    void setStereoEnabled(bool enabled);
    QRadioData::ProgramType programTypeValue(int rdsStandard, unsigned int type) const;
    QString programTypeNameString(int rdsStandard, unsigned int type) const;

    void setError(QRadioTuner::Error error);
    void setRdsError(QRadioData::Error error);

    QLoggingCategory m_log;
    HalPrivate *m_hal;
    QRadioTuner::Error m_error;
    QRadioData::Error m_rdsError;
    bool m_tunerReady;
    bool m_antennaConnected;
    bool m_stereoEnabled;
    unsigned m_currentFreq;

    QRadioTuner::SearchMode m_searchMode;
    QTimer *m_seekTimer;
    bool m_searching;
    bool m_searchAll;
    bool m_searchAllLast;
    bool m_searchWaitForRDS;
    unsigned m_firstFoundFrequency;
    int m_searchRange;
    unsigned m_lastFrequency;

    QString m_stationId;
    QString m_stationName;
    unsigned m_programType;
    QString m_radioText;
};


#endif
