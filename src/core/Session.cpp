// SPDX-License-Identifier: GPL-3.0-or-later
#include "Session.h"
#include "AokiSource.h"
#include "EibiSource.h"
#include "Format.h"
#include "HfccSource.h"
#include "KiwiDirectory.h"
#include "RigClient.h"
#include "RigctldLauncher.h"
#include "StationDb.h"
#include "TraficomSource.h"
#include "Updater.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QTimer>

Session::Session(const QString& dataDir, QObject* parent)
    : QObject(parent)
    , m_dataDir(dataDir)
{
    m_db = new StationDb(m_dataDir + QStringLiteral("/stations.db"), this);
    m_open = m_db->open();
    m_settings.load(m_db);
    m_followRig = m_settings.followRig;

    m_nam = new QNetworkAccessManager(this);
    m_updater = new Updater(this);
    m_updater->addSource(new EibiSource(m_db, m_nam, this));
    m_updater->addSource(new HfccSource(m_db, m_nam, this));
    m_updater->addSource(new AokiSource(m_db, m_nam, this));
    m_updater->addSource(new TraficomSource(m_db, m_nam, this));
    m_updateCheck = new UpdateCheck(m_nam, this);
    connect(m_updateCheck, &UpdateCheck::finished, this, &Session::onVersionChecked);
    m_rig = new RigClient(this);
    m_launcher = new RigctldLauncher(this);
    connect(m_launcher, &RigctldLauncher::started, this, [this]() {
        emit message(tr("rigctld started"), 5000);
        m_rig->reconnectSoon();
    });
    connect(m_launcher, &RigctldLauncher::stopped, this, [this](const QString& msg) {
        emit message(msg, 15000);
    });
    m_kiwiDirectory = new KiwiDirectory(m_db, m_nam, this);
    connect(m_kiwiDirectory, &KiwiDirectory::failed, this, [this](const QString& why) {
        emit message(tr("KiwiSDR directory not available: %1").arg(why), 8000);
    });
    m_bandPlan = BandPlan::builtIn();

    connect(m_rig, &RigClient::frequencyChanged, this, &Session::onRigFrequency);
    connect(m_rig, &RigClient::modeChanged, this, [this](const QString& mode, int) {
        m_rigMode = mode;
        emit rigModeChanged(mode);
    });
    connect(m_rig, &RigClient::stateChanged, this, &Session::rigStateChanged);
    connect(m_rig, &RigClient::answeringChanged, this, &Session::onRigAnswering);
    connect(m_updater, &Updater::progress, this, [this](const QString& m) { emit message(m, 0); });
    connect(m_updater, &Updater::sourceFinished, this,
            [this](ScheduleSource*, bool, const QString&) { emit sourcesChanged(); });
    connect(m_updater, &Updater::finished, this, &Session::onUpdateFinished);

    // Values that change in quick succession (tuning by hand, the zoom, the
    // highlight range) are written once things settle, not on every step.
    m_saveTimer = new QTimer(this);
    m_saveTimer->setObjectName(QStringLiteral("saveTimer"));
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(1000);
    connect(m_saveTimer, &QTimer::timeout, this, [this]() { m_settings.save(m_db); });

    applySettings();
}

Session::~Session()
{
    stop();
}

QString Session::lastError() const
{
    return tr("Cannot open %1:\n%2").arg(m_db->filePath(), m_db->lastError());
}

void Session::stop()
{
    // The socket's destructor emits disconnected(); by then whoever listens
    // to the rig state may be gone, so cut the connections first.
    m_rig->disconnect();
    m_rig->stop();
    m_launcher->stop();
}

// --- settings ------------------------------------------------------------

