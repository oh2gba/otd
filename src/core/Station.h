// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QVector>

// One schedule line: a station transmitting on a frequency during a time
// window. Every field holds what the source publishes, as it publishes it:
// codes stay codes ("NEu", "FI", HFCC's "18,27"), days keep the source's own
// notation. Readable names, the mode, the remarks text and the weekdays in
// one notation come from lookup tables when the entry is shown or searched
// (StationDb), never from the importer changing the data.
struct StationEntry
{
    qint64  id = 0;        // database row id, 0 when not stored
    QString source;        // "eibi", "hfcc", "aoki", or "user" for the personal list
    double  kHz = 0.0;
    int     startMin = 0;  // UTC, minutes since midnight (the source's HHMM)
    int     endMin = 1440; // 1440 == 24:00; 0000-0000 stays 0-0
    QString days;          // as published: EiBi "Mo-Fr", "1245", "15Sep"; HFCC "1234567"; Aoki digits (its own numbering)
    QString itu;           // home country / administration code
    QString station;       // station name, or HFCC's broadcaster code
    QString lang;          // language code (EiBi, HFCC) or name (Aoki, personal list)
    QString target;        // target area: EiBi code, HFCC CIRAF zones
    QString site;          // transmitter site: EiBi code ("/RUS-s" relayed), HFCC code, Aoki name
    int     persistence = 0;   // EiBi's persistence code
    QString startDate;     // valid from: EiBi DDMM, HFCC DDMMYY
    QString stopDate;      // valid until, likewise
    QString lastHeard;     // EiBi MMYY from "[0826]"
    QString remarks;       // the source's remarks or notes
    QString power;         // kW (HFCC, Aoki)
    QString azimuth;       // degrees, or "ND" (HFCC, Aoki)
    QString flag;          // Aoki's mark before the station: "*" or "x" (off air)
    QString mode;          // HFCC's modulation letter; the mode the listener gave (personal list)
};

using StationList = QVector<StationEntry>;

// Source id of the personal list kept in the database.
inline QString userSourceId() { return QStringLiteral("user"); }
