// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/KiwiDirectory.h"
#include "core/StationDb.h"
#include "core/TraficomSource.h"

#include <QTemporaryDir>
#include <QtTest>

class TestTraficom : public QObject
{
    Q_OBJECT
private slots:
    void parsesODataRows()
    {
        const QByteArray json = R"({"@odata.context":"x","value":[
          {"Sub_band_lower_limit__Hz_":9400000,"Sub_band_upper_limit__Hz_":9500000,
           "Services_in_Finland":"BROADCASTING","Sub_band_usage":"Broadcasting",
           "Additional_information":"","Mode_of_traffic":"","Class_of_emission":"",
           "Comment":"Restricted fixed service possible (RR 5.146)."},
          {"Sub_band_lower_limit__Hz_":4217750,"Sub_band_upper_limit__Hz_":4219250,
           "Services_in_Finland":"MARITIME MOBILE","Sub_band_usage":"Data service",
           "Additional_information":null,"Mode_of_traffic":"Simplex","Class_of_emission":"",
           "Comment":"More detailed instructions for use in RR App 17."},
          {"Sub_band_lower_limit__Hz_":100,"Sub_band_upper_limit__Hz_":0,
           "Services_in_Finland":"broken","Sub_band_usage":""}
        ]})";
        QString err;
        const QList<TraficomSource::Row> rows = TraficomSource::parse(json, &err);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].lowKHz, 9400.0);
        QCOMPARE(rows[0].highKHz, 9500.0);
        QCOMPARE(rows[0].usage, QStringLiteral("Broadcasting"));
        QCOMPARE(rows[1].service, QStringLiteral("MARITIME MOBILE"));
        QCOMPARE(rows[1].mode, QStringLiteral("Simplex"));
        QVERIFY(TraficomSource::parse("not json", &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(TraficomSource::hfQuery().contains(QStringLiteral("30000000")));
    }

    void storesAndLooksUpAllocations()
    {
        QTemporaryDir dir;
        StationDb db(dir.filePath("stations.db"));
        QVERIFY2(db.open(), qPrintable(db.lastError()));
        QList<StationDb::Allocation> list;
        StationDb::Allocation a;
        a.lowKHz = 9400; a.highKHz = 9500; a.service = "BROADCASTING"; a.usage = "Broadcasting";
        list << a;
        a.lowKHz = 9500; a.highKHz = 9900; list << a;
        a.lowKHz = 4217.75; a.highKHz = 4219.25; a.service = "MARITIME MOBILE"; a.usage = "Data service";
        list << a;
        QVERIFY2(db.replaceAllocations("traficom", list), qPrintable(db.lastError()));
        QCOMPARE(db.allocationCount("traficom"), 3);
        QCOMPARE(db.allocationsAt(9500.0).size(), 2);          // both edges are inclusive
        QCOMPARE(db.allocationsAt(4218.0).size(), 1);
        QCOMPARE(db.allocationsAt(4218.0).first().usage, QStringLiteral("Data service"));
        QCOMPARE(db.allocationsAt(100.0).size(), 0);
        QVERIFY(db.replaceAllocations("traficom", {}));
        QCOMPARE(db.allocationCount("traficom"), 0);
    }

    void parsesKiwiDirectory()
    {
        const QByteArray js = "// comment\nvar kiwisdr_com =\n[\n"
                              "\t{\"url\":\"http://a.example:8073\",\"name\":\"A  SDR\",\"loc\":\"Loppi, Finland\","
                              "\"users\":\"4\",\"users_max\":\"7\",\"bands\":\"0-30000000\",\"offline\":\"no\",\"grid\":\"KP20\",},\n"
                              "\t{\"url\":\"http://b.example:8073\",\"name\":\"B\",\"loc\":\"Bergen, Norway\","
                              "\"users\":\"0\",\"users_max\":\"4\",\"bands\":\"10000-32000000\",\"offline\":\"yes\",},\n"
                              "];\n";
        QString err;
        const QList<KiwiDirectory::Receiver> list = KiwiDirectory::parse(js, &err);
        QCOMPARE(list.size(), 2);
        QCOMPARE(list[0].location, QStringLiteral("Bergen, Norway"));   // sorted by place
        QVERIFY(list[0].offline);
        QCOMPARE(list[1].name, QStringLiteral("A SDR"));
        QCOMPARE(list[1].users, 4);
        QCOMPARE(list[1].usersMax, 7);
        QCOMPARE(list[1].highKHz, 30000.0);
        QVERIFY(KiwiDirectory::parse("nothing here", &err).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestTraficom)
#include "test_traficom.moc"