void Session::applySettings()
{
    m_rig->setEndpoint(m_settings.rigHost, quint16(m_settings.rigPort));
    m_rig->setPollInterval(m_settings.pollIntervalMs);
    const struct
    {
        const char* id;
        const QString& url;
        bool enabled;
    } sources[] = {{"eibi", m_settings.eibiUrl, m_settings.eibiEnabled},
                   {"hfcc", m_settings.hfccUrl, m_settings.hfccEnabled},
                   {"aoki", m_settings.aokiUrl, m_settings.aokiEnabled},
                   {"traficom", m_settings.traficomUrl, m_settings.traficomEnabled}};
    for (const auto& s : sources)
        if (ScheduleSource* src = m_updater->source(QLatin1String(s.id)))
        {
            src->setBaseUrl(QUrl(s.url));
            src->setEnabled(s.enabled);
        }
}

void Session::applyLauncher()
{
    if (!m_settings.launchRigctld)
    {
        m_launcher->stop();
        return;
    }
    RigctldLauncher::Config cfg;
    cfg.enabled = true;
    cfg.path = m_settings.rigctldPath;
    cfg.model = m_settings.rigModel;
    cfg.device = m_settings.rigDevice;
    cfg.baud = m_settings.rigBaud;
    cfg.extraArgs = m_settings.rigctldExtra;
    cfg.port = quint16(m_settings.rigPort);
    if (!m_launcher->start(cfg))
        emit launcherFailed(m_launcher->lastError());
}

void Session::acceptSettings(const AppSettings& updated)
{
    // The dialog starts from the current settings and changes only what it
    // shows, so its result replaces them whole: no field can be forgotten.
    const AppSettings old = m_settings;
    m_settings = updated;
    const bool updateChanged = m_settings.updateCheck != old.updateCheck
                               || m_settings.updateUrl != old.updateUrl;
    const bool launcherChanged =
        m_settings.launchRigctld != old.launchRigctld || m_settings.rigctldPath != old.rigctldPath
        || m_settings.rigModel != old.rigModel || m_settings.rigDevice != old.rigDevice
        || m_settings.rigBaud != old.rigBaud || m_settings.rigctldExtra != old.rigctldExtra
        || m_settings.rigPort != old.rigPort;
    m_settings.save(m_db);
    applySettings();
    if (launcherChanged)
        applyLauncher();
    m_lastSearchKey.clear();   // the sources may have changed: search again
    emit settingsApplied();
    if (m_updater->anyStale(m_settings.refreshDays))
        updateDatabases();
    if (updateChanged)
        startUpdateCheck();
}

void Session::saveSettings()
{
    m_settings.save(m_db);
    m_saveTimer->stop();   // saved just now; nothing is left to write later
}

void Session::saveSettingsSoon()
{
    m_saveTimer->start();
}

// --- start ---------------------------------------------------------------

void Session::start(double startKHz)
{
    if (startKHz > 0.0)
    {
        // Manual start for this session only; the stored "Follow rig"
        // preference is left untouched.
        setFollowRig(false, false);
        setCentre(startKHz, Origin::Manual);
    }
    else if (!m_followRig)
    {
        // no rig wanted: start where the listener left off (49 m at first)
        setCentre(m_settings.manualKHz, Origin::Manual);
    }

    if (m_updater->anyStale(m_settings.refreshDays))
        QTimer::singleShot(0, this, [this]() {
            emit updateStarted();
            m_updater->update(m_settings.refreshDays, false);
        });

    applyLauncher();
    m_rig->start();
    // ask once after start, then every 24 hours while the program runs
    QTimer::singleShot(3000, this, &Session::startUpdateCheck);
    auto* daily = new QTimer(this);
    daily->setInterval(24 * 60 * 60 * 1000);
    connect(daily, &QTimer::timeout, this, &Session::startUpdateCheck);
    daily->start();
}

// --- tuning --------------------------------------------------------------

