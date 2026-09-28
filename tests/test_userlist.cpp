// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/UserList.h"
#include <QtTest>

class TestUserList : public QObject
{
    Q_OBJECT
private slots:
    void roundTrip()
    {
        StationEntry a;
        a.source = userSourceId();
        a.kHz = 4625;
        a.startMin = 0;
        a.endMin = 1440;
        a.station = "The Buzzer; heard well";
        a.mode = "AM";
        a.itu = "RUS";
        a.remarks = "buzz buzz";
        StationEntry b;
        b.kHz = 8992.5;
        b.startMin = 22 * 60 + 30;
        b.endMin = 60;
        b.days = "Mo-Fr";
        b.station = "HFGCS";
        b.mode = "USB";
        b.lang = "English";
        b.site = "Andrews";

        const QByteArray csv = UserList::toCsv({a, b});
        QVERIFY(csv.startsWith("kHz;Start;End;Days;Station;Mode;Country;Language;Site;Notes\n"));
        int skipped = -1;
        const StationList back = UserList::fromCsv(csv + "garbage line\n", &skipped);
        QCOMPARE(skipped, 1);
        QCOMPARE(back.size(), 2);
        QCOMPARE(back[0].kHz, 4625.0);
        QCOMPARE(back[0].endMin, 1440);
        QCOMPARE(back[0].station, QStringLiteral("The Buzzer, heard well"));   // ';' sanitised
        QCOMPARE(back[0].source, userSourceId());
        QCOMPARE(back[0].remarks, QStringLiteral("buzz buzz"));
        QCOMPARE(back[1].kHz, 8992.5);
        QCOMPARE(back[1].startMin, 1350);
        QCOMPARE(back[1].endMin, 60);
        QCOMPARE(back[1].days, QStringLiteral("Mo-Fr"));
        QCOMPARE(back[1].mode, QStringLiteral("USB"));
        QCOMPARE(back[1].site, QStringLiteral("Andrews"));
    }
};

QTEST_GUILESS_MAIN(TestUserList)
#include "test_userlist.moc"
