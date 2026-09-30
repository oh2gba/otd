// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/AokiParser.h"
#include "core/Schedule.h"
#include "core/StationDb.h"

#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

class TestAoki : public QObject
{
    Q_OBJECT
private slots:
    void parsesList()
    {
        const QByteArray data =
            "A26 Shortwave Frequecy List  September 20  2026,  0300 UTC   Day 1 = Sunday\r\n"
            "FRE   STATION                         UTC       Su-W-Sa Language             Pow Azi Location                ADM L/L             Remarks\r\n"
            "   40 Time Signal                     0000-2400 1234567 A1B                   50  ND Otakadoyama Tamura C    J   372221N1405056E NICT\r\n"
            "  198 BBC R4                          0500-0100  23456  English              250     Droitwich               G   521744N0020622W BBC R4\r\n"
            "  225 Polskie Radio 1                 0440-0450 .234567 Belarussian         1000  ND Solec Kujawski          POL 530116N0181539E POR\r\n"
            "  225 Polskie Radio 1                 1010-1020    1    Belarussian         1000  ND Solec Kujawski          POL 530116N0181539E POR\r\n"
            " 9500 CHINA RADIO INTERNATIONAL       1100-1400 1234567 English              100 209 Kashi-Saibagh 2022      TKS 392152N0754258E CRI a26\r\n"
            " 9500 CNR 1 Voice of China            0600-0900 12.4567 Chinese              100 165 Shijiazhuang 723        CHN 374951N1142810E CNR1 a26\r\n"
            "  765xYamaguchi Hoso (KRY)            0000-2400 1234567 Japanese               5     Shunan                  J   340204N1314237E JOPF'24Jul.29off\r\n"
            " 1557*R.TAIWAN INT.                   0900-1100 1234567 Chinese              250 299 Kouhu                   TWN 233402N1201021E RTI\r\n"
            " 68.5 BPC Time Signal                 0000-2400 1234567                       90     Shanngqiu               CHN 342723N1155013E PBC\r\n"
            "  810 AIR Delhi A (DRM)F250001        0000-2400 1234567 Hind(Digital)        300     Delhi                   IND 284609N0770813E AIR News24x7\r\n"
            " 5505 Shannon Volmet                  0000-2400 1234567 English                                              IRL                 USB\r\n"
            "\r\n\r\n";
        const auto r = AokiParser::parse(data);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(r.title.startsWith("A26 Shortwave"));
        QCOMPARE(r.entries.size(), 11);
        QCOMPARE(r.skippedLines, 0);

        QVERIFY(r.sundayFirst);   // "Day 1 = Sunday"

        // the fields as the list has them
        const StationEntry& ts = r.entries[0];
        QCOMPARE(ts.source, QStringLiteral("aoki"));
        QCOMPARE(ts.kHz, 40.0);
        QCOMPARE(ts.startMin, 0);
        QCOMPARE(ts.endMin, 1440);
        QCOMPARE(ts.days, QStringLiteral("1234567"));
        QCOMPARE(ts.station, QStringLiteral("Time Signal"));
        QCOMPARE(ts.lang, QStringLiteral("A1B"));
        QCOMPARE(ts.itu, QStringLiteral("J"));
        QCOMPARE(ts.site, QStringLiteral("Otakadoyama Tamura C"));
        QCOMPARE(ts.power, QStringLiteral("50"));
        QCOMPARE(ts.azimuth, QStringLiteral("ND"));
        QCOMPARE(ts.remarks, QStringLiteral("NICT"));
        QCOMPARE(r.entries[1].days, QStringLiteral("23456"));   // the list's own numbering
        QCOMPARE(r.entries[1].startMin, 300);
        QCOMPARE(r.entries[1].endMin, 60);
        QCOMPARE(r.entries[2].days, QStringLiteral(".234567"));
        QCOMPARE(r.entries[3].days, QStringLiteral("1"));
        QCOMPARE(r.entries[6].flag, QStringLiteral("x"));
        QCOMPARE(r.entries[6].persistence, 0);
        QCOMPARE(r.entries[7].flag, QStringLiteral("*"));
        QCOMPARE(r.entries[8].kHz, 68.5);
        QVERIFY(r.entries[8].mode.isEmpty());   // no mode in the list; it is worked out when shown
        QCOMPARE(r.entries[10].site, QString());
        QCOMPARE(r.entries[10].itu, QStringLiteral("IRL"));

        // what the list shows, through the lookup table with the day numbering
        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QVERIFY(db.replaceCodes("aoki", {{"days", {{"1", "Sunday"}}}}));
        QVERIFY(db.replaceSource("aoki", r.entries));
        StationList rows;
        for (const StationEntry& e : r.entries)   // in the file's order, as stored
            rows << e;
        // Aoki day 1 = Sunday; " 23456 " = Mon..Fri -> Monday-first 12345
        QCOMPARE(db.names().weekdays(rows[1]), QStringLiteral("12345"));
        QCOMPARE(db.names().weekdays(rows[2]), QStringLiteral("123456"));   // ".234567" = Mon..Sat
        QCOMPARE(db.names().weekdays(rows[3]), QStringLiteral("7"));        // "1" = Sunday
        QCOMPARE(db.names().weekdays(rows[5]), QStringLiteral("134567"));   // "12.4567" = all but Tuesday
        QCOMPARE(db.names().daysOf(rows[0]), QString());                    // every day
        QCOMPARE(StationNames::remarksOf(rows[0]), QStringLiteral("50 kW, NICT"));
        QCOMPARE(StationNames::remarksOf(rows[4]), QStringLiteral("100 kW, az 209°, CRI a26"));
        QVERIFY(StationNames::remarksOf(rows[7]).endsWith("*"));
        QCOMPARE(StationNames::modeOf(rows[8]), QStringLiteral("AM"));
        QCOMPARE(StationNames::modeOf(rows[9]), QStringLiteral("DRM"));
        QCOMPARE(StationNames::modeOf(rows[10]), QStringLiteral("USB"));
        QCOMPARE(db.names().languageOf(rows[4]), QStringLiteral("English"));
        QCOMPARE(db.names().siteOf(rows[4]), QStringLiteral("Kashi-Saibagh 2022"));
        const QDateTime monday(QDate(2026, 9, 28), QTime(6, 0), QTimeZone::UTC);
        QCOMPARE(Schedule::status(rows[6], monday, db.names().weekdays(rows[6])), Schedule::OnAir::Inactive);   // "x"
        QCOMPARE(Schedule::status(rows[1], monday, db.names().weekdays(rows[1])), Schedule::OnAir::Yes);
        QCOMPARE(Schedule::status(rows[3], monday.addSecs(4 * 3600), db.names().weekdays(rows[3])), Schedule::OnAir::No);

        // a list that numbers from Monday
        QVERIFY(db.replaceCodes("aoki", {{"days", {{"1", "Monday"}}}}));
        QCOMPARE(db.names().weekdays(rows[1]), QStringLiteral("23456"));
    }

    void mondayFirstTitle()
    {
        const QByteArray data =
            "B25 Shortwave Frequency List   Day 1 = Monday\r\n"
            "FRE   STATION                         UTC       Su-W-Sa Language             Pow Azi Location                ADM L/L             Remarks\r\n"
            " 9500 CHINA RADIO INTERNATIONAL       1100-1400 1234567 English              100 209 Kashi-Saibagh 2022      TKS 392152N0754258E CRI a26\r\n";
        QVERIFY(!AokiParser::parse(data).sundayFirst);
    }

    void missingHeaderIsAnError()
    {
        const auto r = AokiParser::parse("just some text\r\n 9500 x\r\n");
        QVERIFY(!r.error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestAoki)
#include "test_aoki.moc"
