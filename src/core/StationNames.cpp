// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationNames.h"
#include "Ciraf.h"
#include "EibiParser.h"
#include "StationDb.h"

#include <QStringList>
#include <algorithm>

QString StationNames::code(const QString& source, const QString& kind, const QString& code) const
{
    return m_db->code(source, kind, code);
}

QString StationNames::languageText(const QString& codes, bool details) const
{
    QStringList names;
    bool known = false;
    for (const QString& part : codes.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString c = part.trimmed();
        QString name = code(QStringLiteral("eibi"), QStringLiteral("lang"), c);
        if (name.isEmpty())
        {
            names << c;
            continue;
        }
        known = true;
        // "Finnish: Finland (5m)" -> "Finnish"; the countries after the
        // colon are where it is spoken, not the language
        const int colon = name.indexOf(QLatin1Char(':'));
        names << (colon > 0 && !details ? name.left(colon) : name).trimmed();
    }
    // the descriptions have commas of their own
    return known ? names.join(details ? QStringLiteral("; ") : QStringLiteral(", ")) : QString();
}

QString StationNames::targetText(const QString& codes, bool details) const
{
    const QString eibi = QStringLiteral("eibi"), targets = QStringLiteral("target");
    // "Europe (often including North Africa/Middle East)" -> "Europe" in the list
    auto shortName = [](const QString& name) {
        const int paren = name.indexOf(QStringLiteral(" ("));
        return paren > 0 ? name.left(paren) : name;
    };
    QStringList names;
    bool known = false;
    for (const QString& part : codes.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString c = part.trimmed();
        QString name = code(eibi, targets, c);
        if (!name.isEmpty())
            name = details ? name : shortName(name);
        else if (!code(eibi, QStringLiteral("country"), c).isEmpty())
            name = code(eibi, QStringLiteral("country"), c);
        else if (c.size() >= 3 && QStringLiteral("NSEWC").contains(c.at(0)))
        {
            // EiBi's README gives the parts: "N.." is "North ..", "Eu" is
            // Europe, so "NEu" is North Europe
            const QString prefix = code(eibi, targets, c.left(1) + QStringLiteral(".."));
            const QString base = code(eibi, targets, c.mid(1));
            if (prefix.contains(QStringLiteral("..")) && !base.isEmpty())
                name = QString(prefix).replace(QStringLiteral(".."), shortName(base)).simplified();
        }
        if (name.isEmpty())
        {
            names << c;
            continue;
        }
        known = true;
        names << name;
    }
    return known ? names.join(details ? QStringLiteral("; ") : QStringLiteral(", ")) : QString();
}

QString StationNames::countryName(const QString& itu) const
{
    const QString name = code(QStringLiteral("eibi"), QStringLiteral("country"), itu);
    return name.isEmpty() ? itu : name;
}

QString StationNames::siteName(const QString& homeItu, const QString& siteCode) const
{
    QString itu = homeItu;
    QString c = siteCode;
    bool relayed = false;
    if (c.startsWith(QLatin1Char('/')))
    {
        relayed = true;
        const int dash = c.indexOf(QLatin1Char('-'));
        itu = dash > 0 ? c.mid(1, dash - 1) : c.mid(1);
        c = dash > 0 ? c.mid(dash + 1) : QString();
    }
    QString name = code(QStringLiteral("eibi"), QStringLiteral("site"), itu + QLatin1Char('/') + c);
    if (name.isEmpty())
        name = c;
    if (relayed)
    {
        const QString country = countryName(itu);
        return name.isEmpty() ? country : QStringLiteral("%1 (%2)").arg(name, country);
    }
    return name;
}

QString StationNames::stationOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("broadcaster"), e.station);
        return name.isEmpty() ? e.station : name;
    }
    return e.station;
}

QString StationNames::languageOf(const StationEntry& e, bool details) const
{
    QString name;
    if (e.source == QLatin1String("eibi"))
        name = languageText(e.lang, details);
    else if (e.source == QLatin1String("hfcc"))
        name = code(e.source, QStringLiteral("lang"), e.lang);
    return name.isEmpty() ? e.lang : name;
}

QString StationNames::countryOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("admin"), e.itu);
        if (!name.isEmpty())
            return name;
    }
    return countryName(e.itu);
}

