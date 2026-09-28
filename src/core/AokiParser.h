// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Station.h"
#include <QByteArray>

// Parser for the Aoki / Bi Newsletter shortwave frequency list (NDXC,
// Nagoya DXers Circle): the fixed-width "xxa26.txt" file inside the season zip.
namespace AokiParser
{
    struct ParseResult
    {
        StationList entries;
        int skippedLines = 0;
        QString title;      // "A26 Shortwave Frequecy List  September 20  2026, ..."
        bool sundayFirst = true;   // day 1 in the days column is Sunday (else Monday)
        QString error;
    };

    ParseResult parse(const QByteArray& data);
}
