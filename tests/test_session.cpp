// SPDX-License-Identifier: GPL-3.0-or-later
// The Session without a window: the rules of tuning against a fake rigctld
// on this machine, what the list is given, the header's texts, and what
// OK in Settings sets in motion. No widgets, so these run in a moment,
// except where the rig's real silence timeout is waited for.
#include "core/AppSettings.h"
#include "core/BandLine.h"
#include "core/BandPlan.h"
#include "core/DialMarks.h"
#include "core/EibiParser.h"
#include "core/Format.h"
#include "core/RigClient.h"
#include "core/ScheduleSource.h"
#include "core/Session.h"
#include "core/StationDb.h"
#include "core/Updater.h"
#include "fakerigctld.h"

#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
// RigClient's silence timeout at the seeded poll interval: max(5 s, 3 polls)
const int kSilenceMs = 5000;
const double kStationKHz = 7130.0;

// Settings that keep the session off the internet: no downloads, no
// version check, no rigctld of its own. The rig is the fake on this machine.
bool seed(const QString& dir, quint16 rigPort, bool followRig = true)
{
    StationDb db(dir + QStringLiteral("/stations.db"));
    if (!db.open())
        return false;
    AppSettings s;
    s.eibiEnabled = false;
    s.hfccEnabled = false;
    s.aokiEnabled = false;
    s.traficomEnabled = false;
    s.updateCheck = false;
    s.showPlayer = false;
    s.launchRigctld = false;
    s.followRig = followRig;
    s.rigHost = QStringLiteral("127.0.0.1");
    s.rigPort = rigPort;
    s.pollIntervalMs = 100;
    s.save(&db);
    StationEntry e;
    e.source = userSourceId();
    e.kHz = kStationKHz;
    e.station = QStringLiteral("Test Radio");
    e.mode = QStringLiteral("AM");
    return db.insertEntry(e);
}

bool addStations(const QString& dir)
{
    StationDb db(dir + QStringLiteral("/stations.db"));
    if (!db.open())
        return false;
    for (const auto& [kHz, name] : QList<QPair<double, const char*>>{
             {7200, "Radio Alpha"}, {6100, "Radio Bravo"}, {9400, "Radio Charlie"}})
    {
        StationEntry e;
        e.source = userSourceId();
        e.kHz = kHz;
        e.station = QString::fromLatin1(name);
        if (!db.insertEntry(e))
            return false;
    }
    return true;
}

// the settings as the next start would find them
AppSettings stored(const QString& dir)
{
    StationDb db(dir + QStringLiteral("/stations.db"));
    AppSettings s;
    if (db.open())
        s.load(&db);
    return s;
}

// a port nobody listens on
quint16 deadPort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost);
    const quint16 port = s.serverPort();
    s.close();
    return port;
}

bool noTuningSent(const FakeRigctld& rig, int from)
{
    for (int i = from; i < rig.received.size(); ++i)
    {
        const QString& cmd = rig.received.at(i);
        if (cmd.startsWith(QLatin1String("F ")) || cmd.startsWith(QLatin1String("M ")))
            return false;
    }
    return true;
}
} // namespace

