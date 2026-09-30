// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationModel.h"
#include "core/StationDb.h"

#include <cmath>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <algorithm>

StationModel::StationModel(StationDb* db, QObject* parent)
    : QAbstractTableModel(parent)
    , m_db(db)
{
}

int StationModel::rank(Schedule::OnAir s)
{
    switch (s)
    {
    case Schedule::OnAir::Yes:      return 0;
    case Schedule::OnAir::Unknown:  return 1;
    case Schedule::OnAir::No:       return 2;
    case Schedule::OnAir::Inactive: return 3;
    }
    return 4;
}

void StationModel::setEntries(const StationList& entries, double centreKHz)
{
    beginResetModel();
    m_dialOrder = false;
    m_centre = centreKHz;
    m_rows.clear();
    m_rows.reserve(entries.size());
    m_lastEval = QDateTime::currentDateTimeUtc();
    for (const StationEntry& e : entries)
    {
        Row r;
        r.entry = e;
        r.delta = e.kHz - centreKHz;
        r.status = Schedule::status(e, m_lastEval, m_db->weekdays(e));
        m_rows.push_back(r);
    }
    sortRows();
    endResetModel();
}

void StationModel::setHighlightKHz(double kHz)
{
    if (qFuzzyCompare(kHz + 1.0, m_highlightKHz + 1.0))
        return;
    m_highlightKHz = kHz;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColumnCount - 1),
                         { Qt::ForegroundRole });
}

int StationModel::flashRow() const
{
    if (m_flashId == 0)
        return -1;
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].entry.id == m_flashId)
            return i;
    return -1;
}

void StationModel::setFlash(qint64 entryId)
{
    const int before = flashRow();
    m_flashId = entryId;
    const int after = flashRow();
    for (int row : {before, after})
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, ColumnCount - 1), { Qt::BackgroundRole });
}

void StationModel::setDialEntries(const StationList& entries, double centreKHz)
{
    beginResetModel();
    m_dialOrder = true;
    m_centre = centreKHz;
    m_rows.clear();
    m_rows.reserve(entries.size());
    m_lastEval = QDateTime::currentDateTimeUtc();
    for (const StationEntry& e : entries)
    {
        Row r;
        r.entry = e;
        r.delta = e.kHz - centreKHz;
        r.status = Schedule::status(e, m_lastEval, m_db->weekdays(e));
        m_rows.push_back(r);
    }
    // blank rows at both ends; their frequencies keep them there when sorting
    for (int i = 0; i < m_padding; ++i)
    {
        Row pad;
        pad.blank = true;
        pad.entry.kHz = -1.0;
        pad.delta = -centreKHz - 1.0;
        m_rows.push_back(pad);
        pad.entry.kHz = 1e12;
        pad.delta = 1e12 - centreKHz;
        m_rows.push_back(pad);
    }
    sortRows();
    endResetModel();
}

int StationModel::entryCount() const
{
    return int(std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row& r) { return !r.blank; }));
}

void StationModel::setCentre(double centreKHz)
{
    if (qFuzzyCompare(centreKHz, m_centre))
        return;
    m_centre = centreKHz;
    for (Row& r : m_rows)
        r.delta = r.entry.kHz - centreKHz;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, ColDelta), index(m_rows.size() - 1, ColDelta));
}

void StationModel::refreshStatus(const QDateTime& utc)
{
    m_lastEval = utc;
    bool changed = false;
    for (Row& r : m_rows)
    {
        if (r.blank)
            continue;
        const Schedule::OnAir s = Schedule::status(r.entry, utc, m_db->weekdays(r.entry));
        if (s != r.status)
        {
            r.status = s;
            changed = true;
        }
    }
    if (!changed)
        return;
    beginResetModel();
    sortRows();
    endResetModel();
}

int StationModel::onAirCount() const
{
    return int(std::count_if(m_rows.cbegin(), m_rows.cend(), [](const Row& r) {
        return !r.blank && r.status == Schedule::OnAir::Yes;
    }));
}

void StationModel::shadeGroups()
{
    bool shade = false;
    double last = -1.0;
    for (Row& r : m_rows)
    {
        if (!qFuzzyCompare(r.entry.kHz + 1.0, last + 1.0))
        {
            shade = !shade;
            last = r.entry.kHz;
        }
        r.shaded = shade && !r.blank;
    }

    // rows exactly on the VFO are shared between the two halves, the
    // first (floor) half above the middle and the rest below it
    int onFreq = 0;
    for (const Row& r : m_rows)
        if (qFuzzyIsNull(r.delta))
            ++onFreq;
    int seen = 0;
    for (Row& r : m_rows)
    {
        if (r.delta < 0.0 && !qFuzzyIsNull(r.delta))
            r.side = -1;
        else if (!qFuzzyIsNull(r.delta))
            r.side = +1;
        else
            r.side = seen++ < onFreq / 2 ? -1 : +1;
    }
}

