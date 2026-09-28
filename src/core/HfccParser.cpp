// SPDX-License-Identifier: GPL-3.0-or-later
#include "HfccParser.h"

#include <QRegularExpression>
#include <QStringDecoder>
#include <QVector>

namespace
{
QStringList decodeLines(const QByteArray& data)
{
    QStringDecoder dec(QStringDecoder::Latin1);
    QString text = dec(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    return text.split(QLatin1Char('\n'));
}

// "ABC Some name       more columns" -> code, name (first column only)
void parseCodeTable(const QByteArray& data, int nameWidth, QHash<QString, QString>* out)
{
    const QStringList lines = decodeLines(data);
    for (const QString& raw : lines)
    {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.size() < 5)
            continue;
        const QString code = raw.left(3).trimmed();
        if (code.isEmpty() || raw.size() < 4 || raw[3] != QLatin1Char(' '))
            continue;
        QString name = nameWidth > 0 ? raw.mid(4, nameWidth) : raw.mid(4);
        name = name.trimmed();
        if (!name.isEmpty())
            out->insert(code, name);
    }
}
} // namespace

HfccParser::Tables HfccParser::parseTables(const QByteArray& site, const QByteArray& broadcasters,
                                           const QByteArray& languages, const QByteArray& admins)
{
    Tables t;
    parseCodeTable(broadcasters, 0, &t.broadcasters);
    parseCodeTable(languages, 0, &t.languages);
    parseCodeTable(admins, 50, &t.admins);

    // ;Co Site Name                      ADM Lati  Longi
    // A-A Alma Ata                       KAZ 43N17 077E00
    static const QRegularExpression siteRe(
        QStringLiteral("^(\\S{1,3})\\s+(.+?)\\s+([A-Z]{1,3})\\s+\\d{2}[NS]\\d{2}\\s+\\d{3}[EW]\\d{2}"));
    const QStringList lines = decodeLines(site);
    for (const QString& raw : lines)
    {
        if (raw.startsWith(QLatin1Char(';')))
            continue;
        const QRegularExpressionMatch m = siteRe.match(raw);
        if (!m.hasMatch())
            continue;
        Tables::Site s;
        s.name = m.captured(2).trimmed();
        s.adm = m.captured(3);
        t.sites.insert(m.captured(1), s);
    }
    return t;
}

HfccParser::ParseResult HfccParser::parseSchedule(const QByteArray& data)
{
    ParseResult result;
    const QStringList lines = decodeLines(data);

    // Column spans come from the ruler line ";----+----+...". The header
    // names are not reliable (single-character columns), so map by order.
    enum Col { FREQ, STRT, STOP, CIRAF, LOC, POWR, AZIMUTH, SLW, ANT, DAYS, FDATE, TDATE,
               MOD, AFRQ, LANGUAGE, ADM, BRC, FMO, REQ, OLD, ALT1, ALT2, ALT3, NOTES, ColCount };
    QVector<QPair<int, int>> spans;

    for (const QString& raw : lines)
    {
        if (raw.startsWith(QLatin1String(";----")) && spans.isEmpty())
        {
            int start = 1;
            for (int i = 1; i < raw.size(); ++i)
            {
                if (raw[i] == QLatin1Char('+'))
                {
                    spans.push_back({start, i});
                    start = i + 1;
                }
            }
            spans.push_back({start, 4096});
            continue;
        }
        if (raw.isEmpty() || raw.startsWith(QLatin1Char(';')))
            continue;
        if (spans.size() < ColCount)
        {
            ++result.skippedLines;
            continue;
        }

        auto field = [&](int col) {
            const auto& sp = spans[col];
            return raw.mid(sp.first, sp.second - sp.first).trimmed();
        };

        bool ok = false;
        const double kHz = field(FREQ).toDouble(&ok);
        if (!ok || kHz <= 0.0)
        {
            ++result.skippedLines;
            continue;
        }
        bool okS = false, okE = false;
        const int start = field(STRT).toInt(&okS);
        const int stop = field(STOP).toInt(&okE);
        if (!okS || !okE)
        {
            ++result.skippedLines;
            continue;
        }

        StationEntry e;
        e.source = QStringLiteral("hfcc");
        e.kHz = kHz;
        e.startMin = (start / 100) * 60 + start % 100;
        e.endMin = (stop / 100) * 60 + stop % 100;
        e.days = field(DAYS);
        e.itu = field(ADM);
        e.station = field(BRC);   // the broadcaster's code; its name is looked up
        e.lang = field(LANGUAGE);
        e.site = field(LOC);
        e.target = field(CIRAF);
        e.startDate = field(FDATE);
        e.stopDate = field(TDATE);
        e.power = field(POWR);
        e.azimuth = field(AZIMUTH);
        e.mode = field(MOD);
        e.remarks = field(NOTES);

        result.entries.push_back(e);
    }

    if (spans.isEmpty())
        result.error = QStringLiteral("no column ruler found");
    else if (result.entries.isEmpty())
        result.error = QStringLiteral("no schedule lines found");
    return result;
}
