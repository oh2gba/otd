// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Station.h"
#include <QByteArray>
#include <QDate>
#include <QHash>

// Parsers for the files published by EiBi (http://www.eibispace.de/dx/).
namespace EibiParser
{
    struct ParseResult
    {
        StationList entries;
        int skippedLines = 0;
        QString error;      // non-empty when the data is unusable
    };

    // sked-Xzz.csv: semicolon separated, Latin-1, header line "kHz:75;..."
    ParseResult parseCsv(const QByteArray& data);

    // The mode as EiBi implies it: USB/LSB in the days column, "-CW"/"-TY"/
    // "-HF" language codes, DRM or fax in the name; broadcasters are AM.
    // Worked out when the entry is shown, not stored.
    QString guessMode(const StationEntry& e);

    // Lookup tables extracted from README.TXT.
    struct CodeTables
    {
        QHash<QString, QString> languages;  // "E"   -> "English: UK ..."
        QHash<QString, QString> countries;  // "RUS" -> "Russia"
        QHash<QString, QString> targets;    // "Eu"  -> "Europe ..."
        QHash<QString, QString> sites;      // "RUS/s" -> "Samara", "ARM/" -> "Gavar"
        bool isEmpty() const
        { return languages.isEmpty() && countries.isEmpty() && targets.isEmpty() && sites.isEmpty(); }
    };
    CodeTables parseReadme(const QByteArray& data);

    // Season file code for a date, e.g. "a26" (summer 2026) or "b25"
    // (winter 2025/2026). Seasons flip on the last Sunday of March/October.
    QString seasonCode(const QDate& date);
    QString previousSeason(const QString& season);
    QString nextSeason(const QString& season);
}
