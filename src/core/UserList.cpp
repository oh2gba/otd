// SPDX-License-Identifier: GPL-3.0-or-later
#include "UserList.h"
#include "Schedule.h"

#include <QStringList>

namespace
{
QString clean(QString s)
{
    return s.replace(QLatin1Char(';'), QLatin1Char(',')).replace(QLatin1Char('\n'), QLatin1Char(' ')).trimmed();
}
QString hhmm(int minutes)
{
    return QStringLiteral("%1%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0'));
}
int fromHHMM(const QString& s, bool* ok)
{
    if (s.size() != 4)
    {
        *ok = false;
        return 0;
    }
    bool okH = false, okM = false;
    const int h = s.left(2).toInt(&okH), m = s.mid(2).toInt(&okM);
    *ok = okH && okM && h >= 0 && h <= 24 && m >= 0 && m < 60;
    return h * 60 + m;
}
} // namespace

QByteArray UserList::toCsv(const StationList& entries)
{
    QString out = QStringLiteral("kHz;Start;End;Days;Station;Mode;Country;Language;Site;Notes\n");
    for (const StationEntry& e : entries)
    {
        out += QStringLiteral("%1;%2;%3;%4;%5;%6;%7;%8;%9;%10\n")
                   .arg(QString::number(e.kHz, 'f', 3), hhmm(e.startMin), hhmm(e.endMin), clean(e.days),
                        clean(e.station), clean(e.mode), clean(e.itu), clean(e.lang), clean(e.site),
                        clean(e.remarks));
    }
    return out.toUtf8();
}

StationList UserList::fromCsv(const QByteArray& data, int* skipped)
{
    StationList out;
    int bad = 0;
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    for (const QString& raw : lines)
    {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("kHz;")) || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList f = line.split(QLatin1Char(';'));
        if (f.size() < 5)
        {
            ++bad;
            continue;
        }
        StationEntry e;
        e.source = userSourceId();
        e.persistence = 1;
        bool ok = false;
        e.kHz = f[0].trimmed().toDouble(&ok);
        bool okS = false, okE = false;
        e.startMin = fromHHMM(f[1].trimmed(), &okS);
        e.endMin = fromHHMM(f[2].trimmed(), &okE);
        if (!ok || e.kHz <= 0.0 || !okS || !okE)
        {
            ++bad;
            continue;
        }
        e.days = f[3].trimmed();
        e.station = f[4].trimmed();
        e.mode = f.value(5).trimmed().toUpper();
        e.itu = f.value(6).trimmed();
        e.lang = f.value(7).trimmed();
        e.site = f.value(8).trimmed();
        e.remarks = f.value(9).trimmed();
        out.push_back(e);
    }
    if (skipped)
        *skipped = bad;
    return out;
}
