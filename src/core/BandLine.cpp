// SPDX-License-Identifier: GPL-3.0-or-later
#include "BandLine.h"
#include "BandPlan.h"

#include <QCoreApplication>
#include <QStringList>

BandLine BandLine::describe(const BandPlan& plan, int region, double kHz,
                            const QList<StationDb::Allocation>& national)
{
    BandLine line;
    if (kHz <= 0.0)
        return line;
    line.text = plan.describe(kHz, region);
    const QVector<BandPlan::Band> bands = plan.lookup(kHz, region);
    line.kind = bands.isEmpty() ? QString() : bands.first().kind;
    if (national.isEmpty())
        return line;

    // the national table replaces the built-in plan wherever it has a row
    QStringList names;
    for (const StationDb::Allocation& a : national)
    {
        QString n = a.usage.isEmpty() ? a.service : a.usage;
        if (!a.service.isEmpty() && a.service.compare(n, Qt::CaseInsensitive) != 0)
            n += QStringLiteral(" (%1)").arg(a.service.toLower());
        if (!names.contains(n))
            names << n;
    }
    if (names.size() > 3)
    {
        const int more = names.size() - 3;
        names = names.mid(0, 3);
        names << QCoreApplication::translate("BandLine", "+%1 more").arg(more);
    }
    line.text = names.join(QStringLiteral(" · "));
    const QString svc = national.first().service.toUpper();
    line.kind = svc.contains(QLatin1String("BROADCAST")) ? QStringLiteral("broadcast")
              : svc.contains(QLatin1String("AMATEUR")) ? QStringLiteral("amateur")
              : svc.contains(QLatin1String("AERONAUTICAL")) ? QStringLiteral("aero")
              : svc.contains(QLatin1String("MARITIME")) ? QStringLiteral("maritime")
              : svc.contains(QLatin1String("STANDARD FREQ")) ? QStringLiteral("time")
              : svc.contains(QLatin1String("RADIONAVIGATION")) ? QStringLiteral("beacon")
              : QStringLiteral("other");
    QString tip = QCoreApplication::translate("BandLine", "Traficom allocation table:\n");
    for (const StationDb::Allocation& a : national)
    {
        tip += QStringLiteral("%1 - %2 kHz: %3").arg(a.lowKHz, 0, 'f', 3).arg(a.highKHz, 0, 'f', 3)
                   .arg(a.usage.isEmpty() ? a.service : a.usage);
        if (!a.service.isEmpty() && a.service.compare(a.usage, Qt::CaseInsensitive) != 0)
            tip += QStringLiteral(" [%1]").arg(a.service);
        if (!a.mode.isEmpty())
            tip += QStringLiteral(", %1").arg(a.mode);
        if (!a.info.isEmpty())
            tip += QStringLiteral(". %1").arg(a.info);
        if (!a.comment.isEmpty())
            tip += QStringLiteral(" %1").arg(a.comment);
        tip += QLatin1Char('\n');
    }
    line.tooltip = tip.trimmed();
    return line;
}
