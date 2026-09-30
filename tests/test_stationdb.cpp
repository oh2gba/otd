// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/Ciraf.h"
#include "core/StationDb.h"
#include "core/StationSearch.h"
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
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

    // a few lines of EiBi's code tables, as its README gives them
    static EibiParser::CodeTables codeTables()
    {
        EibiParser::CodeTables t;
        t.languages.insert("FI", "Finnish: Finland (5m)");
        t.languages.insert("SWE", "Swedish: Sweden (8m), Finland (0.3m)");
        t.languages.insert("E", "English: UK (60m), USA (225m), India (200m), others");
        t.languages.insert("F", "French: France (53m), Canada (7m)");
        t.countries.insert("FIN", "Finland");
        t.countries.insert("S", "Sweden");
        t.countries.insert("D", "Germany");
        t.countries.insert("USA", "United States of America");
        t.sites.insert("FIN/hv", "Harjavalta");
        t.sites.insert("D/n", "Nauen");
        t.sites.insert("S/", "Hörby");
        return t;
    }
    static StationEntry eibi(double kHz, const QString& name, const QString& itu, const QString& lang,
                             const QString& site)
    {
        StationEntry e = entry(kHz, name);
        e.itu = itu;
        e.lang = lang;
        e.site = site;
        return e;
    }
    static StationList eibiRows()
    {
        return {eibi(7400, "Radio Hiekka", "FIN", "FI", "hv"),
                eibi(6070, "Radio Sweden", "S", "SWE", ""),
                eibi(9265, "WINB relay", "USA", "E", "/D-n"),
                eibi(11800, "Two tongues", "D", "F,E", "n"),
                eibi(5000, "Unknown", "XYZ", "QQ", "zz")};
    }

