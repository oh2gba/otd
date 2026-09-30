// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

class StationDb;

// Everything the listener can set, kept in the database's meta table (one
// key per field). The desktop's Settings dialog edits a copy of it; the
// window and the online receiver keep the rest of it current.
struct AppSettings
{
    QString rigHost = QStringLiteral("localhost");
    int rigPort = 4532;
    int pollIntervalMs = 500;
    double toleranceKHz = 5.0;
    int ituRegion = 1;      // 1 Europe/Africa, 2 Americas, 3 Asia/Pacific
    bool updateCheck = true;
    QString updateUrl = QStringLiteral("https://otd.oh2gba.eu/version.php");
    int refreshDays = 7;
    QString eibiUrl = QStringLiteral("http://www.eibispace.de/dx/");
    QString hfccUrl = QStringLiteral("http://www.hfcc.org/data/");
    QString aokiUrl = QStringLiteral("http://www1.s2.starcat.ne.jp/ndxc/");
    bool eibiEnabled = true;
    bool hfccEnabled = true;
    bool aokiEnabled = true;
    // national allocation table: Traficom (Finland), CC BY 4.0
    QString traficomUrl = QStringLiteral("https://opendata.traficom.fi/api/v13/Taajuusjakotaulukko");
    bool traficomEnabled = true;
    // "start rigctld for me"
    bool launchRigctld = false;
    QString rigctldPath;
    int rigModel = 1;
    QString rigDevice;
    int rigBaud = 0;
    QString rigctldExtra;
    bool onAirOnly = false;
    bool followRig = true;
    bool alwaysOnTop = false;
    bool showScale = true;
    bool showTable = true;
    bool showPlayer = true;
    double scaleSpanKHz = 10.0;    // width of the dial scale (the wheel steps 10, 20, 50, 100 ... kHz)
    QStringList kiwiReceivers;
    QStringList kiwiFavourites;
    QString kiwiMode = QStringLiteral("AM");   // mode when no rig is followed
    double manualKHz = 6070.0;                 // last frequency set by hand; 49 m as a start
    QString kiwiCurrent;
    int kiwiVolume = 70;

    void load(const StationDb* db);
    void save(StationDb* db) const;
};