class TestSession : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QLocale::setDefault(QLocale::c());
    }

    void frequencyTextAndDigitStep()
    {
        QCOMPARE(Format::kHz(7125.94), QStringLiteral("7 125.940"));
        QCOMPARE(Format::kHz(198), QStringLiteral("198.000"));
        QCOMPARE(Format::kHz(12345.678), QStringLiteral("12 345.678"));
        const QString t = QStringLiteral("6 070.000 kHz");
        QCOMPARE(Format::digitStep(t, 0), 1000.0);
        QCOMPARE(Format::digitStep(t, 2), 100.0);
        QCOMPARE(Format::digitStep(t, 3), 10.0);
        QCOMPARE(Format::digitStep(t, 4), 1.0);
        QCOMPARE(Format::digitStep(t, 6), 0.1);
        QCOMPARE(Format::digitStep(t, 8), 0.001);
        QCOMPARE(Format::digitStep(t, 1), 0.0);    // the thousands space
        QCOMPARE(Format::digitStep(t, 5), 0.0);    // the point
        QCOMPARE(Format::digitStep(t, 10), 0.0);   // the unit
        QCOMPARE(Format::digitStep(t, -1), 0.0);
        QCOMPARE(Format::digitStep(t, 99), 0.0);
    }

    void bandLineFromPlanAndTable()
    {
        const BandPlan plan = BandPlan::builtIn();
        BandLine l = BandLine::describe(plan, 1, 6070, {});
        QCOMPARE(l.text, QStringLiteral("49 m broadcast"));
        QCOMPARE(l.kind, QStringLiteral("broadcast"));
        QVERIFY(l.tooltip.isEmpty());
        l = BandLine::describe(plan, 1, 4625, {});
        QVERIFY(l.text.isEmpty());
        QVERIFY(l.kind.isEmpty());
        l = BandLine::describe(plan, 1, 0, {});
        QVERIFY(l.text.isEmpty());

        StationDb::Allocation wide, narrow;
        wide.lowKHz = 5000;
        wide.highKHz = 7000;
        wide.service = QStringLiteral("FIXED");
        narrow.lowKHz = 5900;
        narrow.highKHz = 6200;
        narrow.service = QStringLiteral("BROADCASTING");
        narrow.usage = QStringLiteral("HF broadcasting");
        narrow.mode = QStringLiteral("AM");
        l = BandLine::describe(plan, 1, 6070, {narrow, wide});   // narrowest first, as the database gives them
        QCOMPARE(l.text, QStringLiteral("HF broadcasting (broadcasting) · FIXED"));
        QCOMPARE(l.kind, QStringLiteral("broadcast"));
        QCOMPARE(l.tooltip, QStringLiteral("Traficom allocation table:\n"
                                           "5900.000 - 6200.000 kHz: HF broadcasting [BROADCASTING], AM\n"
                                           "5000.000 - 7000.000 kHz: FIXED [FIXED]"));   // no usage: the service, and the service
        // the first row decides the kind; more than three rows are counted
        QList<StationDb::Allocation> many;
        for (int i = 0; i < 5; ++i)
        {
            StationDb::Allocation a;
            a.lowKHz = 6000;
            a.highKHz = 6100;
            a.service = QStringLiteral("SERVICE %1").arg(i / 2);   // 0, 0, 1, 1, 2: three distinct
            many << a;
        }
        l = BandLine::describe(plan, 1, 6070, many);
        QCOMPARE(l.text, QStringLiteral("SERVICE 0 · SERVICE 1 · SERVICE 2"));
        QCOMPARE(l.kind, QStringLiteral("other"));
        many[4].service = QStringLiteral("AMATEUR");
        many << many[4];
        many.last().service = QStringLiteral("MARITIME MOBILE");
        l = BandLine::describe(plan, 1, 6070, many);
        QCOMPARE(l.text, QStringLiteral("SERVICE 0 ± SERVICE 1 ± AMATEUR ± +1 more")
                             .replace(QStringLiteral("±"), QStringLiteral("·")));
    }

    void dialMarksGroupPerFrequency()
    {
        const QVector<DialMark> rows = {{6070, QStringLiteral("A"), 2, 1},
                                        {6070, QStringLiteral("B"), 0, 1},
                                        {6070, QStringLiteral("C"), 1, 1},
                                        {6075, QStringLiteral("D"), 2, 1}};
        const QVector<DialMark> marks = groupDialMarks(rows);
        QCOMPARE(marks.size(), 2);
        QCOMPARE(marks[0].kHz, 6070.0);
        QCOMPARE(marks[0].name, QStringLiteral("B"));   // on air names the frequency
        QCOMPARE(marks[0].rank, 0);
        QCOMPARE(marks[0].count, 3);
        QCOMPARE(marks[1].name, QStringLiteral("D"));
        QCOMPARE(marks[1].count, 1);
        QVERIFY(groupDialMarks({}).isEmpty());
    }

    void rigModes()
    {
        for (const char* m : {"AM", "USB", "LSB", "CW"})
            QVERIFY(RigClient::isRigMode(QLatin1String(m)));
        for (const char* m : {"", "DRM", "NFM", "am", "USB "})
            QVERIFY(!RigClient::isRigMode(QLatin1String(m)));
    }

    // Follow rig with a rig that answers: a station goes to the rig, the
    // mode first, and the display follows what the rig then says.
    void tuneGoesToTheRigWhenItAnswers()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        Session s(dir.path());
        QVERIFY(s.isOpen());
        QSignalSpy centres(&s, &Session::centreChanged);
        QSignalSpy messages(&s, &Session::message);
        QCOMPARE(s.frequencyText(), QStringLiteral("---.--- kHz"));
        QVERIFY(s.followRig());
        s.start();
        QTRY_VERIFY_WITH_TIMEOUT(s.rigAnswering(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(s.centreKHz(), 7125.0, 5000);
        QCOMPARE(s.frequencyText(), QStringLiteral("7 125.000 kHz"));
        QTRY_COMPARE_WITH_TIMEOUT(s.modeText(), QStringLiteral("USB"), 5000);
        QCOMPARE(centres.last().at(1).value<Session::Origin>(), Session::Origin::Rig);
        QCOMPARE(s.bandLine().text, QStringLiteral("40 m amateur"));

        QCOMPARE(s.tuneTo(kStationKHz, QStringLiteral("AM")), Session::Tuned::Rig);
        QTRY_COMPARE_WITH_TIMEOUT(rig.hz, qint64(7130000), 5000);
        const int m = rig.received.indexOf(QStringLiteral("M AM 0"));
        const int f = rig.received.indexOf(QStringLiteral("F 7130000"));
        QVERIFY2(m >= 0 && f > m, qPrintable(rig.received.join(QLatin1Char(' '))));
        QCOMPARE(messages.last().at(0).toString(), QStringLiteral("Tuning rig to 7130.000 kHz AM"));
        QVERIFY(s.followRig());
        QTRY_COMPARE_WITH_TIMEOUT(s.centreKHz(), 7130.0, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(s.modeText(), QStringLiteral("AM"), 5000);

        // a mode the rig has no name for is left to the rig
        const int before = rig.received.size();
        QCOMPARE(s.tuneTo(7140.0, QStringLiteral("DRM")), Session::Tuned::Rig);
        QTRY_COMPARE_WITH_TIMEOUT(rig.hz, qint64(7140000), 5000);
        for (int i = before; i < rig.received.size(); ++i)
            QVERIFY2(!rig.received.at(i).startsWith(QLatin1String("M ")), qPrintable(rig.received.at(i)));
    }

    // Follow rig off while the rig answers: a station goes to the display
    // only, its mode is offered to the online receiver, the rig is left
    // alone, and the frequency is remembered once tuning pauses.
    void tuneWithoutRigIsManual()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        Session s(dir.path());
        QSignalSpy modes(&s, &Session::manualModeChosen);
        QSignalSpy centres(&s, &Session::centreChanged);
        s.start();
        QCOMPARE(s.centreKHz(), 6070.0);   // the first start's default
        QCOMPARE(s.modeText(), QStringLiteral("manual"));
        QCOMPARE(centres.last().at(1).value<Session::Origin>(), Session::Origin::Manual);
        QTRY_VERIFY_WITH_TIMEOUT(s.rigAnswering(), 5000);   // there, but not followed
        QCOMPARE(s.centreKHz(), 6070.0);
        QCOMPARE(s.modeText(), QStringLiteral("manual"));

        const int before = rig.received.size();
        QCOMPARE(s.tuneTo(kStationKHz, QStringLiteral("AM")), Session::Tuned::Manual);
        QCOMPARE(s.centreKHz(), kStationKHz);
        QVERIFY(!s.followRig());
        QCOMPARE(modes.size(), 1);
        QCOMPARE(modes.last().at(0).toString(), QStringLiteral("AM"));
        QTest::qWait(500);   // a command on its way would have arrived by now
        QVERIFY(noTuningSent(rig, before));
        QCOMPARE(rig.hz, qint64(7125000));
        QCOMPARE(stored(dir.path()).manualKHz, 6070.0);   // not yet
        QTRY_COMPARE_WITH_TIMEOUT(stored(dir.path()).manualKHz, kStationKHz, 3000);

        s.tuneTo(7135.0, QString());   // no mode: nothing offered
        QCOMPARE(modes.size(), 1);
        QCOMPARE(s.centreKHz(), 7135.0);
    }

    // Follow rig with a rig that has gone silent: a station tunes by hand,
    // Follow rig goes off and is stored so. Ticked again while the rig is
    // still silent, the dial stays: nothing the rig said still holds.
    void tuneWithSilentRigDropsFollow()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        Session s(dir.path());
        QSignalSpy follows(&s, &Session::followRigChanged);
        s.start();
        QTRY_COMPARE_WITH_TIMEOUT(s.centreKHz(), 7125.0, 5000);
        rig.radioOn = false;
        QTRY_VERIFY_WITH_TIMEOUT(!s.rigAnswering(), 3 * kSilenceMs);
        QCOMPARE(s.modeText(), QStringLiteral("no rig"));
        QCOMPARE(s.rigSilentText(), QStringLiteral("Rig not answering (127.0.0.1:%1)").arg(rig.port()));
        QCOMPARE(s.rigHz(), qint64(0));

        const int before = rig.received.size();
        QCOMPARE(s.tuneTo(kStationKHz, QStringLiteral("AM")), Session::Tuned::Manual);
        QVERIFY(!s.followRig());
        QCOMPARE(follows.size(), 1);
        QVERIFY(!stored(dir.path()).followRig);
        QCOMPARE(s.modeText(), QStringLiteral("manual"));
        QCOMPARE(s.centreKHz(), kStationKHz);
        QTest::qWait(500);
        QVERIFY(noTuningSent(rig, before));

        s.setFollowRig(true);
        QCOMPARE(s.centreKHz(), kStationKHz);
        QVERIFY(stored(dir.path()).followRig);
        QCOMPARE(s.modeText(), QStringLiteral("no rig"));
    }

    // Follow rig ticked but nothing ever answers: once the rig counts as
    // silent, the remembered frequency goes on the dial.
    void silentRigStartsOnRememberedFrequency()
    {
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), deadPort()));
        Session s(dir.path());
        QSignalSpy centres(&s, &Session::centreChanged);
        s.start();
        QCOMPARE(s.centreKHz(), 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(!s.rigAnswering() && s.centreKHz() > 0.0, 3 * kSilenceMs);
        QCOMPARE(s.centreKHz(), 6070.0);
        QCOMPARE(centres.size(), 1);
        QCOMPARE(centres.last().at(1).value<Session::Origin>(), Session::Origin::Remembered);
        QCOMPARE(s.frequencyText(), QStringLiteral("6 070.000 kHz"));
        QCOMPARE(s.modeText(), QStringLiteral("no rig"));
        QVERIFY(s.followRig());
    }

    // The Follow rig switch: on takes the rig's frequency at once and is
    // stored; off for the session only leaves the stored preference; while
    // off, the rig's moves are noted but not shown.
    void followSwitchTakesTheRigsFrequency()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        Session s(dir.path());
        s.start();
        QTRY_VERIFY_WITH_TIMEOUT(s.rigAnswering(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(s.rigHz(), qint64(7125000), 5000);
        QCOMPARE(s.centreKHz(), 6070.0);

        QSignalSpy centres(&s, &Session::centreChanged);
        s.setFollowRig(true);
        QCOMPARE(s.centreKHz(), 7125.0);
        QCOMPARE(centres.last().at(1).value<Session::Origin>(), Session::Origin::Rig);
        QVERIFY(stored(dir.path()).followRig);
        QCOMPARE(s.modeText(), QStringLiteral("USB"));

        s.setFollowRig(false, false);   // for the session only
        QVERIFY(!s.followRig());
        QVERIFY(stored(dir.path()).followRig);
        s.setManualKHz(9500.0);
        QCOMPARE(s.centreKHz(), 9500.0);
        rig.hz = 7200000;
        QTRY_COMPARE_WITH_TIMEOUT(s.rigHz(), qint64(7200000), 5000);
        QCOMPARE(s.centreKHz(), 9500.0);   // not followed
        s.setFollowRig(true);
        QCOMPARE(s.centreKHz(), 7200.0);
    }

    // Started with -f: manual for the session, the stored preference stays.
    void startWithAFrequencyIsManualForTheSession()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port()));
        Session s(dir.path());
        s.start(9500.0);
        QVERIFY(!s.followRig());
        QCOMPARE(s.centreKHz(), 9500.0);
        QCOMPARE(s.modeText(), QStringLiteral("manual"));
        QVERIFY(stored(dir.path()).followRig);
        QTRY_VERIFY_WITH_TIMEOUT(s.rigAnswering(), 5000);
        QCOMPARE(s.centreKHz(), 9500.0);
    }

    // What the list is given: nothing before anything is tuned, the dial
    // around the frequency, the search; the same search is not run again
    // while it is on screen, until the text or the data changes.
    void rowsForTheDialAndTheSearch()
    {
        FakeRigctld rig;
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), rig.port(), false));
        QVERIFY(addStations(dir.path()));
        Session s(dir.path());
        Session::Rows r = s.rows(QString(), false);
        QCOMPARE(r.kind, Session::Lookup::None);
        QVERIFY(r.list.isEmpty());

        s.start();
        r = s.rows(QString(), false);
        QCOMPARE(r.kind, Session::Lookup::Dial);
        QCOMPARE(r.list.size(), 4);
        QCOMPARE(r.list.first().kHz, 6100.0);   // in frequency order
        QCOMPARE(r.list.last().kHz, 9400.0);

        r = s.rows(QStringLiteral("radio"), false);
        QCOMPARE(r.kind, Session::Lookup::Search);
        QVERIFY(!r.same);
        QCOMPARE(r.list.size(), 4);
        r = s.rows(QStringLiteral(" radio "), true);   // again, only the VFO moved
        QVERIFY(r.same);
        QVERIFY(r.list.isEmpty());
        r = s.rows(QStringLiteral("radio"), false);   // the dial is on screen: a search
        QVERIFY(!r.same);
        QCOMPARE(r.list.size(), 4);
        s.forgetSearch();   // the data changed
        r = s.rows(QStringLiteral("radio"), true);
        QVERIFY(!r.same);
        r = s.rows(QStringLiteral("radio !bravo"), true);
        QVERIFY(!r.same);
        QCOMPARE(r.list.size(), 3);
        r = s.rows(QString(), true);   // back to the dial
        QCOMPARE(r.kind, Session::Lookup::Dial);
        r = s.rows(QStringLiteral("radio !bravo"), true);   // a search after the dial runs
        QVERIFY(!r.same);

        // all four downloadable sources are off, the personal list is not one
        const QStringList disabled = s.disabledSources();
        QCOMPARE(disabled.size(), 4);
        QVERIFY(disabled.contains(QStringLiteral("eibi")));
        QVERIFY(!disabled.contains(userSourceId()));
    }

    // The status line counts the enabled sources, says when none is, and
    // lists a source the program does not know by its id.
    void sourcesStatusTexts()
    {
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), deadPort()));
        {
            Session s(dir.path());
            QString tip = QStringLiteral("x");
            QCOMPARE(s.sourcesStatus(&tip), QStringLiteral("No sources enabled"));
            QVERIFY(tip.isEmpty());
        }
        {
            StationDb db(dir.path() + QStringLiteral("/stations.db"));
            QVERIFY(db.open());
            AppSettings a;
            a.load(&db);
            a.eibiEnabled = true;
            a.save(&db);
            db.setMeta(QStringLiteral("eibi.updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            db.setMeta(QStringLiteral("eibi.season"), EibiParser::seasonCode(QDate::currentDate()));
            StationEntry e;
            e.source = QStringLiteral("eibi");
            e.kHz = 6005;
            e.station = QStringLiteral("Radio Foo");
            QVERIFY(db.replaceSource(QStringLiteral("eibi"), {e}));
            e.source = QStringLiteral("extra");
            QVERIFY(db.replaceSource(QStringLiteral("extra"), {e, e}));
        }
        Session s(dir.path());
        const QString name = s.updater()->source(QStringLiteral("eibi"))->displayName();
        const QString season = EibiParser::seasonCode(QDate::currentDate()).toUpper();
        QString tip;
        QCOMPARE(s.sourcesStatus(&tip), QStringLiteral("%1 %2: 1  |  EXTRA: 2").arg(name, season));
        QVERIFY2(tip.startsWith(QStringLiteral("%1 %2: 1 entries, checked ").arg(name, season)), qPrintable(tip));
    }

    // OK in Settings: the rig's new address is in use at once, the settings
    // are stored, and the listeners are told. The version check, switched
    // off, shows nothing and says so when asked by hand.
    void acceptSettingsMovesTheRig()
    {
        const quint16 first = deadPort(), second = deadPort();
        QTemporaryDir dir;
        QVERIFY(seed(dir.path(), first));
        Session s(dir.path());
        QSignalSpy applied(&s, &Session::settingsApplied);
        QSignalSpy messages(&s, &Session::message);
        int versions = 0, validVersions = 0;
        connect(&s, &Session::versionChecked, this, [&](const UpdateCheck::Result& r) {
            ++versions;
            if (r.valid)
                ++validVersions;
        });
        s.start();
        QCOMPARE(s.rig()->port(), first);
        AppSettings n = s.settings();
        n.rigPort = second;
        n.toleranceKHz = 12.5;
        s.acceptSettings(n);
        QCOMPARE(applied.size(), 1);
        QCOMPARE(s.rig()->port(), second);
        QCOMPARE(s.rigSilentText(), QStringLiteral("Rig not answering (127.0.0.1:%1)").arg(second));
        QCOMPARE(stored(dir.path()).rigPort, int(second));
        QCOMPARE(stored(dir.path()).toleranceKHz, 12.5);

        s.startUpdateCheck();
        QCOMPARE(versions, 1);
        QCOMPARE(validVersions, 0);
        s.checkForNewVersionNow();
        QCOMPARE(messages.last().at(0).toString(), QStringLiteral("Version check is switched off in Settings"));
        QCOMPARE(versions, 1);

        // a value saved "soon" arrives, and a save now drops the pending one
        s.settings().scaleSpanKHz = 250.0;
        s.saveSettingsSoon();
        QCOMPARE(stored(dir.path()).scaleSpanKHz, 100.0);
        QTRY_COMPARE_WITH_TIMEOUT(stored(dir.path()).scaleSpanKHz, 250.0, 3000);
        s.settings().scaleSpanKHz = 300.0;
        s.saveSettingsSoon();
        s.saveSettings();
        QCOMPARE(stored(dir.path()).scaleSpanKHz, 300.0);
        auto* timer = s.findChild<QTimer*>(QStringLiteral("saveTimer"));
        QVERIFY(timer && !timer->isActive());
    }
};

QTEST_GUILESS_MAIN(TestSession)
#include "test_session.moc"
