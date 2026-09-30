// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/HfccParser.h"
#include "core/Schedule.h"
#include "core/StationDb.h"

#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

class TestHfcc : public QObject
{
    Q_OBJECT

    static HfccParser::Tables tables()
    {
        const QByteArray site =
            ";         20-May-2026 SITE.TXT REFERENCE TABLE\r\n"
            ";--+------------------------------+---+-----+------\r\n"
            ";Co Site Name                      ADM Lati  Longi\r\n"
            "A-A Alma Ata                       KAZ 43N17 077E00\r\n"
            "BEC Bechar                         ALG 31N34 002W21\r\n"
            "GAL Galbeni                        ROU 46N44 026E50\r\n"
            "MOS Moosbrunn                      AUT 48N00 016E28\r\n";
        const QByteArray brc =
            ";CO BROADCASTER\r\n"
            "RRO Radio Romania International\r\n"
            "TDA Telediffusion d'Algerie\r\n";
        const QByteArray lang =
            "\xEF\xBB\xBF;         23-JAN-2014  Reference Table Language\r\n"
            "Arb Standard Arabic                     \r\n"
            "Ron Romanian                            \r\n";
        const QByteArray adm =
            ";CO ADMINISTRATION ENGLISH NAME                        ADMINISTRATION FRENCH NAME\r\n"
            "ALG Algeria                                            Alg\xE9rie                                           Argelia\r\n"
            "AUT Austria                                            Autriche                                          Austria\r\n"
            "ROU Romania                                            Roumanie                                          Rumania\r\n";
        return HfccParser::parseTables(site, brc, lang, adm);
    }

private slots:
    void referenceTables()
    {
        const auto t = tables();
        QCOMPARE(t.sites.size(), 4);
        QCOMPARE(t.sites.value("A-A").name, QStringLiteral("Alma Ata"));
        QCOMPARE(t.sites.value("GAL").adm, QStringLiteral("ROU"));
        QCOMPARE(t.broadcasters.value("RRO"), QStringLiteral("Radio Romania International"));
        QCOMPARE(t.languages.value("Arb"), QStringLiteral("Standard Arabic"));
        QCOMPARE(t.admins.value("ALG"), QStringLiteral("Algeria"));
        QCOMPARE(t.admins.value("ROU"), QStringLiteral("Romania"));
    }

    void schedule()
    {
        const QByteArray data =
            "; A26 ALL 18-sep-2026\r\n"
            "; Global HF Schedule\r\n"
            ";----+----+----+------------------------------+---+----+-------+---+---+-------+------+------+-+-----+----------+---+---+---+-----+-+-----+-----+-----+-------\r\n"
            ";FREQ STRT STOP CIRAF ZONES                    LOC POWR AZIMUTH SLW ANT DAYS    FDATE  TDATE MOD AFRQ LANGUAGE   ADM BRC FMO REQ# OLD ALT1 ALT2  ALT3  NOTES\r\n"
            ";----+----+----+------------------------------+---+----+-------+---+---+-------+------+------+-+-----+----------+---+---+---+-----+-+-----+-----+-----+-------\r\n"
            " 9500 0300 0400 37SE,38SW,46E,47NW             BEC  300 131       0 146 1234567 290326 251026 D  7778 Arb        ALG TDA TDA  2498                     \r\n"
            " 9500 0400 0500 27SE                           GAL  300 285       0 206 1234567 290326 251026 D       Ron        ROU RRO ROU   985                     \r\n"
            " 6155 1500 1600 28                             MOS  100 0         0 900 23456   010626 310826 N       Ron        ROU RRO ROU  2347                     NOTE\r\n"
            " 3210 0000 2400 18,27,28,37                    NIJ   15 0         0 750 1234567 300326 251026 D       Nld        HOL OMR OMR  4853                     \r\n"
            "garbage line\r\n";
        const auto r = HfccParser::parseSchedule(data);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.entries.size(), 4);
        QCOMPARE(r.skippedLines, 1);