void StationModel::sortRows()
{
    if (m_dialOrder)
    {
        // ascending frequency; on the same frequency on-air stations first,
        // then by start time
        std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) {
            if (!qFuzzyCompare(a.entry.kHz + 1.0, b.entry.kHz + 1.0))
                return a.entry.kHz < b.entry.kHz;
            const int ra = rank(a.status), rb = rank(b.status);
            if (ra != rb)
                return ra < rb;
            return a.entry.startMin < b.entry.startMin;
        });
        shadeGroups();
        return;
    }
    // on air first, then by frequency: an order that does not change
    // when the VFO moves, so tuning to a search result keeps the list still
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) {
        const int ra = rank(a.status), rb = rank(b.status);
        if (ra != rb)
            return ra < rb;
        if (!qFuzzyCompare(a.entry.kHz + 1.0, b.entry.kHz + 1.0))
            return a.entry.kHz < b.entry.kHz;
        if (a.entry.startMin != b.entry.startMin)
            return a.entry.startMin < b.entry.startMin;
        return a.entry.station < b.entry.station;
    });
    shadeGroups();
}

int StationModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int StationModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant StationModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    switch (section)
    {
    case ColDelta:     return tr("Δ kHz");
    case ColFrequency: return tr("kHz");
    case ColStatus:    return tr("Status");
    case ColMode:      return tr("Mode");
    case ColStation:   return tr("Station");
    case ColLanguage:  return tr("Language");
    case ColTime:      return tr("UTC");
    case ColDays:      return tr("Days");
    case ColCountry:   return tr("Country");
    case ColSite:      return tr("Transmitter");
    case ColTarget:    return tr("Target");
    case ColLastHeard: return tr("Heard");
    case ColSource:    return tr("Source");
    case ColRemarks:   return tr("Remarks");
    }
    return QVariant();
}

QString StationModel::sourceLabel(const QString& id)
{
    if (id == QLatin1String("eibi")) return QStringLiteral("EiBi");
    if (id == QLatin1String("hfcc")) return QStringLiteral("HFCC");
    if (id == QLatin1String("aoki")) return QStringLiteral("Aoki");
    if (id == userSourceId())        return tr("Mine");
    return id.toUpper();
}

namespace
{
// a validity date as the sources give it: DDMM (EiBi), DDMMYY (HFCC)
QString dateText(const QString& d)
{
    if (d.size() == 4)
        return QStringLiteral("%1.%2.").arg(d.left(2), d.mid(2, 2));
    if (d.size() == 6)
        return QStringLiteral("%1.%2.20%3").arg(d.left(2), d.mid(2, 2), d.mid(4, 2));
    return d;
}
}

QString StationModel::tooltip(const Row& r) const
{
    // the list has the short names; here also what the codes stand for
    const StationEntry& e = r.entry;
    QString t = QStringLiteral("<b>%1</b><br>%2 kHz, %3 UTC")
                    .arg(m_db->stationOf(e).toHtmlEscaped())
                    .arg(e.kHz, 0, 'f', e.kHz == qRound(e.kHz) ? 0 : 3)
                    .arg(Schedule::timeWindow(e));
    const QString days = m_db->daysOf(e);
    if (!days.isEmpty())
        t += tr("<br>Days: %1").arg(days.toHtmlEscaped());
    const QString lang = m_db->languageOf(e, true);
    if (!lang.isEmpty())
        t += tr("<br>Language: %1").arg(lang.toHtmlEscaped());
    const QString mode = StationDb::modeOf(e);
    if (!mode.isEmpty())
        t += tr("<br>Mode: %1").arg(mode);
    t += tr("<br>Country: %1").arg(m_db->countryOf(e).toHtmlEscaped());
    const QString site = m_db->siteOf(e);
    if (!site.isEmpty())
        t += tr("<br>Transmitter: %1").arg(site.toHtmlEscaped());
    const QString target = m_db->targetOf(e, true);
    if (!target.isEmpty())
        t += tr("<br>Target: %1").arg(target.toHtmlEscaped());
    const QString remarks = StationDb::remarksOf(e);
    if (!remarks.isEmpty())
        t += tr("<br>Remarks: %1").arg(remarks.toHtmlEscaped());
    if (!e.startDate.isEmpty() || !e.stopDate.isEmpty())
        t += tr("<br>Valid: %1 - %2").arg(dateText(e.startDate), dateText(e.stopDate));
    if (!e.lastHeard.isEmpty())
        t += tr("<br>Last heard: %1/20%2").arg(e.lastHeard.left(2), e.lastHeard.mid(2));
    t += tr("<br>Source: %1, status: %2").arg(sourceLabel(e.source), Schedule::statusText(r.status));
    return t;
}

