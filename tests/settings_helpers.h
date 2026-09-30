// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by the settings tests: a value for every field, and a comparison
// of every field. Keep both in step with AppSettings.
#pragma once
#include "core/AppSettings.h"
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