QString StationNames::siteOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("eibi"))
        return siteName(e.itu, e.site);
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("site"), e.site);
        if (name.isEmpty())
            return e.site;
        // a site abroad says where it is
        const QString adm = code(e.source, QStringLiteral("siteadm"), e.site);
        if (adm.isEmpty() || adm == e.itu)
            return name;
        const QString admName = code(e.source, QStringLiteral("admin"), adm);
        return QStringLiteral("%1 (%2)").arg(name, admName.isEmpty() ? adm : admName);
    }
    return e.site;
}

QString StationNames::targetOf(const StationEntry& e, bool details) const
{
    QString name;
    if (e.source == QLatin1String("eibi"))
        name = targetText(e.target, details);
    else if (e.source == QLatin1String("hfcc"))
    {
        name = Ciraf::text(e.target, details);
        if (details && !name.isEmpty())
            name = QStringLiteral("CIRAF %1").arg(name);
    }
    return name.isEmpty() ? e.target : name;
}

QString StationNames::modeOf(const StationEntry& e)
{
    if (e.source == QLatin1String("eibi"))
        return EibiParser::guessMode(e);
    if (e.source == QLatin1String("hfcc"))
    {
        // HFCC's modulation letters
        if (e.mode == QLatin1String("D"))
            return QStringLiteral("AM");    // double sideband
        if (e.mode == QLatin1String("N"))
            return QStringLiteral("DRM");   // digital
        return QString();
    }
    if (e.source == QLatin1String("aoki"))
    {
        const QString remarks = e.remarks.toUpper();
        if (e.lang.contains(QLatin1String("(Digital)")) || e.station.contains(QLatin1String("DRM"))
            || remarks.contains(QLatin1String("DRM")))
            return QStringLiteral("DRM");
        if (remarks.contains(QLatin1String("USB")))
            return QStringLiteral("USB");
        if (remarks.contains(QLatin1String("LSB")))
            return QStringLiteral("LSB");
        if (remarks.contains(QLatin1String("CW")) || e.lang == QLatin1String("A1A"))
            return QStringLiteral("CW");
        return QStringLiteral("AM");
    }
    return e.mode;   // the personal list and other imports: as given
}

QString StationNames::remarksOf(const StationEntry& e)
{
    if (e.source != QLatin1String("hfcc") && e.source != QLatin1String("aoki"))
        return e.remarks;
    QStringList parts;
    if (!e.power.isEmpty())
        parts << e.power + QStringLiteral(" kW");
    if (!e.azimuth.isEmpty() && e.azimuth != QLatin1String("0") && e.azimuth != QLatin1String("ND"))
        parts << QStringLiteral("az %1°").arg(e.azimuth);
    if (e.source == QLatin1String("hfcc") && !e.mode.isEmpty() && modeOf(e).isEmpty())
        parts << QStringLiteral("mod %1").arg(e.mode);   // a letter without a mode of ours
    if (!e.remarks.isEmpty())
        parts << e.remarks;
    if (e.flag == QLatin1String("*"))
        parts << QStringLiteral("*");
    return parts.join(QStringLiteral(", "));
}

QString StationNames::weekdays(const StationEntry& e) const
{
    const bool hfcc = e.source == QLatin1String("hfcc");
    if (!hfcc && e.source != QLatin1String("aoki"))
        return e.days;   // EiBi's digits are Monday-first already, its words as written
    // HFCC's days are Sunday-first, as in the ITU's HFBC file format and
    // HFCC's own help ("1 - Sunday ... 7 - Saturday"). Aoki's are its own
    // numbering: day 1 Sunday unless the list said Monday. Both become
    // Monday-first digits, like EiBi's.
    const bool sundayFirst =
        hfcc || code(e.source, QStringLiteral("days"), QStringLiteral("1")) != QLatin1String("Monday");
    QList<int> days;
    for (const QChar c : e.days)
        if (c.isDigit() && c != QLatin1Char('0') && c <= QLatin1Char('7'))
        {
            int d = c.digitValue();
            if (sundayFirst)
                d = d == 1 ? 7 : d - 1;
            if (!days.contains(d))
                days << d;
        }
    std::sort(days.begin(), days.end());
    QString out;
    for (int d : days)
        out += QString::number(d);
    return out;
}

QString StationNames::daysOf(const StationEntry& e) const
{
    const QString days = weekdays(e);
    return days == QLatin1String("1234567") ? QString() : days;   // every day
}
