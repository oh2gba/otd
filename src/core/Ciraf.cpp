// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ciraf.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

// the table is a resource of the static core library
static void initCoreResources()
{
    Q_INIT_RESOURCE(core);
}

namespace
{
struct Zone
{
    QString name;   // short, for the list
    QString area;   // what it covers, for the tooltip
};

const QHash<int, Zone>& zones()
{
    static const QHash<int, Zone> table = []() {
        QHash<int, Zone> t;
        initCoreResources();
        QFile f(QStringLiteral(":/ciraf.json"));
        if (!f.open(QIODevice::ReadOnly))
            return t;
        const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("zones")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it)
        {
            const QJsonObject z = it.value().toObject();
            t.insert(it.key().toInt(), {z.value(QStringLiteral("name")).toString(), z.value(QStringLiteral("area")).toString()});
        }
        return t;
    }();
    return table;
}

QString quadrantWord(const QString& q)
{
    static const QHash<QString, QString> words = {
        {QStringLiteral("N"), QStringLiteral("north")},     {QStringLiteral("S"), QStringLiteral("south")},
        {QStringLiteral("E"), QStringLiteral("east")},      {QStringLiteral("W"), QStringLiteral("west")},
        {QStringLiteral("NE"), QStringLiteral("northeast")}, {QStringLiteral("NW"), QStringLiteral("northwest")},
        {QStringLiteral("SE"), QStringLiteral("southeast")}, {QStringLiteral("SW"), QStringLiteral("southwest")}};
    return words.value(q, q);
}
}

QString Ciraf::text(const QString& list, bool details)
{
    static const QRegularExpression token(QStringLiteral("^(\\d+)(?:-(\\d+))?([NSEW]{1,2})?$"));
    const QHash<int, Zone>& table = zones();
    QStringList parts;
    QList<int> covered;
    bool known = false;
    for (const QString& raw : list.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString t = raw.trimmed();
        const QRegularExpressionMatch m = token.match(t);
        if (!m.hasMatch())
        {
            parts << t;
            continue;
        }
        const int first = m.captured(1).toInt();
        const int last = m.captured(2).isEmpty() ? first : m.captured(2).toInt();
        const QString quadrant = m.captured(3).isEmpty() ? QString() : quadrantWord(m.captured(3));
        for (int z = first; z <= last && z - first < 100; ++z)
        {
            const auto it = table.constFind(z);
            if (it == table.constEnd())
            {
                parts << QString::number(z) + m.captured(3);
                continue;
            }
            known = true;
            if (quadrant.isEmpty() && !covered.contains(z))
                covered << z;
            QString part;
            if (details)
                part = quadrant.isEmpty() ? QStringLiteral("%1: %2").arg(z).arg(it->area)
                                          : QStringLiteral("%1 %2: %3").arg(z).arg(quadrant, it->area);
            else
                part = quadrant.isEmpty() ? it->name : QStringLiteral("%1 (%2)").arg(it->name, quadrant);
            if (!parts.contains(part))   // the Antarctic zones are all "Antarctica"
                parts << part;
        }
    }
    if (!known)
        return QString();
    if (!details && covered.size() == table.size())
        return QStringLiteral("worldwide");
    return parts.join(details ? QStringLiteral("; ") : QStringLiteral(", "));
}
