// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/EibiParser.h"
#include "core/EibiSource.h"
#include "core/StationDb.h"

#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class TestEibi : public QObject
{
    Q_OBJECT
private slots:
    void parsesTypicalLines()
    {
        const QByteArray csv =
            "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\r\n"
            "4625;0000-2400;;RUS;The Buzzer;;RUS;B2;1;;[0826]\r\n"
            "4610;2200-0500;;G;GYA Northwood Meteo Fax;;WEu;nw;1;;\r\n"
            "9500;0500-0600;Mo-Fr;ROU;Radio Romania Int.;E;Eu;/AUT-m;6;0102;2802[0219]\r\n"
            "junk line without fields\r\n"
            "16.3;0000-2400;;IND;VTX1 Indian Navy;;SAs;v;1;;\r\n";
        const auto r = EibiParser::parseCsv(csv);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.entries.size(), 4);
        QCOMPARE(r.skippedLines, 1);

        const StationEntry& buzzer = r.entries[0];
        QCOMPARE(buzzer.kHz, 4625.0);
        QCOMPARE(buzzer.startMin, 0);
        QCOMPARE(buzzer.endMin, 1440);
        QCOMPARE(buzzer.station, QStringLiteral("The Buzzer"));
        QCOMPARE(buzzer.itu, QStringLiteral("RUS"));
        QCOMPARE(buzzer.site, QStringLiteral("B2"));
        QCOMPARE(buzzer.persistence, 1);
        QCOMPARE(buzzer.lastHeard, QStringLiteral("0826"));
        QVERIFY(buzzer.stopDate.isEmpty());

        const StationEntry& fax = r.entries[1];
        QCOMPARE(fax.startMin, 22 * 60);
        QCOMPARE(fax.endMin, 5 * 60);

        const StationEntry& rri = r.entries[2];
        QCOMPARE(rri.days, QStringLiteral("Mo-Fr"));
        QCOMPARE(rri.lang, QStringLiteral("E"));
        QCOMPARE(rri.site, QStringLiteral("/AUT-m"));
        QCOMPARE(rri.persistence, 6);
        QCOMPARE(rri.startDate, QStringLiteral("0102"));
        QCOMPARE(rri.stopDate, QStringLiteral("2802"));
        QCOMPARE(rri.lastHeard, QStringLiteral("0219"));

        QCOMPARE(r.entries[3].kHz, 16.3);

        // EiBi has no mode column: nothing is stored, the list works it out
        QVERIFY(buzzer.mode.isEmpty());
        QCOMPARE(EibiParser::guessMode(buzzer), QStringLiteral("AM"));
        QCOMPARE(EibiParser::guessMode(fax), QStringLiteral("FAX"));
    }

    void modeGuessing()
    {
        StationEntry e;
        e.station = "Some Radio";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("AM"));
        e.days = "USB";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("USB"));
        e.days.clear();
        e.lang = "-CW";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("CW"));
        e.lang = "-TY";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("RTTY"));
        e.lang = "-HF";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("HFDL"));
        e.lang.clear();
        e.station = "Shannon Volmet";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("USB"));
        e.station = "Radio Kuwait DRM";
        QCOMPARE(EibiParser::guessMode(e), QStringLiteral("DRM"));
    }

    void latin1IsDecoded()
    {
        const QByteArray csv = "6000;0000-0100;;D;Radio B\xfcrgerfunk;D;Eu;;1;;\n";
        const auto r = EibiParser::parseCsv(csv);
        QCOMPARE(r.entries.size(), 1);
        QCOMPARE(r.entries[0].station, QString::fromUtf8("Radio Bürgerfunk"));
    }

    void emptyInputIsAnError()
    {
        const auto r = EibiParser::parseCsv("kHz:75;Time(UTC):93\n");
        QVERIFY(!r.error.isEmpty());
        QVERIFY(r.entries.isEmpty());
    }

    void seasonCodes()
    {
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 9, 20)), QStringLiteral("a26"));
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 2, 1)),  QStringLiteral("b25"));
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 3, 28)), QStringLiteral("b25")); // Saturday
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 3, 29)), QStringLiteral("a26")); // last Sunday
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 10, 24)), QStringLiteral("a26"));
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 10, 25)), QStringLiteral("b26"));
        QCOMPARE(EibiParser::seasonCode(QDate(2026, 12, 31)), QStringLiteral("b26"));
        QCOMPARE(EibiParser::seasonCode(QDate(2027, 1, 1)),  QStringLiteral("b26"));

        QCOMPARE(EibiParser::previousSeason(QStringLiteral("a26")), QStringLiteral("b25"));
        QCOMPARE(EibiParser::previousSeason(QStringLiteral("b26")), QStringLiteral("a26"));
        QCOMPARE(EibiParser::nextSeason(QStringLiteral("a26")), QStringLiteral("b26"));
        QCOMPARE(EibiParser::nextSeason(QStringLiteral("b26")), QStringLiteral("a27"));
        QCOMPARE(EibiParser::previousSeason(QStringLiteral("a00")), QStringLiteral("b99"));
    }

    void readmeTables()
    {
        const QByteArray readme =
            "D) Codes used.\n"
            "   I)   Language codes.\n"
            "   II)  Country codes.\n"
            "\n"
            "   I) Language codes.\n"
            "   \n"
            "   -CW   Morse Station\n"
            "   E     English: UK (60m), USA (225m), India (200m), others               [eng]\n"
            "   DI    Dinka: South Sudan (1.4m)                           [dip,diw,dik,dib,dks]\n"
            "         Fujian: see TW-Taiwanese\n"
            "\n"
            "   II) Country codes.\n"
            "   Asterisks (*) denote non-official abbreviations\n"
            "   RUS  Russia\n"
            "   G    United Kingdom\n"
            "   CAB  Cabinda *\n"
            "\n"
            "   III) Target-area codes.\n"
            "   Eu  - Europe (often including North Africa/Middle East)\n"
            "   C.. - Central ..\n"
            "\n"
            "   IV) Transmitter site codes.\n"
            "   One-letter or two-letter codes.\n"
            "   AFS: Meyerton 26S35-28E08 except:\n"
            "        ct-Cape Town 33S41-18E42\n"
            "   ARM: Gavar (formerly Kamo) 40N25-45E12\n"
            "        y-Yerevan 40N10-44E30\n"
            "   RUS: s-Samara 53N17-50E15\n"
            "        B2-Kerro, near St. Petersburg 60N18-30E17\n";
        const auto t = EibiParser::parseReadme(readme);
        QCOMPARE(t.languages.value("E"), QStringLiteral("English: UK (60m), USA (225m), India (200m), others"));
        QCOMPARE(t.languages.value("-CW"), QStringLiteral("Morse Station"));
        QCOMPARE(t.languages.value("DI"), QStringLiteral("Dinka: South Sudan (1.4m)"));
        QCOMPARE(t.countries.value("RUS"), QStringLiteral("Russia"));
        QCOMPARE(t.countries.value("G"), QStringLiteral("United Kingdom"));
        QCOMPARE(t.countries.value("CAB"), QStringLiteral("Cabinda"));
        QCOMPARE(t.targets.value("Eu"), QStringLiteral("Europe (often including North Africa/Middle East)"));
        QCOMPARE(t.sites.value("AFS/"), QStringLiteral("Meyerton"));
        QCOMPARE(t.sites.value("AFS/ct"), QStringLiteral("Cape Town"));
        QCOMPARE(t.sites.value("ARM/"), QStringLiteral("Gavar (formerly Kamo)"));
        QCOMPARE(t.sites.value("ARM/y"), QStringLiteral("Yerevan"));
        QCOMPARE(t.sites.value("RUS/s"), QStringLiteral("Samara"));
        QCOMPARE(t.sites.value("RUS/B2"), QStringLiteral("Kerro, near St. Petersburg"));
    }

    // A whole EiBi update from a small web server on this machine: the
    // schedule, then the README with the code tables. The rows keep EiBi's
    // codes; the list and the search get the names from the README's tables.
    void importFillsInNames()
    {
        QByteArray csv = "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\r\n"
                         "7400;0000-2400;;FIN;Radio Hiekka;FI;NEu;hv;1;;\r\n"
                         "9265;0000-2400;;USA;WINB;E;Eu;/D-n;1;;\r\n";
        for (int i = 0; i < 120; ++i)   // the importer wants a real-sized file
            csv += QByteArray::number(5000 + i) + ";0000-2400;;RUS;Filler;R;Eu;s;1;;\r\n";
        const QByteArray readme =
            "   I) Language codes.\n"
            "   E     English: UK (60m), USA (225m), India (200m), others               [eng]\n"
            "   FI    Finnish: Finland (5m)                                             [fin]\n"
            "   R     Russian: Russia (140m), others                                    [rus]\n"
            "\n"
            "   II) Country codes.\n"
            "   D    Germany\n"
            "   FIN  Finland\n"
            "   RUS  Russia\n"
            "   USA  United States of America\n"
            "\n"
            "   III) Target-area codes.\n"
            "   Eu  - Europe (often including North Africa/Middle East)\n"
            "   N.. - North ..\n"
            "\n"
            "   IV) Transmitter site codes.\n"
            "   D: Wertachtal 48N05-10E42 except:\n"
            "        n-Nauen 52N38-12E54\n"
            "   FIN: Pori 61N28-21E35 except:\n"
            "        hv-Harjavalta 61N18-22E08\n"
            "   RUS: s-Samara 53N17-50E15\n";

        QTcpServer web;
        QVERIFY(web.listen(QHostAddress::LocalHost));
        connect(&web, &QTcpServer::newConnection, this, [&]() {
            while (QTcpSocket* s = web.nextPendingConnection())
                connect(s, &QTcpSocket::readyRead, s, [s, csv, readme]() {
                    const QByteArray request = s->readAll();
                    const QByteArray path = request.split(' ').value(1);
                    QByteArray body;
                    QByteArray status = "200 OK";
                    if (path.endsWith(".csv"))
                        body = csv;
                    else if (path.endsWith("README.TXT"))
                        body = readme;
                    else
                        status = "404 Not Found";
                    s->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size())
                             + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
        });

        QTemporaryDir dir;
        StationDb db(dir.filePath("s.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QNetworkAccessManager nam;
        EibiSource eibi(&db, &nam);
        eibi.setBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/dx/").arg(web.serverPort())));
        QSignalSpy finished(&eibi, &ScheduleSource::finished);
        eibi.update();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
        QVERIFY2(finished.first().first().toBool(), qPrintable(finished.first().last().toString()));

        const StationList finnish = db.search(QStringLiteral("finnish"));
        QCOMPARE(finnish.size(), 1);
        QCOMPARE(finnish.first().station, QStringLiteral("Radio Hiekka"));
        // stored as published, shown through the README's tables
        QCOMPARE(finnish.first().lang, QStringLiteral("FI"));
        QCOMPARE(finnish.first().site, QStringLiteral("hv"));
        QCOMPARE(finnish.first().target, QStringLiteral("NEu"));
        QCOMPARE(db.names().languageOf(finnish.first()), QStringLiteral("Finnish"));
        QCOMPARE(db.names().siteOf(finnish.first()), QStringLiteral("Harjavalta"));
        QCOMPARE(db.names().targetOf(finnish.first()), QStringLiteral("North Europe"));
        QCOMPARE(db.search(QStringLiteral("north europe")).size(), 1);
        const StationList nauen = db.search(QStringLiteral("nauen"));
        QCOMPARE(nauen.size(), 1);
        QCOMPARE(nauen.first().station, QStringLiteral("WINB"));
        QCOMPARE(nauen.first().site, QStringLiteral("/D-n"));
        QCOMPARE(db.names().siteOf(nauen.first()), QStringLiteral("Nauen (Germany)"));
        QCOMPARE(db.search(QStringLiteral("russian samara")).size(), 120);
    }
};

QTEST_GUILESS_MAIN(TestEibi)
#include "test_eibi.moc"