void Session::setCentre(double kHz, Origin origin)
{
    if (origin == Origin::Rig && !m_followRig)
        return;
    m_centreKHz = kHz;
    if (origin == Origin::Manual && kHz > 0.0 && !qFuzzyCompare(kHz + 1.0, m_settings.manualKHz + 1.0))
    {
        m_settings.manualKHz = kHz;   // remembered for the next start without a rig
        saveSettingsSoon();           // once tuning pauses, not on every step
    }
    emit centreChanged(kHz, origin);
}

void Session::setManualKHz(double kHz)
{
    setCentre(kHz, Origin::Manual);
}

void Session::setFollowRig(bool follow, bool remember)
{
    if (remember)
    {
        m_settings.followRig = follow;
        m_settings.save(m_db);
    }
    m_followRig = follow;
    emit followRigChanged(follow);
    if (follow && m_rigHz > 0)
        setCentre(m_rigHz / 1000.0, Origin::Rig);
}

Session::Tuned Session::tuneTo(double kHz, const QString& mode)
{
    // Follow rig unticked means the listener has set the rig aside for now
    // (listening online, say): then a station goes to the display and the
    // online receiver, not to the rig, and the tick stays off.
    if (m_followRig && m_rig->isConnected() && m_rig->isAnswering())
    {
        // Send it to the radio and let the display follow the rig's answer.
        // Mode first: many rigs shift the dial when the mode changes (a CW or
        // SSB offset), so a frequency set before the mode would land off.
        if (RigClient::isRigMode(mode))
            m_rig->setMode(mode);
        m_rig->setFrequency(qRound64(kHz * 1000.0));
        emit message(tr("Tuning rig to %1 kHz %2").arg(kHz, 0, 'f', 3).arg(mode).trimmed(), 5000);
        return Tuned::Rig;
    }
    if (m_followRig)
        setFollowRig(false);   // no rig answering: manual mode
    // the station's mode for the online receiver, when it is one it has
    if (!mode.isEmpty())
        emit manualModeChosen(mode);
    setCentre(kHz, Origin::Manual);
    return Tuned::Manual;
}

void Session::onRigFrequency(qint64 hz)
{
    m_rigHz = hz;
    setCentre(hz / 1000.0, Origin::Rig);
}

void Session::onRigAnswering(bool answering)
{
    m_rigAnswering = answering;
    if (!answering)
    {
        m_rigHz = 0;   // nothing the rig said still holds
        if (m_centreKHz <= 0.0)
        {
            // nothing has ever come from a rig: show the remembered frequency
            // so a listener without a radio is not left with an empty dial
            m_centreKHz = m_settings.manualKHz;
            emit centreChanged(m_centreKHz, Origin::Remembered);
        }
    }
    emit rigAnsweringChanged(answering);
}

QString Session::frequencyText() const
{
    return m_centreKHz > 0.0 ? Format::kHz(m_centreKHz) + tr(" kHz") : QStringLiteral("---.--- kHz");
}

QString Session::modeText() const
{
    if (m_followRig)
        return m_rigAnswering ? m_rigMode : tr("no rig");
    return tr("manual");
}

BandLine Session::bandLine() const
{
    QList<StationDb::Allocation> national;
    if (m_settings.traficomEnabled && m_centreKHz > 0.0)
        national = m_db->allocationsAt(m_centreKHz);
    return BandLine::describe(m_bandPlan, m_settings.ituRegion, m_centreKHz, national);
}

QString Session::rigSilentText() const
{
    return tr("Rig not answering (%1:%2)").arg(m_settings.rigHost).arg(m_settings.rigPort);
}

// --- the list ------------------------------------------------------------

Session::Rows Session::rows(const QString& searchText, bool searchOnScreen)
{
    const QString text = searchText.trimmed();
    Rows r;
    if (!text.isEmpty())
    {
        r.kind = Lookup::Search;
        // the same search again, only the VFO moved: keep the list as it
        // is and just update the distances, so tuning to a row does not
        // reshuffle the results
        const QString key = text + QLatin1Char('\n') + disabledSources().join(QLatin1Char(','));
        if (key == m_lastSearchKey && searchOnScreen)
        {
            r.same = true;
            return r;
        }
        m_lastSearchKey = key;
        r.list = m_db->search(text, disabledSources());
        return r;
    }
    m_lastSearchKey.clear();
    if (m_centreKHz > 0.0)
    {
        r.kind = Lookup::Dial;
        r.list = m_db->around(m_centreKHz, 1000, disabledSources());
    }
    return r;
}

