// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Station.h"
#include <QString>

class StationDb;

// What the list shows for an entry: its fields through the lookup tables
// the database holds (EiBi's README, HFCC's reference files, Aoki's day
// numbering, the CIRAF zones). The rows themselves keep what the source
// published. StationDb::names() is the one to use.
class StationNames
{
public:
    explicit StationNames(const StationDb* db) : m_db(db) {}

    QString stationOf(const StationEntry& e) const;   // HFCC: the broadcaster's name
    QString languageOf(const StationEntry& e, bool details = false) const;
    QString countryOf(const StationEntry& e) const;
    QString siteOf(const StationEntry& e) const;
    QString targetOf(const StationEntry& e, bool details = false) const;
    static QString modeOf(const StationEntry& e);      // from the source's hints; empty when unknown
    static QString remarksOf(const StationEntry& e);   // power, azimuth and the source's remarks
    // the days as Monday-first digits (or EiBi's own notation) for the
    // schedule; daysOf leaves "every day" empty, for the list
    QString weekdays(const StationEntry& e) const;
    QString daysOf(const StationEntry& e) const;

    // EiBi codes as names. languageText: "Finnish" for FI, "French,
    // English" for F,E; with details the whole description ("Finnish:
    // Finland (5m)"). targetText: "Europe" for Eu, "North Europe" for NEu
    // (from the README's "N.." and "Eu"), a country for a country code.
    // Both empty when the tables know none of it.
    QString languageText(const QString& codes, bool details = false) const;
    QString targetText(const QString& codes, bool details = false) const;
    QString countryName(const QString& itu) const;   // EiBi's country table; the code when unknown
    // Human readable EiBi transmitter site: "/RUS-s" -> "Samara (Russia)".
    QString siteName(const QString& homeItu, const QString& siteCode) const;

private:
    QString code(const QString& source, const QString& kind, const QString& code) const;
    const StationDb* m_db;
};
