// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationSearch.h"

#include <QRegularExpression>

const QStringList& StationSearch::fields()
{
    static const QStringList fields = {QStringLiteral("station"), QStringLiteral("language"),
                                       QStringLiteral("country"), QStringLiteral("site"),
                                       QStringLiteral("target"), QStringLiteral("mode")};
    return fields;
}

QList<StationSearch::Term> StationSearch::parse(const QString& text)
{
    // word | !word | field:value | field:"two words" (the closing quote may
    // still be missing while it is typed); an unknown field is a plain word
    static const QRegularExpression token(
        QStringLiteral("(!?)([A-Za-z]+):\"([^\"]*)\"?|(!?)([A-Za-z]+):(\\S*)|(\\S+)"));
    QList<Term> terms;
    auto it = token.globalMatch(text);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        Term t;
        const bool quoted = m.capturedStart(2) >= 0;
        if (quoted || m.capturedStart(5) >= 0)
        {
            QString field = m.captured(quoted ? 2 : 5).toLower();
            if (field == QLatin1String("lang"))
                field = QStringLiteral("language");
            if (fields().contains(field))
            {
                t.negate = !m.captured(quoted ? 1 : 4).isEmpty();
                t.field = field;
                t.value = m.captured(quoted ? 3 : 6).trimmed();
                if (!t.value.isEmpty())   // "target:" alone: a value is still being picked
                    terms << t;
                continue;
            }
        }
        QString word = m.captured(0);
        if (word.startsWith(QLatin1Char('!')))
        {
            t.negate = true;
            word.remove(0, 1);
        }
        if (word.isEmpty())
            continue;
        t.value = word;
        terms << t;
    }
    return terms;
}