        // every field as the file has it: codes, days, dates, power, azimuth
        const StationEntry& a = r.entries[0];
        QCOMPARE(a.source, QStringLiteral("hfcc"));
        QCOMPARE(a.kHz, 9500.0);
        QCOMPARE(a.startMin, 180);
        QCOMPARE(a.endMin, 240);
        QCOMPARE(a.days, QStringLiteral("1234567"));
        QCOMPARE(a.itu, QStringLiteral("ALG"));
        QCOMPARE(a.station, QStringLiteral("TDA"));
        QCOMPARE(a.lang, QStringLiteral("Arb"));
        QCOMPARE(a.site, QStringLiteral("BEC"));
        QCOMPARE(a.target, QStringLiteral("37SE,38SW,46E,47NW"));
        QCOMPARE(a.startDate, QStringLiteral("290326"));
        QCOMPARE(a.stopDate, QStringLiteral("251026"));
        QCOMPARE(a.power, QStringLiteral("300"));
        QCOMPARE(a.azimuth, QStringLiteral("131"));
        QCOMPARE(a.mode, QStringLiteral("D"));
        QVERIFY(a.remarks.isEmpty());

        const StationEntry& b = r.entries[2];
        QCOMPARE(b.days, QStringLiteral("23456"));
        QCOMPARE(b.mode, QStringLiteral("N"));
        QCOMPARE(b.remarks, QStringLiteral("NOTE"));