void Session::forgetSearch()
{
    m_lastSearchKey.clear();
}

QStringList Session::disabledSources() const
{
    // The sources to leave out: downloadable ones that are switched off.
    // Everything else in the database is shown, the personal list and any
    // list that was put there by other means included.
    QStringList disabled;
    for (ScheduleSource* src : m_updater->sources())
        if (!src->isEnabled())
            disabled << src->id();
    return disabled;
}

QString Session::sourcesStatus(QString* tooltip) const
{
    QStringList parts;
    QString tip;
    for (ScheduleSource* src : m_updater->sources())
    {
        // the status counts stations; the allocation table is not one
        if (!src->isEnabled() || src->id() == QLatin1String("traficom"))
            continue;
        const int n = src->count();
        if (n == 0)
        {
            parts << tr("%1: no data").arg(src->displayName());
            continue;
        }
        parts << (src->season().isEmpty()
                      ? QStringLiteral("%1: %2").arg(src->displayName()).arg(n)
                      : QStringLiteral("%1 %2: %3").arg(src->displayName(), src->season().toUpper()).arg(n));
        const QDateTime updated = src->lastUpdate().toLocalTime();
        tip += tr("%1 %2: %3 entries, checked %4\n")
                   .arg(src->displayName(), src->season().toUpper())
                   .arg(n)
                   .arg(updated.isValid() ? updated.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                          : tr("never"));
    }
    QStringList known;
    for (ScheduleSource* src : m_updater->sources())
        known << src->id();
    known << userSourceId();
    for (const auto& sc : m_db->sourceCounts())
        if (!known.contains(sc.first))
            parts << QStringLiteral("%1: %2").arg(sc.first.toUpper()).arg(sc.second);
    if (tooltip)
        *tooltip = tip.trimmed();
    return parts.isEmpty() ? tr("No sources enabled") : parts.join(QStringLiteral("  |  "));
}

// --- downloads and the version check -------------------------------------

void Session::updateDatabases()
{
    if (m_updater->isBusy())
        return;
    emit updateStarted();
    m_updater->update(m_settings.refreshDays, true);
}

void Session::onUpdateFinished(bool ok, const QString& summary)
{
    if (ok)
        m_lastSearchKey.clear();   // the data changed: search again
    emit updateFinished(ok, summary, m_db->count() == 0);
}

void Session::startUpdateCheck()
{
    if (!m_settings.updateCheck)
    {
        emit versionChecked(UpdateCheck::Result());   // nothing to show
        return;
    }
    m_updateCheck->run(QUrl(m_settings.updateUrl));
}

void Session::checkForNewVersionNow()
{
    if (!m_settings.updateCheck)
    {
        emit message(tr("Version check is switched off in Settings"), 5000);
        return;
    }
    emit message(tr("Checking for a new version ..."), 5000);
    m_manualCheck = true;   // this time the answer is spoken out
    startUpdateCheck();
}

void Session::onVersionChecked(const UpdateCheck::Result& r)
{
    if (m_manualCheck)
    {
        // asked for by hand: say what came back, also when there is nothing new
        m_manualCheck = false;
        if (!r.valid)
            emit message(tr("Could not reach otd.oh2gba.eu for the version check"), 10000);
        else if (r.newer)
            emit message(tr("Version %1 is available, see the link above the clock").arg(r.latest), 15000);
        else
            emit message(tr("You have the current version, %1")
                             .arg(QCoreApplication::applicationVersion()), 10000);
    }
    emit versionChecked(r);
}
