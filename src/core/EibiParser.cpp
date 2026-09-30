// SPDX-License-Identifier: GPL-3.0-or-later
#include "EibiParser.h"
#include "BandPlan.h"

#include <QRegularExpression>
#include <QStringDecoder>

namespace
{
int parseHHMM(const QString& s, bool* ok)
{
    if (s.size() != 4)
    {
        *ok = false;
        return 0;
    }
    bool okH = false, okM = false;
    const int h = s.left(2).toInt(&okH);
    const int m = s.mid(2).toInt(&okM);
    *ok = okH && okM && h >= 0 && h <= 24 && m >= 0 && m < 60;
    return h * 60 + m;
}

QDate lastSundayOf(int year, int month)
{
    QDate d(year, month, 1);
    d = d.addDays(d.daysInMonth() - 1);
    return d.addDays(-(d.dayOfWeek() % 7));   // Qt: Mon=1 .. Sun=7
}

QString two(int v)
{
    return QString("%1").arg(((v % 100) + 100) % 100, 2, 10, QChar('0'));
}

// Strips trailing coordinates like " 26S35-28E08" or " 33S41'18\"-18E42'22\"".
QString stripCoordinates(QString s)
{
    static const QRegularExpression coord(
        QStringLiteral("\\s+\\d{1,3}[NS]\\d{2}.*$"));
    s.remove(coord);
    return s.trimmed();
}
} // namespace

EibiParser::ParseResult EibiParser::parseCsv(const QByteArray& data)
{
    ParseResult result;
    QStringDecoder decoder(QStringDecoder::Latin1);
    const QString text = decoder(data);
    const QStringList lines = text.split(QLatin1Char('\n'));

    static const QRegularExpression dateRe(
        QStringLiteral("^(\\d{4})?\\d*\\s*(?:\\[(\\d{4})\\])?"));

    for (const QString& rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("kHz:")))
            continue;

        const QStringList f = line.split(QLatin1Char(';'));
        if (f.size() < 9)
        {
            ++result.skippedLines;
            continue;
        }

        StationEntry e;
        e.source = QStringLiteral("eibi");

        bool ok = false;
        e.kHz = f[0].trimmed().toDouble(&ok);
        if (!ok || e.kHz <= 0.0)
        {
            ++result.skippedLines;
            continue;
        }

        const QString t = f[1].trimmed();
        if (t.size() == 9 && t[4] == QLatin1Char('-'))
        {
            bool okStart = false, okEnd = false;
            e.startMin = parseHHMM(t.left(4), &okStart);
            e.endMin = parseHHMM(t.mid(5), &okEnd);
            if (!okStart || !okEnd)
            {
                ++result.skippedLines;
                continue;
            }
        }
        else
        {
            e.startMin = 0;
            e.endMin = 1440;
        }

        e.days = f[2].trimmed();
        e.itu = f[3].trimmed();
        e.station = f[4].trimmed();
        e.lang = f[5].trimmed();
        e.target = f[6].trimmed();
        e.site = f[7].trimmed();
        e.persistence = f.value(8).trimmed().toInt();

        QRegularExpressionMatch m = dateRe.match(f.value(9).trimmed());
        e.startDate = m.captured(1);
        m = dateRe.match(f.value(10).trimmed());
        e.stopDate = m.captured(1);
        e.lastHeard = m.captured(2);

        result.entries.push_back(e);
    }

    if (result.entries.isEmpty())
        result.error = QStringLiteral("no schedule lines found");
    return result;
}

QString EibiParser::guessMode(const StationEntry& e)
{
    const QString days = e.days.toUpper();
    if (days == QLatin1String("USB") || days == QLatin1String("LSB"))
        return days;
    if (e.lang == QLatin1String("-CW"))  return QStringLiteral("CW");
    if (e.lang == QLatin1String("-TY"))  return QStringLiteral("RTTY");
    if (e.lang == QLatin1String("-HF"))  return QStringLiteral("HFDL");
    if (e.lang == QLatin1String("-EC"))  return QStringLiteral("CW");
    const QString name = e.station.toLower();
    if (name.contains(QLatin1String("drm")))    return QStringLiteral("DRM");
    if (name.contains(QLatin1String("fax")))    return QStringLiteral("FAX");
    if (name.contains(QLatin1String("volmet"))) return QStringLiteral("USB");
    // EiBi gives no mode. The allocation the frequency sits in tells the
    // most: the aeronautical and maritime bands are single sideband (the
    // oceanic air traffic, the coast stations), as are the coast guard
    // weather broadcasts inside the amateur bands. What else the list has
    // there are the out-of-band broadcasts and jammers from China, Taiwan
    // and Korea, in their languages: those stay AM, as do the broadcast
    // bands, time signals and anything the plan does not know.
    static const BandPlan plan = BandPlan::builtIn();
    const QVector<BandPlan::Band> bands = plan.lookup(e.kHz, 1);
    const QString kind = bands.isEmpty() ? QString() : bands.first().kind;
    if (kind == QLatin1String("aero") || kind == QLatin1String("maritime") || kind == QLatin1String("amateur"))
    {
        static const QStringList broadcastItu = {QStringLiteral("TWN"), QStringLiteral("CHN"), QStringLiteral("KRE")};
        static const QStringList broadcastLang = {QStringLiteral("M"), QStringLiteral("C"), QStringLiteral("CA"),
                                                  QStringLiteral("K"), QStringLiteral("VN")};
        const QString firstLang = e.lang.section(QLatin1Char(','), 0, 0).trimmed();
        if (!broadcastItu.contains(e.itu) && !broadcastLang.contains(firstLang))
            return QStringLiteral("USB");
    }
    return QStringLiteral("AM");
}