QVariant StationModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return QVariant();
    const Row& r = m_rows[index.row()];
    const StationEntry& e = r.entry;

    if (r.blank)
    {
        switch (role)
        {
        case DialSideRole:  return r.side;
        case DeltaRole:     return r.delta;
        case OnAirRankRole: return -1;   // never filtered out
        case StatusRole:    return int(Schedule::OnAir::No);
        default:            return QVariant();
        }
    }

    switch (role)
    {
    case Qt::DisplayRole:
        switch (index.column())
        {
        case ColDelta:
        {
            if (qFuzzyIsNull(r.delta))
                return QStringLiteral("\u25CF");              // on the VFO
            return QStringLiteral("%1 %2").arg(r.delta > 0 ? QStringLiteral("\u25B2")     // above
                                                            : QStringLiteral("\u25BC"))    // below
                                           .arg(qAbs(r.delta), 0, 'f', 1);
        }
        case ColFrequency: return QString::number(e.kHz, 'f', e.kHz == qRound(e.kHz) ? 0 : 3);
        case ColStatus:    return Schedule::statusText(r.status);
        case ColMode:      return StationDb::modeOf(e);
        case ColStation:   return m_db->stationOf(e);
        case ColLanguage:  return m_db->languageOf(e);
        case ColTime:      return Schedule::timeWindow(e);
        case ColDays:      return m_db->daysOf(e);
        case ColCountry:   return m_db->countryOf(e);
        case ColSite:      return m_db->siteOf(e);
        case ColTarget:    return m_db->targetOf(e);
        case ColLastHeard: return e.lastHeard.isEmpty()
                                  ? QString()
                                  : QStringLiteral("%1/%2").arg(e.lastHeard.left(2), e.lastHeard.mid(2));
        case ColSource:    return sourceLabel(e.source);
        case ColRemarks:   return StationDb::remarksOf(e);
        }
        return QVariant();

    case Qt::ToolTipRole:
        return tooltip(r);

    case Qt::TextAlignmentRole:
        if (index.column() == ColDelta || index.column() == ColFrequency || index.column() == ColTime)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);

    case Qt::FontRole:
        if (r.status == Schedule::OnAir::Yes)
        {
            QFont f;
            f.setBold(true);
            return f;
        }
        return QVariant();

    case Qt::ForegroundRole:
        if (m_dialOrder && std::abs(r.delta) <= m_highlightKHz + 1e-6)
            return QBrush(QColor(0xf8, 0x51, 0x49));     // within reach of the VFO
        if (index.column() == ColDelta && !qFuzzyIsNull(r.delta)
            && r.status != Schedule::OnAir::No && r.status != Schedule::OnAir::Inactive)
            return QBrush(r.delta > 0 ? QColor(0x79, 0xc0, 0xff) : QColor(0xff, 0xa6, 0x57));
        if (r.status == Schedule::OnAir::No || r.status == Schedule::OnAir::Inactive)
            return QBrush(QGuiApplication::palette().color(QPalette::Disabled, QPalette::Text));
        if (r.status == Schedule::OnAir::Yes && index.column() == ColStatus)
            return QBrush(QColor(0x2e, 0xa0, 0x43));
        return QVariant();

    case SortRole:
        switch (index.column())
        {
        case ColDelta:     return qAbs(r.delta);
        case ColFrequency: return e.kHz;
        case ColStatus:    return rank(r.status);
        case ColTime:      return e.startMin;
        default:           return data(index, Qt::DisplayRole);
        }

    case Qt::BackgroundRole:
        if (m_flashId != 0 && e.id == m_flashId)
            return QBrush(QColor(0xf8, 0x51, 0x49, 0x50));   // the row just sent to the rig
        if (r.shaded)
            return QBrush(QGuiApplication::palette().color(QPalette::AlternateBase));
        return QVariant();

    case StatusRole:
        return int(r.status);
    case OnAirRankRole:
        return rank(r.status);
    case DeltaRole:
        return r.delta;
    case DialSideRole:
        return r.side;
    }
    return QVariant();
}
