// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/StationDb.h"
#include <QTemporaryDir>
#include <QtTest>

class TestStationDb : public QObject
{
    Q_OBJECT

    static StationEntry entry(double kHz, const QString& name)
    {
        StationEntry e;
        e.source = "eibi";
        e.kHz = kHz;
        e.station = name;
        e.itu = "RUS";
        return e;
    }

private slots:
    // The frequency matches as written in the list, with all its decimals:
    // "13553.125" and "10000.25" must be found (a 6-digit rendering lost them).
    void searchMatchesFrequencyText()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        StationList list;
        list << entry(13553.125, "HiFER beacon") << entry(10000.25, "Odd one") << entry(9500, "Round")
             << entry(11774.7, "Sound of Hope");
        QVERIFY(db.replaceSource("eibi", list));
        QCOMPARE(db.search("13553.125").size(), 1);
        QCOMPARE(db.search("10000.25").size(), 1);
        QCOMPARE(db.search("9500").size(), 1);
        QCOMPARE(db.search("9500.0").size(), 0);      // written without decimals in the list
        QCOMPARE(db.search("11774.7").size(), 1);
        QCOMPARE(db.search("11774.70").size(), 0);
        QCOMPARE(db.search("1355").size(), 1);
        QCOMPARE(db.search("!13553 beacon").size(), 0);
    }

    void roundTripAndLookup()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        StationDb db(dir.filePath("sub/stations.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QCOMPARE(db.count(), 0);

        StationList list;
        list << entry(4625, "The Buzzer") << entry(4610, "GYA Northwood Meteo Fax")
             << entry(4635, "Far away") << entry(4624, "Close one");
        QVERIFY2(db.replaceSource("eibi", list), qPrintable(db.lastError()));
        QCOMPARE(db.count(), 4);
        QCOMPARE(db.count("eibi"), 4);
        QCOMPARE(db.count("aoki"), 0);

        const StationList near = db.lookup(4625, 5);
        QCOMPARE(near.size(), 2);
        QCOMPARE(near[0].station, QStringLiteral("The Buzzer"));
        QCOMPARE(near[1].station, QStringLiteral("Close one"));
        QCOMPARE(db.lookup(4625, 12).size(), 3);
        QCOMPARE(db.lookup(4625, 15).size(), 4);   // BETWEEN is inclusive
        QCOMPARE(db.lookup(100, 1).size(), 0);

        const StationList found = db.search("buzz");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].kHz, 4625.0);
        QCOMPARE(db.search("100%").size(), 0);
        QCOMPARE(db.search("RUS").size(), 4);                       // ITU code match
        QCOMPARE(db.search("buzz", {"eibi"}).size(), 0);           // source excluded
        QCOMPARE(db.search("buzz", {"aoki"}).size(), 1);
        QCOMPARE(db.search("buzz RUS").size(), 1);                  // every word must match
        QCOMPARE(db.search("buzz meteo").size(), 0);
        QCOMPARE(db.search("!buzz").size(), 3);                     // exclusion
        QCOMPARE(db.search("RUS !buzz !meteo").size(), 2);
        QCOMPARE(db.search("!").size(), 0);
        QCOMPARE(db.search("4625").size(), 1);                      // the frequency counts as text
        QCOMPARE(db.search("462").size(), 2);                       // 4625 and 4624
        QCOMPARE(db.search("  ").size(), 0);
        QCOMPARE(db.sourceCounts().size(), 1);
        QCOMPARE(db.sourceCounts().first().second, 4);

        // replacing drops the old rows
        QVERIFY(db.replaceSource("eibi", {entry(6000, "Only one")}));
        QCOMPARE(db.count(), 1);

        QVERIFY(db.setMeta("eibi.season", "a26"));
        QCOMPARE(db.meta("eibi.season"), QStringLiteral("a26"));
        QCOMPARE(db.meta("missing", "dflt"), QStringLiteral("dflt"));
    }

    void personalEntries()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("stations.db"));
        QVERIFY(db.open());
        StationEntry e = entry(4625, "My Buzzer note");
        e.source = userSourceId();
        e.mode = "AM";
        QVERIFY2(db.insertEntry(e), qPrintable(db.lastError()));
        QVERIFY(e.id > 0);
        QCOMPARE(db.count(userSourceId()), 1);

        StationList mine = db.entriesOf(userSourceId());
        QCOMPARE(mine.size(), 1);
        QCOMPARE(mine[0].id, e.id);
        QCOMPARE(mine[0].mode, QStringLiteral("AM"));

        e.station = "Renamed";
        e.kHz = 4626;
        QVERIFY(db.updateEntry(e));
        const StationList near = db.lookup(4626, 0.1);
        QCOMPARE(near.size(), 1);
        QCOMPARE(near[0].station, QStringLiteral("Renamed"));
        QCOMPARE(near[0].id, e.id);

        QVERIFY(db.removeEntry(e.id));
        QVERIFY(!db.removeEntry(e.id));
        QCOMPARE(db.count(), 0);
    }

    void codeTablesPersist()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("stations.db");
        {
            StationDb db(path);
            QVERIFY(db.open());
            EibiParser::CodeTables t;
            t.languages.insert("E", "English");
            t.countries.insert("RUS", "Russia");
            t.countries.insert("AUT", "Austria");
            t.targets.insert("Eu", "Europe");
            t.sites.insert("RUS/s", "Samara");
            t.sites.insert("AUT/m", "Moosbrunn");
            t.sites.insert("ARM/", "Gavar");
            QVERIFY(db.storeCodes(t));
        }
        StationDb db(path);
        QVERIFY(db.open());
        QCOMPARE(db.languageName("E"), QStringLiteral("English"));
        QCOMPARE(db.languageName("ZZZ"), QStringLiteral("ZZZ"));
        QCOMPARE(db.countryName("RUS"), QStringLiteral("Russia"));
        QCOMPARE(db.targetName("Eu"), QStringLiteral("Europe"));
        QCOMPARE(db.targetName("RUS"), QStringLiteral("Russia"));   // ITU code as target
        QCOMPARE(db.siteName("RUS", "s"), QStringLiteral("Samara"));
        QCOMPARE(db.siteName("ROU", "/AUT-m"), QStringLiteral("Moosbrunn (Austria)"));
        QCOMPARE(db.siteName("ROU", "/AUT"), QStringLiteral("Austria"));
        QCOMPARE(db.siteName("ARM", ""), QStringLiteral("Gavar"));
        QCOMPARE(db.siteName("RUS", "zz"), QStringLiteral("zz"));
    }
};

QTEST_GUILESS_MAIN(TestStationDb)
#include "test_stationdb.moc"