EibiParser::CodeTables EibiParser::parseReadme(const QByteArray& data)
{
    CodeTables tables;
    QStringDecoder decoder(QStringDecoder::Latin1);
    const QString text = decoder(data);
    const QStringList lines = text.split(QLatin1Char('\n'));

    enum Section { None, Languages, Countries, Targets, Sites };
    Section section = None;

    static const QRegularExpression langRe(
        QStringLiteral("^\\s{2,4}(\\S{1,4})\\s{2,}(.+?)\\s*(?:\\[[^\\]]*\\]\\s*)*$"));
    static const QRegularExpression countryRe(
        QStringLiteral("^\\s{2,4}(\\S{1,4})\\s{2,}(.+?)\\s*$"));
    static const QRegularExpression targetRe(
        QStringLiteral("^\\s{2,4}(\\S{1,3})\\s+-\\s+(.+?)\\s*$"));
    static const QRegularExpression siteCountryRe(
        QStringLiteral("^\\s{2,4}([A-Z]{1,3}):\\s*(.*)$"));
    static const QRegularExpression siteRe(
        QStringLiteral("^\\s{5,}([A-Za-z0-9]{1,3})-(.+?)\\s*$"));

    QString siteCountry;

    for (const QString& rawLine : lines)
    {
        const QString line = rawLine.trimmed().remove(QLatin1Char('\r'));
        const QString stripped = rawLine;

        if (line == QLatin1String("I) Language codes."))       { section = Languages; continue; }
        if (line == QLatin1String("II) Country codes."))       { section = Countries; continue; }
        if (line == QLatin1String("III) Target-area codes."))  { section = Targets;   continue; }
        if (line.startsWith(QLatin1String("IV) Transmitter"))) { section = Sites;     continue; }

        if (line.isEmpty())
            continue;

        QRegularExpressionMatch m;
        switch (section)
        {
        case Languages:
            m = langRe.match(stripped);
            if (m.hasMatch())
                tables.languages.insert(m.captured(1), m.captured(2).trimmed());
            break;
        case Countries:
            m = countryRe.match(stripped);
            if (m.hasMatch() && !m.captured(2).startsWith(QLatin1String("(")))
            {
                // "CAB  Cabinda *": the asterisk marks a non-official code
                QString name = m.captured(2).trimmed();
                if (name.endsWith(QLatin1Char('*')))
                    name.chop(1);
                tables.countries.insert(m.captured(1), name.trimmed());
            }
            break;
        case Targets:
            m = targetRe.match(stripped);
            if (m.hasMatch())
                tables.targets.insert(m.captured(1), m.captured(2).trimmed());
            break;
        case Sites:
            m = siteCountryRe.match(stripped);
            if (m.hasMatch())
            {
                siteCountry = m.captured(1);
                QString rest = m.captured(2).trimmed();
                // "Meyerton 26S35-28E08 except:" -> default site of that country
                QRegularExpressionMatch inl = siteRe.match(QStringLiteral("     ") + rest);
                if (inl.hasMatch() && inl.captured(1).size() <= 2 && rest.indexOf(QLatin1Char('-')) < 3)
                    tables.sites.insert(siteCountry + QLatin1Char('/') + inl.captured(1),
                                        stripCoordinates(inl.captured(2)));
                else if (!rest.isEmpty())
                {
                    rest.remove(QLatin1String(" except:"));
                    tables.sites.insert(siteCountry + QLatin1Char('/'), stripCoordinates(rest));
                }
                break;
            }
            if (siteCountry.isEmpty())
                break;
            m = siteRe.match(stripped);
            if (m.hasMatch())
                tables.sites.insert(siteCountry + QLatin1Char('/') + m.captured(1),
                                    stripCoordinates(m.captured(2)));
            break;
        case None:
            break;
        }
    }
    return tables;
}

QString EibiParser::seasonCode(const QDate& date)
{
    const int y = date.year();
    const QDate summerStart = lastSundayOf(y, 3);
    const QDate winterStart = lastSundayOf(y, 10);
    if (date < summerStart)
        return QLatin1Char('b') + two(y - 1);
    if (date < winterStart)
        return QLatin1Char('a') + two(y);
    return QLatin1Char('b') + two(y);
}

QString EibiParser::previousSeason(const QString& season)
{
    if (season.size() != 3)
        return QString();
    const int yy = season.mid(1).toInt();
    if (season.startsWith(QLatin1Char('a')))
        return QLatin1Char('b') + two(yy - 1);
    return QLatin1Char('a') + two(yy);
}

QString EibiParser::nextSeason(const QString& season)
{
    if (season.size() != 3)
        return QString();
    const int yy = season.mid(1).toInt();
    if (season.startsWith(QLatin1Char('a')))
        return QLatin1Char('b') + two(yy);
    return QLatin1Char('a') + two(yy + 1);
}
