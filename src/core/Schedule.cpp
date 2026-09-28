// SPDX-License-Identifier: GPL-3.0-or-later
#include "Schedule.h"
#include "EibiParser.h"

#include <QRegularExpression>
#include <QStringList>

namespace
{
const QStringList kDayNames = {
    QStringLiteral("Mo"), QStringLiteral("Tu"), QStringLiteral("We"), QStringLiteral("Th"),
    QStringLiteral("Fr"), QStringLiteral("Sa"), QStringLiteral("Su")};
const QStringList kMonthNames = {
    QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"), QStringLiteral("Apr"),
    QStringLiteral("May"), QStringLiteral("Jun"), QStringLiteral("Jul"), QStringLiteral("Aug"),
    QStringLiteral("Sep"), QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec")};

int dayIndex(const QString& name)      // 1..7, or 0 when not a day name
{
    const int i = kDayNames.indexOf(name);
    return i < 0 ? 0 : i + 1;
}

// Parse "Mo-Fr", "Tu,Fr", "SaSu", "We-Mo", "156". Returns false when the
// string contains something that is not a plain day specification.
bool parseDayMask(const QString& spec, quint8* mask)
{
    quint8 result = 0;
    static const QRegularExpression digitsRe(QStringLiteral("^[1-7]+$"));
    if (digitsRe.match(spec).hasMatch())
    {
        for (const QChar c : spec)
            result |= quint8(1u << (c.digitValue() - 1));
        *mask = result;
        return true;
    }

    const QStringList parts = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    for (const QString& part : parts)
    {
        const int dash = part.indexOf(QLatin1Char('-'));
        if (dash > 0)
        {
            const int from = dayIndex(part.left(dash));
            const int to = dayIndex(part.mid(dash + 1));
            if (from == 0 || to == 0)
                return false;
            int d = from;
            for (;;)
            {
                result |= quint8(1u << (d - 1));
                if (d == to)
                    break;
                d = (d % 7) + 1;
            }
        }
        else
        {
            if (part.size() % 2 != 0)
                return false;
            for (int i = 0; i < part.size(); i += 2)
            {
                const int d = dayIndex(part.mid(i, 2));
                if (d == 0)
                    return false;
                result |= quint8(1u << (d - 1));
            }
        }
    }
    *mask = result;
    return true;
}

bool isDay(quint8 mask, const QDate& date)
{
    return mask & quint8(1u << (date.dayOfWeek() - 1));
}
} // namespace

Schedule::OnAir Schedule::dayStatus(const QString& daysIn, const QDate& date)
{
    const QString days = daysIn.trimmed();
    if (days.isEmpty())
        return OnAir::Yes;

    const QString lower = days.toLower();
    if (lower == QLatin1String("lsb") || lower == QLatin1String("usb"))
        return OnAir::Yes;

    static const QStringList irregular = {
        QStringLiteral("irr"), QStringLiteral("alt"), QStringLiteral("tent"), QStringLiteral("test"),
        QStringLiteral("harm"), QStringLiteral("imod"), QStringLiteral("haj"), QStringLiteral("ram")};
    if (irregular.contains(lower))
        return OnAir::Unknown;

    // "15Sep": a single date
    static const QRegularExpression dateRe(QStringLiteral("^(\\d{1,2})([A-Z][a-z]{2})$"));
    QRegularExpressionMatch m = dateRe.match(days);
    if (m.hasMatch())
    {
        const int month = kMonthNames.indexOf(m.captured(2)) + 1;
        if (month == 0)
            return OnAir::Unknown;
        return (date.day() == m.captured(1).toInt() && date.month() == month) ? OnAir::Yes
                                                                              : OnAir::No;
    }

    // "1.Sa": n-th weekday of the month;  "1WeFr": n-th weekday plus a repeat
    static const QRegularExpression nthRe(QStringLiteral("^([1-5])\\.?([A-Z][a-z])([A-Z][a-z])?$"));
    m = nthRe.match(days);
    if (m.hasMatch())
    {
        const int n = m.captured(1).toInt();
        const int d = dayIndex(m.captured(2));
        if (d == 0)
            return OnAir::Unknown;
        if (date.dayOfWeek() == d && (date.day() - 1) / 7 + 1 == n)
            return OnAir::Yes;
        if (!m.captured(3).isEmpty())
            return dayIndex(m.captured(3)) == date.dayOfWeek() ? OnAir::Unknown : OnAir::No;
        return OnAir::No;
    }

    // "Last7": last given weekday of the month
    static const QRegularExpression lastRe(QStringLiteral("^Last([1-7])$"));
    m = lastRe.match(days);
    if (m.hasMatch())
    {
        const int d = m.captured(1).toInt();
        return (date.dayOfWeek() == d && date.day() + 7 > date.daysInMonth()) ? OnAir::Yes
                                                                              : OnAir::No;
    }

    // "MF-15": Monday to Friday up to the 15th
    static const QRegularExpression mfRe(QStringLiteral("^MF-(\\d{1,2})$"));
    m = mfRe.match(days);
    if (m.hasMatch())
        return (date.dayOfWeek() <= 5 && date.day() <= m.captured(1).toInt()) ? OnAir::Yes
                                                                              : OnAir::No;

    // "altFr": alternating weeks
    if (days.startsWith(QLatin1String("alt")))
    {
        quint8 mask = 0;
        if (parseDayMask(days.mid(3), &mask))
            return isDay(mask, date) ? OnAir::Unknown : OnAir::No;
        return OnAir::Unknown;
    }

    quint8 mask = 0;
    if (parseDayMask(days, &mask))
        return isDay(mask, date) ? OnAir::Yes : OnAir::No;

    return OnAir::Unknown;
}

Schedule::OnAir Schedule::status(const StationEntry& e, const QDateTime& utcIn)
{
    return status(e, utcIn, e.days);
}

Schedule::OnAir Schedule::status(const StationEntry& e, const QDateTime& utcIn, const QString& weekdays)
{
    // EiBi's persistence 8, Aoki's "x" mark: listed, but off air
    if (e.persistence % 90 == 8 || (e.source == QLatin1String("aoki") && e.flag == QLatin1String("x")))
        return OnAir::Inactive;

    const QDateTime utc = utcIn.toUTC();
    const int now = utc.time().hour() * 60 + utc.time().minute();
    QDate day = utc.date();

    bool inTime = true;
    if (e.startMin == 0 && e.endMin >= 1440)
        inTime = true;
    else if (e.startMin < e.endMin)
        inTime = now >= e.startMin && now < e.endMin;
    else if (e.startMin > e.endMin)
    {
        inTime = now >= e.startMin || now < e.endMin;
        if (inTime && now < e.endMin)
            day = day.addDays(-1);   // broadcast started yesterday
    }
    if (!inTime)
        return OnAir::No;

    // Season-only entries (4 = winter only, 5 = summer only)
    const int p = e.persistence % 90;
    if (p == 4 || p == 5)
    {
        const bool summer = EibiParser::seasonCode(day).startsWith(QLatin1Char('a'));
        if ((p == 4 && summer) || (p == 5 && !summer))
            return OnAir::No;
    }

    // Validity dates with the year, DDMMYY (HFCC)
    if (e.startDate.size() == 6 && e.stopDate.size() == 6)
    {
        auto date = [](const QString& d) {
            return QDate(2000 + d.mid(4, 2).toInt(), d.mid(2, 2).toInt(), d.left(2).toInt());
        };
        const QDate from = date(e.startDate), to = date(e.stopDate);
        if (from.isValid() && to.isValid() && (day < from || day > to))
            return OnAir::No;
    }
    // Validity dates, DDMM
    if (e.startDate.size() == 4 && e.stopDate.size() == 4)
    {
        const int from = e.startDate.mid(2).toInt() * 100 + e.startDate.left(2).toInt();
        const int to = e.stopDate.mid(2).toInt() * 100 + e.stopDate.left(2).toInt();
        const int key = day.month() * 100 + day.day();
        const bool inRange = (from <= to) ? (key >= from && key <= to)
                                          : (key >= from || key <= to);
        if (!inRange)
            return OnAir::No;
    }

    return dayStatus(weekdays, day);
}

QString Schedule::statusText(OnAir s)
{
    switch (s)
    {
    case OnAir::Yes:      return QStringLiteral("on air");
    case OnAir::Unknown:  return QStringLiteral("maybe");
    case OnAir::No:       return QStringLiteral("off");
    case OnAir::Inactive: return QStringLiteral("inactive");
    }
    return QString();
}

QString Schedule::timeWindow(const StationEntry& e)
{
    if ((e.startMin == 0 && e.endMin >= 1440) || e.startMin == e.endMin)   // 0000-2400, 0000-0000
        return QStringLiteral("24h");
    return QStringLiteral("%1%2-%3%4")
        .arg(e.startMin / 60, 2, 10, QChar('0')).arg(e.startMin % 60, 2, 10, QChar('0'))
        .arg(e.endMin / 60, 2, 10, QChar('0')).arg(e.endMin % 60, 2, 10, QChar('0'));
}