        const StationEntry& c = r.entries[3];
        QCOMPARE(c.startMin, 0);
        QCOMPARE(c.endMin, 1440);
        QCOMPARE(c.station, QStringLiteral("OMR"));
    }

    // What the list shows for HFCC lines: the codes through HFCC's own
    // reference tables, kept as lookup tables beside the lines.
    void shownThroughLookupTables()
    {
        const QByteArray data =
            ";----+----+----+------------------------------+---+----+-------+---+---+-------+------+------+-+-----+----------+---+---+---+-----+-+-----+-----+-----+-------\r\n"
            " 9500 0300 0400 37SE,38SW,46E,47NW             BEC  300 131       0 146 1234567 290326 251026 D  7778 Arb        ALG TDA TDA  2498                     \r\n"
            " 6155 1500 1600 28                             MOS  100 0         0 900 23456   010626 310826 N       Ron        ROU RRO ROU  2347                     NOTE\r\n"
            " 3210 0000 0000 18,27,28,37                    NIJ   15 0         0 750 1234567 300326 251026 X       Nld        HOL OMR OMR  4853                     \r\n";
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.storeHfccTables(tables()));
        QVERIFY(db.replaceSource("hfcc", HfccParser::parseSchedule(data).entries));
        const StationList rows = db.entriesOf("hfcc");
        QCOMPARE(rows.size(), 3);
        const StationEntry& c = rows[0];   // 3210
        const StationEntry& b = rows[1];   // 6155
        const StationEntry& a = rows[2];   // 9500
        QCOMPARE(a.station, QStringLiteral("TDA"));   // stored as published
        QCOMPARE(db.names().stationOf(a), QStringLiteral("Telediffusion d'Algerie"));
        QCOMPARE(db.names().languageOf(a), QStringLiteral("Standard Arabic"));
        QCOMPARE(db.names().siteOf(a), QStringLiteral("Bechar"));
        QCOMPARE(db.names().countryOf(a), QStringLiteral("Algeria"));
        QCOMPARE(StationNames::remarksOf(a), QStringLiteral("300 kW, az 131°"));
        QCOMPARE(StationNames::modeOf(a), QStringLiteral("AM"));
        QCOMPARE(db.names().daysOf(a), QString());   // every day
        QVERIFY(db.names().targetOf(a).startsWith(QLatin1String("Iberia, Northwest Africa (southeast)")));

        QCOMPARE(db.names().siteOf(b), QStringLiteral("Moosbrunn (Austria)"));   // relay abroad
        QCOMPARE(StationNames::remarksOf(b), QStringLiteral("100 kW, NOTE"));
        QCOMPARE(StationNames::modeOf(b), QStringLiteral("DRM"));
        QCOMPARE(db.names().daysOf(b), QStringLiteral("12345"));   // HFCC counts from Sunday: Mon-Fri

        QCOMPARE(db.names().stationOf(c), QStringLiteral("OMR"));   // unknown broadcaster: its code
        QCOMPARE(db.names().siteOf(c), QStringLiteral("NIJ"));
        QCOMPARE(db.names().languageOf(c), QStringLiteral("Nld"));
        QCOMPARE(StationNames::modeOf(c), QString());
        QCOMPARE(StationNames::remarksOf(c), QStringLiteral("15 kW, mod X"));
        QCOMPARE(Schedule::timeWindow(c), QStringLiteral("24h"));   // 0000-0000

        // validity with the year: on the air inside it, off outside
        const QDateTime inside(QDate(2026, 7, 1), QTime(15, 30), QTimeZone::UTC);
        const QDateTime after(QDate(2026, 9, 1), QTime(15, 30), QTimeZone::UTC);
        QCOMPARE(Schedule::status(b, inside, db.names().weekdays(b)), Schedule::OnAir::Yes);   // a Wednesday, Mon-Fri
        QCOMPARE(Schedule::status(b, after, db.names().weekdays(b)), Schedule::OnAir::No);
        QCOMPARE(Schedule::status(c, after, db.names().weekdays(c)), Schedule::OnAir::Yes);

        // the search finds the names, not only the codes
        QCOMPARE(db.search("telediffusion").size(), 1);
        QCOMPARE(db.search("romanian").size(), 1);
        QCOMPARE(db.search("moosbrunn").size(), 1);
        QCOMPARE(db.search("algeria").size(), 1);
        QCOMPARE(db.search("nordic").size(), 1);
        QCOMPARE(db.search("tda").size(), 0);   // the code is not what the list shows
    }

    void missingRulerIsAnError()
    {
        const auto r = HfccParser::parseSchedule(" 9500 0300 0400 ...\r\n");
        QVERIFY(!r.error.isEmpty());
    }

    // HFCC numbers the days from Sunday (1 = Sunday ... 7 = Saturday); otd
    // shows and evaluates them Monday-first. The file keeps HFCC's digits.
    void daysCountFromSunday()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath(QStringLiteral("s.db")));
        QVERIFY(db.open());
        auto hfcc = [](const char* days) {
            StationEntry e;
            e.source = QStringLiteral("hfcc");
            e.kHz = 5970;
            e.startMin = 990;
            e.endMin = 1020;
            e.days = QString::fromLatin1(days);
            return e;
        };
        QCOMPARE(db.names().weekdays(hfcc("23456")), QStringLiteral("12345"));   // Mon-Fri
        QCOMPARE(db.names().weekdays(hfcc("17")), QStringLiteral("67"));         // Sat, Sun
        QCOMPARE(db.names().weekdays(hfcc("1")), QStringLiteral("7"));           // Sunday
        QCOMPARE(db.names().weekdays(hfcc("7")), QStringLiteral("6"));           // Saturday
        QCOMPARE(db.names().weekdays(hfcc("1234567")), QStringLiteral("1234567"));
        QCOMPARE(db.names().daysOf(hfcc("1234567")), QString());                // every day
        QCOMPARE(hfcc("17").days, QStringLiteral("17"));                         // the row as published

        const StationEntry saturdays = hfcc("7");
        const QDateTime saturday(QDate(2026, 10, 3), QTime(16, 45), QTimeZone::UTC);
        const QDateTime sunday(QDate(2026, 10, 4), QTime(16, 45), QTimeZone::UTC);
        QCOMPARE(saturday.date().dayOfWeek(), 6);
        QCOMPARE(Schedule::status(saturdays, saturday, db.names().weekdays(saturdays)), Schedule::OnAir::Yes);
        QCOMPARE(Schedule::status(saturdays, sunday, db.names().weekdays(saturdays)), Schedule::OnAir::No);
        const StationEntry weekend = hfcc("17");
        const QDateTime monday(QDate(2026, 10, 5), QTime(16, 45), QTimeZone::UTC);
        QCOMPARE(Schedule::status(weekend, sunday, db.names().weekdays(weekend)), Schedule::OnAir::Yes);
        QCOMPARE(Schedule::status(weekend, monday, db.names().weekdays(weekend)), Schedule::OnAir::No);
    }
};

QTEST_GUILESS_MAIN(TestHfcc)
#include "test_hfcc.moc"
