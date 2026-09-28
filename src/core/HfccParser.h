// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Station.h"
#include <QByteArray>
#include <QHash>

// Parser for the HFCC public schedule files (http://www.hfcc.org/data/):
// fixed-width "Xnnall00.txt" plus reference tables site/broadcas/language/admin.
namespace HfccParser
{
    struct Tables
    {
        struct Site
        {
            QString name;
            QString adm;   // ITU code of the site's country
        };
        QHash<QString, Site> sites;             // "GAL" -> Galbeni, ROU
        QHash<QString, QString> broadcasters;   // "RRO" -> Radio Romania International
        QHash<QString, QString> languages;      // "Ron" -> Romanian
        QHash<QString, QString> admins;         // "ROU" -> Romania
    };

    struct ParseResult
    {
        StationList entries;
        int skippedLines = 0;
        QString error;
    };

    // Reference tables: "CODE Name ..." lines, ';' comments. The site table
    // also carries the administration in fixed columns.
    Tables parseTables(const QByteArray& site, const QByteArray& broadcasters,
                       const QByteArray& languages, const QByteArray& admins);

    // The schedule lines as published; the codes are resolved with the
    // tables when shown, not here.
    ParseResult parseSchedule(const QByteArray& data);
}
