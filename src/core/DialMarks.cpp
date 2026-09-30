// SPDX-License-Identifier: GPL-3.0-or-later
#include "DialMarks.h"

#include <QtGlobal>

QVector<DialMark> groupDialMarks(const QVector<DialMark>& rows)
{
    QVector<DialMark> marks;
    for (const DialMark& row : rows)
    {
        if (!marks.isEmpty() && qFuzzyCompare(marks.last().kHz + 1.0, row.kHz + 1.0))
        {
            DialMark& m = marks.last();
            m.count += row.count;
            if (row.rank < m.rank)
            {
                m.rank = row.rank;
                m.name = row.name;
            }
            continue;
        }
        DialMark m = row;
        m.count = qMax(1, row.count);
        marks.push_back(m);
    }
    return marks;
}
