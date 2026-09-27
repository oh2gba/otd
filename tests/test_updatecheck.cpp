// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/UpdateCheck.h"
#include <QtTest>

class TestUpdateCheck : public QObject
{
    Q_OBJECT
private slots:
    void versionOrder()
    {
        QCOMPARE(UpdateCheck::compareVersions("1.0.2", "1.0.2"), 0);
        QCOMPARE(UpdateCheck::compareVersions("1.0.10", "1.0.9"), 1);
        QCOMPARE(UpdateCheck::compareVersions("1.1", "1.0.9"), 1);
        QCOMPARE(UpdateCheck::compareVersions("1.0", "1.0.0"), 0);
        QCOMPARE(UpdateCheck::compareVersions("2.0.0", "10.0.0"), -1);
        QCOMPARE(UpdateCheck::compareVersions("1.0.0-rc1", "1.0.0"), -1);
        QCOMPARE(UpdateCheck::compareVersions("1.0.0", "1.0.0-rc1"), 1);
        QCOMPARE(UpdateCheck::compareVersions(" 1.2.3 ", "1.2.3"), 0);
    }

    void parsing()
    {
        auto r = UpdateCheck::parseResponse(
            "{\"version\":\"1.1.0\",\"url\":\"https://otd.oh2gba.eu/#download\",\"message\":\"Aoki list moved\"}",
            "1.0.2");
        QVERIFY(r.valid);
        QVERIFY(r.newer);
        QCOMPARE(r.latest, QStringLiteral("1.1.0"));
        QCOMPARE(r.url, QStringLiteral("https://otd.oh2gba.eu/#download"));
        QCOMPARE(r.message, QStringLiteral("Aoki list moved"));

        r = UpdateCheck::parseResponse("{\"version\":\"1.0.2\",\"url\":\"\",\"message\":\"\"}", "1.0.2");
        QVERIFY(r.valid);
        QVERIFY(!r.newer);

        // only https links are accepted, odd versions are rejected
        r = UpdateCheck::parseResponse("{\"version\":\"1.0.3\",\"url\":\"http://evil.example/x\"}", "1.0.2");
        QVERIFY(r.valid);
        QVERIFY(r.url.isEmpty());
        r = UpdateCheck::parseResponse("{\"version\":\"<b>1</b>\"}", "1.0.2");
        QVERIFY(!r.valid);
        r = UpdateCheck::parseResponse("not json", "1.0.2");
        QVERIFY(!r.valid);
    }

    void platform()
    {
        const QString p = UpdateCheck::platformName();
        QVERIFY(p == "linux" || p == "linux-flatpak" || p == "linux-appimage" || p == "windows" || p == "macos");
    }
};

QTEST_GUILESS_MAIN(TestUpdateCheck)
#include "test_updatecheck.moc"
