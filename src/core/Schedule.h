// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Station.h"
#include <QDateTime>

namespace Schedule
{
    enum class OnAir
    {
        Yes,        // time window, day and season all match
        Unknown,    // time matches but the schedule is irregular/alternative
        No,         // outside the time window or not on this day
        Inactive    // persistence 8 (EiBi: inactive entry)
    };

    // Evaluate the day-of-week/day-of-month rules of the "Days" column
    // for the given UTC date. Empty means daily.
    OnAir dayStatus(const QString& days, const QDate& utcDate);

    // Full evaluation: time window (with midnight wrap), validity dates,
    // season-only flags and the Days column.
    OnAir status(const StationEntry& entry, const QDateTime& utc);
    // weekdays: the entry's days in EiBi's notation (StationDb::weekdays
    // turns Aoki's own numbering into it)
    OnAir status(const StationEntry& entry, const QDateTime& utc, const QString& weekdays);

    QString statusText(OnAir s);
    QString timeWindow(const StationEntry& entry);   // "0500-0600" / "24h"
}
