// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVector>

// One label on the dial scale: the frequency, the station printed for it
// and how many entries share the frequency.
struct DialMark
{
    double kHz = 0.0;
    QString name;     // the station shown for this frequency
    int rank = 2;     // 0 on air, 1 maybe, 2 off, 3 inactive
    int count = 1;    // entries on this frequency
};

// The list's rows, in frequency order, one mark each, folded to one mark
// per frequency: the best entry (on air first) gives the name, the count
// tells how many there are.
QVector<DialMark> groupDialMarks(const QVector<DialMark>& rows);
