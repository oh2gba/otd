// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// The search box's language: words, "!word" exclusions, "field:value" and
// field:"two words". StationDb::search turns the terms into SQL.
namespace StationSearch
{
    struct Term
    {
        QString field;     // empty: anywhere
        QString value;
        bool negate = false;
    };
    // The fields a term can be limited to: station, language, country,
    // site, target, mode ("lang:" is language:).
    const QStringList& fields();
    // "bbc !china target:\"North Europe\"" -> its terms; "target:" alone
    // (a value still being typed) is no term yet
    QList<Term> parse(const QString& text);
}
