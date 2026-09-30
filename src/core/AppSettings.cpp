// SPDX-License-Identifier: GPL-3.0-or-later
#include "AppSettings.h"
#include "StationDb.h"

namespace
{
const char* kKeys[] = {"rig.host", "rig.port", "rig.pollMs", "view.toleranceKHz", "view.onAirOnly",
                       "view.followRig", "view.alwaysOnTop", "data.refreshDays", "data.eibiUrl",
                       "data.hfccUrl", "data.aokiUrl", "data.eibiEnabled", "data.hfccEnabled",
                       "data.aokiEnabled", "rigctld.launch", "rigctld.path", "rigctld.model",
                       "rigctld.device", "rigctld.baud", "rigctld.extra", "view.ituRegion",
                       "update.check", "update.url", "view.scale", "view.table",
                       "view.player", "kiwi.receivers", "kiwi.current", "kiwi.volume",
                       "data.traficomUrl", "data.traficomEnabled", "view.scaleSpan", "kiwi.favourites", "kiwi.mode", "view.manualKHz",
                       "view.lastKHz"};
QString key(int i) { return QStringLiteral("settings.") + QLatin1String(kKeys[i]); }
bool toBool(const QString& v, bool fallback)
{
    if (v.isEmpty()) return fallback;
    return v == QLatin1String("1") || v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
}
} // namespace

void AppSettings::load(const StationDb* db)
{
    auto str = [db](int i, const QString& fallback) { return db->meta(key(i), fallback); };
    auto num = [&](int i, double fallback) {
        bool ok = false;
        const double v = str(i, QString()).toDouble(&ok);
        return ok ? v : fallback;
    };
    rigHost = str(0, rigHost);
    rigPort = int(num(1, rigPort));
    pollIntervalMs = int(num(2, pollIntervalMs));
    toleranceKHz = num(3, toleranceKHz);
    onAirOnly = toBool(str(4, QString()), onAirOnly);
    followRig = toBool(str(5, QString()), followRig);
    alwaysOnTop = toBool(str(6, QString()), alwaysOnTop);
    refreshDays = int(num(7, refreshDays));
    eibiUrl = str(8, eibiUrl);
    hfccUrl = str(9, hfccUrl);
    aokiUrl = str(10, aokiUrl);
    eibiEnabled = toBool(str(11, QString()), eibiEnabled);
    hfccEnabled = toBool(str(12, QString()), hfccEnabled);
    aokiEnabled = toBool(str(13, QString()), aokiEnabled);
    launchRigctld = toBool(str(14, QString()), launchRigctld);
    // rigctld.path is no longer offered in Settings; a value stored by an
    // older version must not keep overriding the included rigctld unseen
    rigctldPath.clear();
    rigModel = int(num(16, rigModel));
    rigDevice = str(17, rigDevice);
    rigBaud = int(num(18, rigBaud));
    rigctldExtra = str(19, rigctldExtra);
    ituRegion = qBound(1, int(num(20, ituRegion)), 3);
    updateCheck = toBool(str(21, QString()), updateCheck);
    updateUrl = str(22, updateUrl);
    // the page moved on 2026-09-27; settings written before that still
    // carry the old address (which redirects, but let us not rely on it)
    if (updateUrl == QLatin1String("https://onthedial.oh2gba.eu/version.php"))
        updateUrl = QStringLiteral("https://otd.oh2gba.eu/version.php");
    showScale = toBool(str(23, QString()), showScale);
    showTable = toBool(str(24, QString()), showTable);
    showPlayer = toBool(str(25, QString()), showPlayer);
    kiwiReceivers = str(26, QString()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    kiwiCurrent = str(27, kiwiCurrent);
    kiwiVolume = qBound(0, int(num(28, kiwiVolume)), 100);
    traficomUrl = str(29, traficomUrl);
    traficomEnabled = toBool(str(30, QString()), traficomEnabled);
    scaleSpanKHz = qBound(5.0, num(31, scaleSpanKHz), 20000.0);
    kiwiFavourites = str(32, QString()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    kiwiMode = str(33, kiwiMode);
    manualKHz = num(34, manualKHz) > 0.0 ? num(34, manualKHz) : AppSettings().manualKHz;
    lastKHz = qMax(0.0, num(35, lastKHz));
}

void AppSettings::save(StationDb* db) const
{
    const QString values[] = {
        rigHost, QString::number(rigPort), QString::number(pollIntervalMs),
        QString::number(toleranceKHz), QString::number(onAirOnly ? 1 : 0),
        QString::number(followRig ? 1 : 0), QString::number(alwaysOnTop ? 1 : 0),
        QString::number(refreshDays), eibiUrl, hfccUrl, aokiUrl,
        QString::number(eibiEnabled ? 1 : 0), QString::number(hfccEnabled ? 1 : 0),
        QString::number(aokiEnabled ? 1 : 0), QString::number(launchRigctld ? 1 : 0),
        rigctldPath, QString::number(rigModel), rigDevice, QString::number(rigBaud),
        rigctldExtra, QString::number(ituRegion), QString::number(updateCheck ? 1 : 0), updateUrl,
        QString::number(showScale ? 1 : 0), QString::number(showTable ? 1 : 0),
        QString::number(showPlayer ? 1 : 0), kiwiReceivers.join(QLatin1Char('\n')), kiwiCurrent,
        QString::number(kiwiVolume), traficomUrl, QString::number(traficomEnabled ? 1 : 0),
        QString::number(scaleSpanKHz), kiwiFavourites.join(QLatin1Char('\n')), kiwiMode,
        QString::number(manualKHz, 'f', 3), QString::number(lastKHz, 'f', 3)};
    for (int i = 0; i < int(sizeof(kKeys) / sizeof(kKeys[0])); ++i)
        db->setMeta(key(i), values[i]);
}
