// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "StationDb.h"
#include <QList>
#include <QString>

class BandPlan;

// What the header says about the tuned frequency's allocation: the built-in
// plan's band, or, where a national table (Traficom) has rows, those.
struct BandLine
{
    QString text;      // "49 m broadcast", "HF broadcasting (broadcasting) · FIXED"; empty: nothing listed
    QString kind;      // "broadcast", "amateur", "aero", "maritime", "time", "beacon", "informal", "other"; empty with the text
    QString tooltip;   // the national table's rows, one per line; empty when the plan answered

    static BandLine describe(const BandPlan& plan, int region, double kHz,
                             const QList<StationDb::Allocation>& national);
};
