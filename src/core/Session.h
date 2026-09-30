// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppSettings.h"
#include "BandLine.h"
#include "BandPlan.h"
#include "Station.h"
#include "UpdateCheck.h"

#include <QObject>
#include <QStringList>

class KiwiDirectory;
class QNetworkAccessManager;
class QTimer;
class RigClient;
class RigctldLauncher;
class StationDb;
class Updater;

// One listening session, without a screen: the database and the schedule
// sources, the rig and the rigctld started for it, the version check, the
// settings, and the rules of tuning: what the tuned frequency is, when a
// station goes to the rig and when to the display alone, and what the list
// shows. A window (or a phone screen) draws what it says and hands it what
// the listener does.
class Session : public QObject
{
    Q_OBJECT
public:
    explicit Session(const QString& dataDir, QObject* parent = nullptr);
    ~Session() override;

    bool isOpen() const { return m_open; }   // the database; lastError() says why not
    QString lastError() const;
    QString dataDir() const { return m_dataDir; }

    StationDb* db() const { return m_db; }
    Updater* updater() const { return m_updater; }
    RigClient* rig() const { return m_rig; }
    RigctldLauncher* launcher() const { return m_launcher; }
    UpdateCheck* updateCheck() const { return m_updateCheck; }
    KiwiDirectory* kiwiDirectory() const { return m_kiwiDirectory; }
    QNetworkAccessManager* network() const { return m_nam; }

    // --- settings ------------------------------------------------------
    AppSettings& settings() { return m_settings; }
    const AppSettings& settings() const { return m_settings; }
    // What OK in Settings does: the settings are replaced whole, saved, and
    // whatever they steer is told (the rig's address, the sources, rigctld,
    // a download when one is due, the version check when it was switched).
    void acceptSettings(const AppSettings& updated);
    void saveSettings();       // now; a pending save is dropped
    void saveSettingsSoon();   // once things settle, not on every step

    // --- start -----------------------------------------------------------
    // Connects to the rig, starts rigctld when wanted, downloads schedules
    // that are stale, schedules the version checks. startKHz > 0: manual
    // for this session, the stored "Follow rig" untouched.
    void start(double startKHz = 0.0);
    // Before the window goes: nothing said by the rig reaches anyone any more.
    void stop();

    // --- tuning ----------------------------------------------------------
    enum class Origin
    {
        Rig,          // the rig moved
        Manual,       // typed, keyed, wheeled, or a station without a rig
        Remembered    // no rig ever answered: the frequency of the last time
    };
    Q_ENUM(Origin)
    double centreKHz() const { return m_centreKHz; }
    bool followRig() const { return m_followRig; }
    // remember: the stored preference follows (not for -f at start)
    void setFollowRig(bool follow, bool remember = true);
    bool rigAnswering() const { return m_rigAnswering; }
    qint64 rigHz() const { return m_rigHz; }
    QString rigMode() const { return m_rigMode; }
    // typed, keys, wheel: the display only, remembered for the next start
    void setManualKHz(double kHz);
    enum class Tuned { Rig, Manual };
    Q_ENUM(Tuned)
    // A station from the list goes to the rig when it is followed and
    // answers (mode first). Otherwise to the display: Follow rig goes off,
    // the station's mode is offered to the online receiver.
    Tuned tuneTo(double kHz, const QString& mode);
    QString frequencyText() const;   // "7 125.000 kHz", "---.--- kHz" before anything is tuned
    QString modeText() const;        // the rig's mode, "no rig", or "manual"
    BandLine bandLine() const;       // the allocation of the tuned frequency
    QString rigSilentText() const;   // "Rig not answering (host:port)"

    // --- the list --------------------------------------------------------
    enum class Lookup
    {
        None,     // nothing tuned yet
        Dial,     // a thousand entries either side of the tuned frequency
        Search    // the whole database against the search text
    };
    Q_ENUM(Lookup)
    struct Rows
    {
        Lookup kind = Lookup::None;
        StationList list;
        bool same = false;   // the search on screen again, only the VFO moved: keep the list
    };
    // searchOnScreen: the list shows the last search (not the dial), so an
    // unchanged search need not be run again
    Rows rows(const QString& searchText, bool searchOnScreen);
    void forgetSearch();   // the data changed: the next rows() searches again
    QStringList disabledSources() const;   // downloadable sources switched off
    // "EiBi B26: 12345  |  HFCC B26: 6789", or that no source is enabled;
    // the tooltip tells when each was checked
    QString sourcesStatus(QString* tooltip = nullptr) const;

    // --- downloads and the version check ---------------------------------
    void updateDatabases();        // every enabled source, now
    void startUpdateCheck();       // quietly; nothing when switched off
    void checkForNewVersionNow();  // asked by hand: the answer is spoken out

signals:
    void centreChanged(double kHz, Session::Origin origin);
    void followRigChanged(bool follow);
    void rigModeChanged(const QString& mode);
    void rigAnsweringChanged(bool answering);
    void rigStateChanged(bool connected, const QString& message);
    void manualModeChosen(const QString& mode);   // a station tuned without the rig
    void message(const QString& text, int timeoutMs);   // for the status bar; 0: until replaced
    void settingsApplied();   // after acceptSettings, before any download it starts
    void updateStarted();
    void updateFinished(bool ok, const QString& summary, bool databaseEmpty);
    void sourcesChanged();    // a source finished: counts and dates moved
    void versionChecked(const UpdateCheck::Result& result);   // invalid: nothing to show
    void launcherFailed(const QString& error);

private:
    void applySettings();   // the rig's address and the sources, from the settings
    void applyLauncher();
    void setCentre(double kHz, Origin origin);
    void onRigFrequency(qint64 hz);
    void onRigAnswering(bool answering);
    void onUpdateFinished(bool ok, const QString& summary);
    void onVersionChecked(const UpdateCheck::Result& r);

    QString m_dataDir;
    bool m_open = false;
    AppSettings m_settings;
    StationDb* m_db = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    Updater* m_updater = nullptr;
    UpdateCheck* m_updateCheck = nullptr;
    RigClient* m_rig = nullptr;
    RigctldLauncher* m_launcher = nullptr;
    KiwiDirectory* m_kiwiDirectory = nullptr;
    QTimer* m_saveTimer = nullptr;
    BandPlan m_bandPlan;

    double m_centreKHz = 0.0;
    bool m_followRig = true;
    qint64 m_rigHz = 0;
    QString m_rigMode;
    bool m_rigAnswering = false;
    QString m_lastSearchKey;   // search text and sources of the list on screen
    bool m_manualCheck = false;
};