private slots:
    // The rows keep EiBi's codes; the lookup tables give the names the
    // list shows, and the search finds those names.
    void eibiNamesAreSearchable()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.replaceSource("eibi", eibiRows()));
        QCOMPARE(db.search("finnish").size(), 0);   // no tables yet: nothing is called Finnish
        QVERIFY(db.storeCodes(codeTables()));
        HfccParser::Tables hfccTables;
        hfccTables.languages.insert("Fin", "Finnish");
        hfccTables.sites.insert("HAR", {"Harjavalta", "FIN"});
        QVERIFY(db.storeHfccTables(hfccTables));
        StationEntry hfcc = entry(7400, "HFC");
        hfcc.source = "hfcc";
        hfcc.itu = "FIN";
        hfcc.lang = "Fin";
        hfcc.site = "HAR";
        QVERIFY(db.replaceSource("hfcc", {hfcc}));

        auto only = [&](const QString& text) {
            QStringList names;
            for (const StationEntry& e : db.search(text))
                names << e.station;
            names.sort();
            return names;
        };
        QCOMPARE(only("finnish"), (QStringList{"HFC", "Radio Hiekka"}));
        QCOMPARE(only("harjavalta"), (QStringList{"HFC", "Radio Hiekka"}));
        QCOMPARE(only("nauen"), (QStringList{"Two tongues", "WINB relay"}));
        QCOMPARE(only("english"), (QStringList{"Two tongues", "WINB relay"}));
        QCOMPARE(only("french"), QStringList{"Two tongues"});
        QCOMPARE(only("swedish"), QStringList{"Radio Sweden"});
        QCOMPARE(only("hörby"), QStringList{"Radio Sweden"});   // a country's main site (no site code)
        // Finland in Swedish's description ("Sweden (8m), Finland (0.3m)")
        // is where it is spoken, not the language: no Radio Sweden here
        QCOMPARE(only("finland"), (QStringList{"HFC", "Radio Hiekka"}));
        QCOMPARE(only("finnish !hiekka"), QStringList{"HFC"});
        QCOMPARE(only("qq"), QStringList{"Unknown"});   // an unknown code is shown, so found, as it is
        QCOMPARE(only("fi"), (QStringList{"HFC", "Radio Hiekka"}));   // in "Finnish", not the code FI

        // the rows are as they were given
        auto named = [&](const QString& name) {
            for (const StationEntry& e : db.search(name))
                if (e.station == name)
                    return e;
            return StationEntry();
        };
        const StationEntry hiekka = named("Radio Hiekka");
        QCOMPARE(hiekka.lang, QStringLiteral("FI"));
        QCOMPARE(hiekka.site, QStringLiteral("hv"));
        QCOMPARE(db.names().languageOf(hiekka), QStringLiteral("Finnish"));
        QCOMPARE(db.names().languageOf(hiekka, true), QStringLiteral("Finnish: Finland (5m)"));
        QCOMPARE(db.names().siteOf(hiekka), QStringLiteral("Harjavalta"));
        QCOMPARE(db.names().languageOf(named("Two tongues")), QStringLiteral("French, English"));
        QCOMPARE(db.names().siteOf(named("WINB relay")), QStringLiteral("Nauen (Germany)"));   // a relay
        QCOMPARE(db.names().languageOf(named("Unknown")), QStringLiteral("QQ"));
        QCOMPARE(db.names().siteOf(named("Unknown")), QStringLiteral("zz"));
        QCOMPARE(db.names().siteOf(named("HFC")), QStringLiteral("Harjavalta"));   // HFCC's own table
    }

    // A database from before the rows were kept as published: what was
    // downloaded goes (it is fetched again), the personal list stays with
    // the names it was entered with, the code tables start empty.
    void oldLayoutIsMigrated()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("s.db");
        {
            QSqlDatabase old = QSqlDatabase::addDatabase("QSQLITE", "old");
            old.setDatabaseName(path);
            QVERIFY(old.open());
            QSqlQuery q(old);
            QVERIFY(q.exec("CREATE TABLE stations (id INTEGER PRIMARY KEY, source TEXT NOT NULL, khz REAL NOT NULL,"
                           " start_min INTEGER NOT NULL, end_min INTEGER NOT NULL, days TEXT, itu TEXT, station TEXT,"
                           " lang TEXT, target TEXT, site TEXT, persistence INTEGER, start_date TEXT, stop_date TEXT,"
                           " last_heard TEXT, remarks TEXT, lang_text TEXT, site_text TEXT, mode TEXT, target_text TEXT)"));
            QVERIFY(q.exec("CREATE TABLE codes (kind TEXT NOT NULL, code TEXT NOT NULL, name TEXT, PRIMARY KEY (kind, code))"));
            QVERIFY(q.exec("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT)"));
            QVERIFY(q.exec("INSERT INTO stations (source, khz, start_min, end_min, station, lang, lang_text, site_text, mode)"
                           " VALUES ('eibi', 7400, 0, 1440, 'Radio Hiekka', 'FI', 'Finnish', 'Harjavalta', 'AM')"));
            QVERIFY(q.exec("INSERT INTO stations (source, khz, start_min, end_min, station, lang, lang_text, site_text, mode)"
                           " VALUES ('hfcc', 9500, 0, 60, 'Telediffusion', 'Arb', 'Standard Arabic', 'Bechar', 'AM')"));
            QVERIFY(q.exec("INSERT INTO stations (source, khz, start_min, end_min, station, lang, lang_text, site_text, mode, remarks)"
                           " VALUES ('user', 4625, 0, 1440, 'The Buzzer', '', 'None', 'Povarovo', 'USB', 'mine')"));
            QVERIFY(q.exec("INSERT INTO stations (source, khz, start_min, end_min, station, lang_text)"
                           " VALUES ('kf-ut', 8000, 0, 1440, 'Utility', 'English')"));
            QVERIFY(q.exec("INSERT INTO codes VALUES ('lang', 'FI', 'Finnish: Finland (5m)')"));
            QVERIFY(q.exec("INSERT INTO meta VALUES ('eibi.lastModified', 'x'), ('eibi.updated', 'y'),"
                           " ('schema.dataVersion', '2'), ('settings.view.manualKHz', '7400')"));
            old.close();
        }
        QSqlDatabase::removeDatabase("old");

        StationDb db(path);
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QCOMPARE(db.count("eibi"), 0);
        QCOMPARE(db.count("hfcc"), 0);
        const StationList mine = db.entriesOf("user");
        QCOMPARE(mine.size(), 1);
        QCOMPARE(mine.first().station, QStringLiteral("The Buzzer"));
        QCOMPARE(mine.first().lang, QStringLiteral("None"));
        QCOMPARE(mine.first().site, QStringLiteral("Povarovo"));
        QCOMPARE(mine.first().mode, QStringLiteral("USB"));
        QCOMPARE(mine.first().remarks, QStringLiteral("mine"));
        QCOMPARE(db.entriesOf("kf-ut").size(), 1);   // not downloaded by otd: kept
        QCOMPARE(db.entriesOf("kf-ut").first().lang, QStringLiteral("English"));
        QCOMPARE(db.search("povarovo").size(), 1);
        QVERIFY(db.code("eibi", "lang", "FI").isEmpty());   // tables come again with the download
        QVERIFY(db.meta("eibi.lastModified").isEmpty());    // so EiBi counts as never fetched
        QVERIFY(db.meta("eibi.updated").isEmpty());
        QCOMPARE(db.meta("settings.view.manualKHz"), QStringLiteral("7400"));   // the settings stay
        QCOMPARE(db.meta("schema.dataVersion"), QStringLiteral("3"));

        // and done once: a later download is not thrown away again
        QVERIFY(db.replaceSource("eibi", eibiRows()));
        StationDb again(path);
        QVERIFY(again.open());
        QCOMPARE(again.count("eibi"), 5);
    }

    // EiBi's target codes as names: from its table, put together from a
    // direction and a region ("NEu"), or a country.
    void targetTextFromCodes()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        EibiParser::CodeTables t = codeTables();
        t.targets.insert("Eu", "Europe (often including North Africa/Middle East)");
        t.targets.insert("Am", "America(s)");
        t.targets.insert("As", "Asia");
        t.targets.insert("ME", "Middle East");
        t.targets.insert("N..", "North ..");
        t.targets.insert("S..", "South ..");
        t.targets.insert("C..", "Central ..");
        QVERIFY(db.storeCodes(t));
        QCOMPARE(db.names().targetText("Eu"), QStringLiteral("Europe"));
        QCOMPARE(db.names().targetText("Eu", true), QStringLiteral("Europe (often including North Africa/Middle East)"));
        QCOMPARE(db.names().targetText("NEu"), QStringLiteral("North Europe"));
        QCOMPARE(db.names().targetText("SAs"), QStringLiteral("South Asia"));
        QCOMPARE(db.names().targetText("CAm"), QStringLiteral("Central America(s)"));
        QCOMPARE(db.names().targetText("FIN"), QStringLiteral("Finland"));   // a country as the target
        QCOMPARE(db.names().targetText("Eu,ME"), QStringLiteral("Europe, Middle East"));
        QCOMPARE(db.names().targetText("WEu"), QString());   // no "W.." in these tables
        QCOMPARE(db.names().targetText("Tas"), QString());
        QCOMPARE(db.names().targetText(""), QString());
    }

    // HFCC's CIRAF zones as names: lists, quadrants, ranges.
    void cirafZoneNames()
    {
        QCOMPARE(Ciraf::text("18,27"), QStringLiteral("Nordic countries, Western Europe"));
        QCOMPARE(Ciraf::text("40E,41NW"), QStringLiteral("Iran, Afghanistan (east), South Asia (northwest)"));
        QCOMPARE(Ciraf::text("27-29"), QStringLiteral("Western Europe, Central Europe, Eastern Europe, Caucasus"));
        QCOMPARE(Ciraf::text("1-85"), QStringLiteral("worldwide"));
        QCOMPARE(Ciraf::text("69-73"), QStringLiteral("Antarctica"));
        QCOMPARE(Ciraf::text("18,99"), QStringLiteral("Nordic countries, 99"));
        QCOMPARE(Ciraf::text("99"), QString());
        QCOMPARE(Ciraf::text("abc"), QString());
        QCOMPARE(Ciraf::text("18,40E", true), QStringLiteral("18: Denmark, Finland, Norway, Sweden; 40 east: Afghanistan, Iran"));
        for (int z = 1; z <= 85; ++z)   // every zone has a name
            QVERIFY2(!Ciraf::text(QString::number(z)).isEmpty(), qPrintable(QString::number(z)));
    }

    // Targets: EiBi's codes and HFCC's CIRAF zones are shown and found by
    // their names, "north europe" or "nordic".
    void targetNamesAreSearchable()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        EibiParser::CodeTables t = codeTables();
        t.targets.insert("Eu", "Europe (often including North Africa/Middle East)");
        t.targets.insert("N..", "North ..");
        QVERIFY(db.storeCodes(t));
        StationEntry hiekka = eibi(7400, "Radio Hiekka", "FIN", "FI", "hv");
        hiekka.target = "NEu";
        StationEntry europe = eibi(6000, "Euro one", "D", "", "");
        europe.target = "Eu";
        StationEntry odd = eibi(6010, "Odd target", "D", "", "");
        odd.target = "Tas";
        QVERIFY(db.replaceSource("eibi", {hiekka, europe, odd}));
        StationEntry zones = entry(9700, "HFCC A");
        zones.source = "hfcc";
        zones.target = "18,27NW";
        StationEntry unknownZone = entry(9710, "HFCC B");
        unknownZone.source = "hfcc";
        unknownZone.target = "99";
        QVERIFY(db.replaceSource("hfcc", {zones, unknownZone}));

        auto only = [&](const QString& text) {
            QStringList names;
            for (const StationEntry& e : db.search(text))
                names << db.names().stationOf(e);
            names.sort();
            return names;
        };
        QCOMPARE(only("north europe"), (QStringList{"HFCC A", "Radio Hiekka"}));   // "Western Europe (northwest)" too
        QCOMPARE(only("europe"), (QStringList{"Euro one", "HFCC A", "Radio Hiekka"}));   // Western Europe (northwest)
        QCOMPARE(only("nordic"), QStringList{"HFCC A"});
        QCOMPARE(only("northwest"), QStringList{"HFCC A"});
        QCOMPARE(only("tas"), QStringList{"Odd target"});

        QCOMPARE(db.names().targetOf(hiekka), QStringLiteral("North Europe"));
        QCOMPARE(db.names().targetOf(zones), QStringLiteral("Nordic countries, Western Europe (northwest)"));
        QCOMPARE(db.names().targetOf(zones, true), QStringLiteral("CIRAF 18: Denmark, Finland, Norway, Sweden; "
                                                          "27 northwest: Belgium, France, Great Britain, Ireland, Monaco, Netherlands"));
        QCOMPARE(db.names().targetOf(unknownZone), QStringLiteral("99"));
        QCOMPARE(db.names().targetOf(odd), QStringLiteral("Tas"));
        QCOMPARE(db.entriesOf("eibi").first().target, QStringLiteral("Eu"));   // the row keeps the code
    }

    // Search terms: words, exclusions, one field, a quoted value.
    void parseSearchTerms()
    {
        const auto t = StationSearch::parse("bbc !china target:\"North Europe\" lang:finnish !country:fin 13:00 target: mode:");
        QCOMPARE(t.size(), 6);
        QCOMPARE(t[0].value, QStringLiteral("bbc"));
        QVERIFY(t[0].field.isEmpty() && !t[0].negate);
        QCOMPARE(t[1].value, QStringLiteral("china"));
        QVERIFY(t[1].negate);
        QCOMPARE(t[2].field, QStringLiteral("target"));
        QCOMPARE(t[2].value, QStringLiteral("North Europe"));
        QCOMPARE(t[3].field, QStringLiteral("language"));   // lang: is language:
        QCOMPARE(t[3].value, QStringLiteral("finnish"));
        QCOMPARE(t[4].field, QStringLiteral("country"));
        QVERIFY(t[4].negate);
        QVERIFY(t[5].field.isEmpty());                       // "13" is no field: a plain word
        QCOMPARE(t[5].value, QStringLiteral("13:00"));
        // a quote still being typed
        const auto open = StationSearch::parse("target:\"North Eu");
        QCOMPARE(open.size(), 1);
        QCOMPARE(open[0].value, QStringLiteral("North Eu"));
    }

    // field:value looks in that field only, any part of what the list shows
    // there ("target:eu", "language:eng" as well as the whole name).
    void fieldSearch()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        EibiParser::CodeTables t = codeTables();
        t.targets.insert("Eu", "Europe (often including North Africa/Middle East)");
        t.targets.insert("N..", "North ..");
        QVERIFY(db.storeCodes(t));
        StationList rows = eibiRows();
        rows[0].target = "NEu";            // Radio Hiekka
        rows[1].target = "Eu";             // Radio Sweden
        rows[3].target = "FIN";            // Two tongues, aimed at Finland
        StationEntry volmet = eibi(5505, "Shannon Volmet", "IRL", "E", "");
        volmet.days = "USB";
        rows << volmet;
        QVERIFY(db.replaceSource("eibi", rows));
        StationEntry zones = entry(9700, "HFC");
        zones.source = "hfcc";
        zones.target = "18,27-28";
        QVERIFY(db.replaceSource("hfcc", {zones}));

        auto only = [&](const QString& text) {
            QStringList names;
            for (const StationEntry& e : db.search(text))
                names << e.station;
            names.sort();
            return names;
        };
        QCOMPARE(only("target:\"North Europe\""), QStringList{"Radio Hiekka"});
        QCOMPARE(only("target:europe"), (QStringList{"HFC", "Radio Hiekka", "Radio Sweden"}));
        QCOMPARE(only("finland"), (QStringList{"Radio Hiekka", "Two tongues"}));   // country or target
        QCOMPARE(only("target:finland"), QStringList{"Two tongues"});
        QCOMPARE(only("country:finland"), QStringList{"Radio Hiekka"});
        QCOMPARE(only("country:fin"), QStringList{"Radio Hiekka"});               // the code, or in the name
        QCOMPARE(only("language:english"), (QStringList{"Shannon Volmet", "Two tongues", "WINB relay"}));
        QCOMPARE(only("language:english !target:finland"), (QStringList{"Shannon Volmet", "WINB relay"}));
        QCOMPARE(only("site:nauen"), (QStringList{"Two tongues", "WINB relay"}));
        QCOMPARE(only("station:radio"), (QStringList{"Radio Hiekka", "Radio Sweden"}));
        QCOMPARE(only("station:finnish"), QStringList());                          // not a station
        QCOMPARE(only("mode:usb"), QStringList{"Shannon Volmet"});
        QCOMPARE(only("english !mode:usb"), (QStringList{"Two tongues", "WINB relay"}));
        QCOMPARE(only("target:nordic"), QStringList{"HFC"});
        QCOMPARE(db.search("mode:am", {}, 2).size(), 2);                           // the limit after the mode
        QCOMPARE(only("target:eu language:swe"), QStringList{"Radio Sweden"});      // parts of the names, typed
    }

    // The names as the list shows a language code.
    void languageTextFromCodes()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.storeCodes(codeTables()));
        QCOMPARE(db.names().languageText("FI"), QStringLiteral("Finnish"));
        QCOMPARE(db.names().languageText("F,E"), QStringLiteral("French, English"));
        QCOMPARE(db.names().languageText("E,QQ"), QStringLiteral("English, QQ"));   // an unknown part stays a code
        QCOMPARE(db.names().languageText("QQ"), QString());
        QCOMPARE(db.names().languageText(""), QString());
    }

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
        QCOMPARE(db.names().languageText("E"), QStringLiteral("English"));
        QCOMPARE(db.names().languageText("ZZZ"), QString());
        QCOMPARE(db.names().countryName("RUS"), QStringLiteral("Russia"));
        QCOMPARE(db.names().targetText("Eu"), QStringLiteral("Europe"));
        QCOMPARE(db.names().targetText("RUS"), QStringLiteral("Russia"));   // ITU code as target
        QCOMPARE(db.names().siteName("RUS", "s"), QStringLiteral("Samara"));
        QCOMPARE(db.names().siteName("ROU", "/AUT-m"), QStringLiteral("Moosbrunn (Austria)"));
        QCOMPARE(db.names().siteName("ROU", "/AUT"), QStringLiteral("Austria"));
        QCOMPARE(db.names().siteName("ARM", ""), QStringLiteral("Gavar"));
        QCOMPARE(db.names().siteName("RUS", "zz"), QStringLiteral("zz"));
    }
};

QTEST_GUILESS_MAIN(TestStationDb)
#include "test_stationdb.moc"
