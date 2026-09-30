// SPDX-License-Identifier: GPL-3.0-or-later
// AppSettings in the database. The settings are stored as one meta key per
// field through an index table; these tests catch a key and a value
// drifting apart, and old values that must no longer take effect.
#include "core/AppSettings.h"
#include "core/StationDb.h"
#include "settings_helpers.h"

#include <QTemporaryDir>
#include <QtTest>

class TestAppSettings : public QObject
{
    Q_OBJECT
private slots:
    void defaultsOnFirstStart()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        AppSettings s;
        s.load(&db);
        QCOMPARE(s.rigHost, QStringLiteral("localhost"));
        QCOMPARE(s.rigPort, 4532);
        QVERIFY(s.traficomEnabled);
        QVERIFY(s.showPlayer);
        QVERIFY(s.showScale);
        QVERIFY(s.showTable);
        QCOMPARE(s.manualKHz, 4625.0);   // The Buzzer, always there
        QVERIFY(!s.followRig);            // no radio set up yet
        QVERIFY(s.onAirOnly);
        QCOMPARE(s.kiwiMode, QStringLiteral("AM"));
        QCOMPARE(s.scaleSpanKHz, 100.0);
        QCOMPARE(s.updateUrl, QStringLiteral("https://otd.oh2gba.eu/version.php"));
        QVERIFY(s.rigctldPath.isEmpty());
    }

    // Every field survives save and load; a key/value mix-up in the index
    // table shows up here as a field arriving in the wrong place. The other
    // fields all get values of their own, but a boolean has only two: over
    // the passes boolean number i takes the bits of i + 1, so any two
    // booleans differ in at least one pass, and each is on in one pass and
    // off in another.
    void everyFieldRoundTrips_data()
    {
        QTest::addColumn<int>("pass");
        for (int pass = 0; pass < 4; ++pass)   // 12 booleans: i + 1 fits in 4 bits
            QTest::addRow("pass %d", pass) << pass;
    }

    void everyFieldRoundTrips()
    {
        QFETCH(int, pass);
        auto bit = [pass](int i) { return (((i + 1) >> pass) & 1) != 0; };
        AppSettings out = everythingChanged();
        out.updateCheck = bit(0);
        out.eibiEnabled = bit(1);
        out.hfccEnabled = bit(2);
        out.aokiEnabled = bit(3);
        out.traficomEnabled = bit(4);
        out.launchRigctld = bit(5);
        out.onAirOnly = bit(6);
        out.followRig = bit(7);
        out.alwaysOnTop = bit(8);
        out.showScale = bit(9);
        out.showTable = bit(10);
        out.showPlayer = bit(11);

        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        out.save(&db);
        AppSettings in;
        in.load(&db);
        expectSame(in, out);
    }

    // The rigctld path is no longer offered in Settings; a path an older
    // version stored must not keep overriding the included rigctld.
    void storedRigctldPathIsIgnored()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        db.setMeta(QStringLiteral("settings.rigctld.path"), QStringLiteral("C:/old/otd/hamlib/rigctld.exe"));
        AppSettings s;
        s.load(&db);
        QVERIFY(s.rigctldPath.isEmpty());
    }

    // ... and the next save removes the old path from the database.
    void storedRigctldPathIsPurgedOnSave()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        db.setMeta(QStringLiteral("settings.rigctld.path"), QStringLiteral("C:/old/otd/hamlib/rigctld.exe"));
        AppSettings s;
        s.load(&db);
        s.save(&db);
        QCOMPARE(db.meta(QStringLiteral("settings.rigctld.path")), QString());
    }

    // Skipping the path must not shift the keys next to it.
    void storedRigctldPathDoesNotDisturbNeighbours()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        db.setMeta(QStringLiteral("settings.rigctld.launch"), QStringLiteral("1"));
        db.setMeta(QStringLiteral("settings.rigctld.path"), QStringLiteral("/opt/old/rigctld"));
        db.setMeta(QStringLiteral("settings.rigctld.model"), QStringLiteral("3073"));
        db.setMeta(QStringLiteral("settings.rigctld.device"), QStringLiteral("/dev/ttyUSB3"));
        AppSettings s;
        s.load(&db);
        QVERIFY(s.launchRigctld);
        QCOMPARE(s.rigModel, 3073);
        QCOMPARE(s.rigDevice, QStringLiteral("/dev/ttyUSB3"));
        QVERIFY(s.rigctldPath.isEmpty());
    }

    // The page moved to otd.oh2gba.eu; the old stored address is rewritten.
    void oldUpdateAddressIsMigrated()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        db.setMeta(QStringLiteral("settings.update.url"), QStringLiteral("https://onthedial.oh2gba.eu/version.php"));
        AppSettings s;
        s.load(&db);
        QCOMPARE(s.updateUrl, QStringLiteral("https://otd.oh2gba.eu/version.php"));
    }

    void badManualFrequencyFallsBack()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY(db.open());
        db.setMeta(QStringLiteral("settings.view.manualKHz"), QStringLiteral("rubbish"));
        AppSettings s;
        s.load(&db);
        QCOMPARE(s.manualKHz, 4625.0);
    }
};

QTEST_GUILESS_MAIN(TestAppSettings)
#include "test_appsettings.moc"
