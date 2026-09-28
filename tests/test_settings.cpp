// SPDX-License-Identifier: GPL-3.0-or-later
// AppSettings in the database, and the Settings dialog's result.
// The settings are stored as one meta key per field through an index table;
// these tests catch a key and a value drifting apart, a field the dialog
// forgets to hand back, and old values that must no longer take effect.
#include "SettingsDialog.h"
#include "core/StationDb.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
AppSettings everythingChanged()
{
    AppSettings s;
    s.rigHost = QStringLiteral("shack-pi");
    s.rigPort = 4600;
    s.pollIntervalMs = 250;
    s.toleranceKHz = 3.5;
    s.ituRegion = 2;
    s.updateCheck = false;
    s.updateUrl = QStringLiteral("https://example.org/v.php");
    s.refreshDays = 14;
    s.eibiUrl = QStringLiteral("http://e.example/");
    s.hfccUrl = QStringLiteral("http://h.example/");
    s.aokiUrl = QStringLiteral("http://a.example/");
    s.eibiEnabled = false;
    s.hfccEnabled = false;
    s.aokiEnabled = false;
    s.traficomUrl = QStringLiteral("https://t.example/api");
    s.traficomEnabled = false;
    s.launchRigctld = true;
    s.rigModel = 3073;
    s.rigDevice = QStringLiteral("/dev/ttyUSB3");
    s.rigBaud = 38400;
    s.rigctldExtra = QStringLiteral("-C stop_bits=2");
    s.onAirOnly = true;
    s.followRig = false;
    s.alwaysOnTop = true;
    s.showScale = false;
    s.showTable = false;
    s.showPlayer = false;
    s.kiwiReceivers = {QStringLiteral("http://mine.example:8073"), QStringLiteral("http://two.example:8074")};
    s.kiwiFavourites = {QStringLiteral("http://fav.example:8073")};
    s.kiwiCurrent = QStringLiteral("http://mine.example:8073");
    s.kiwiVolume = 33;
    s.scaleSpanKHz = 50.0;
    s.kiwiMode = QStringLiteral("USB");
    s.manualKHz = 9500.125;
    return s;
}

// Compares every field. Keep it in step with AppSettings. Not compared:
// rigctldPath, which load() and the dialog both clear on purpose (the
// storedRigctldPath tests below cover it).
void expectSame(const AppSettings& in, const AppSettings& out)
{
    QCOMPARE(in.rigHost, out.rigHost);
    QCOMPARE(in.rigPort, out.rigPort);
    QCOMPARE(in.pollIntervalMs, out.pollIntervalMs);
    QCOMPARE(in.toleranceKHz, out.toleranceKHz);
    QCOMPARE(in.ituRegion, out.ituRegion);
    QCOMPARE(in.updateCheck, out.updateCheck);
    QCOMPARE(in.updateUrl, out.updateUrl);
    QCOMPARE(in.refreshDays, out.refreshDays);
    QCOMPARE(in.eibiUrl, out.eibiUrl);
    QCOMPARE(in.hfccUrl, out.hfccUrl);
    QCOMPARE(in.aokiUrl, out.aokiUrl);
    QCOMPARE(in.eibiEnabled, out.eibiEnabled);
    QCOMPARE(in.hfccEnabled, out.hfccEnabled);
    QCOMPARE(in.aokiEnabled, out.aokiEnabled);
    QCOMPARE(in.traficomUrl, out.traficomUrl);
    QCOMPARE(in.traficomEnabled, out.traficomEnabled);
    QCOMPARE(in.launchRigctld, out.launchRigctld);
    QCOMPARE(in.rigModel, out.rigModel);
    QCOMPARE(in.rigDevice, out.rigDevice);
    QCOMPARE(in.rigBaud, out.rigBaud);
    QCOMPARE(in.rigctldExtra, out.rigctldExtra);
    QCOMPARE(in.onAirOnly, out.onAirOnly);
    QCOMPARE(in.followRig, out.followRig);
    QCOMPARE(in.alwaysOnTop, out.alwaysOnTop);
    QCOMPARE(in.showScale, out.showScale);
    QCOMPARE(in.showTable, out.showTable);
    QCOMPARE(in.showPlayer, out.showPlayer);
    QCOMPARE(in.scaleSpanKHz, out.scaleSpanKHz);
    QCOMPARE(in.kiwiReceivers, out.kiwiReceivers);
    QCOMPARE(in.kiwiFavourites, out.kiwiFavourites);
    QCOMPARE(in.kiwiMode, out.kiwiMode);
    QCOMPARE(in.manualKHz, out.manualKHz);
    QCOMPARE(in.kiwiCurrent, out.kiwiCurrent);
    QCOMPARE(in.kiwiVolume, out.kiwiVolume);
}
}

class TestSettings : public QObject
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
        QCOMPARE(s.manualKHz, 6070.0);
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
        QCOMPARE(s.manualKHz, 6070.0);
    }

    // The dialog hands back every field it shows, unchanged when nothing
    // was touched, and keeps the fields it does not show (the KiwiSDR list,
    // the manual frequency, ...) as they were. Its result replaces the
    // window's settings whole, so every field counts.
    void dialogRoundTripKeepsEverything()
    {
        AppSettings cur = everythingChanged();
        cur.launchRigctld = false;   // so the host field is the user's
        SettingsDialog dlg(cur);
        expectSame(dlg.settings(cur), cur);
    }

    // Ticking the Traficom switch in the dialog comes back as on (it was
    // once dropped on the way to the saved settings).
    void dialogTraficomSwitch()
    {
        AppSettings cur;
        cur.traficomEnabled = false;
        SettingsDialog dlg(cur);
        QCheckBox* traficom = nullptr;
        for (QCheckBox* box : dlg.findChildren<QCheckBox*>())
            if (box->text() == QLatin1String("Traficom"))
                traficom = box;
        QVERIFY(traficom);
        traficom->setChecked(true);
        QVERIFY(dlg.settings(cur).traficomEnabled);
    }

    // With "start rigctld for me" the host is this machine, whatever the
    // field said before.
    void startingRigctldPinsLocalhost()
    {
        AppSettings cur;
        cur.rigHost = QStringLiteral("shack-pi");
        cur.launchRigctld = false;
        SettingsDialog dlg(cur);
        QCheckBox* launch = nullptr;
        for (QCheckBox* box : dlg.findChildren<QCheckBox*>())
            if (box->text().startsWith(QLatin1String("Start Hamlib")))
                launch = box;
        QVERIFY(launch);
        launch->setChecked(true);
        const AppSettings s = dlg.settings(cur);
        QCOMPARE(s.rigHost, QStringLiteral("localhost"));
        QVERIFY(s.launchRigctld);
        launch->setChecked(false);
        QCOMPARE(dlg.settings(cur).launchRigctld, false);
    }
};

QTEST_MAIN(TestSettings)
#include "test_settings.moc"
