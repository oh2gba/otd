// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/Schedule.h"
#include "core/Station.h"

#include <QAbstractTableModel>
#include <QDateTime>

class StationDb;

// Table of schedule entries around the tuned frequency with their live
// on-air status. Rows are pre-sorted: on air first, then by distance.
class StationModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column
    {
        ColDelta,
        ColFrequency,
        ColStatus,
        ColMode,
        ColStation,
        ColLanguage,
        ColTime,
        ColDays,
        ColCountry,
        ColSite,
        ColTarget,
        ColLastHeard,
        ColSource,
        ColRemarks,
        ColumnCount
    };
    enum Roles
    {
        SortRole = Qt::UserRole + 1,
        StatusRole,
        OnAirRankRole,
        DeltaRole,
        DialSideRole    // -1 = upper half of the dial view, +1 = lower half
    };

    explicit StationModel(StationDb* db, QObject* parent = nullptr);

    void setEntries(const StationList& entries, double centreKHz);
    // Dial order: ascending frequency, the rows nearest the VFO in the middle.
    void setDialEntries(const StationList& entries, double centreKHz);
    // dial view: this many blank rows above and below the real ones, so
    // the VFO can stay in the middle even at the ends of the list
    void setPadding(int rows) { m_padding = rows; }
    bool isBlank(int row) const { return row >= 0 && row < m_rows.size() && m_rows[row].blank; }
    int entryCount() const;
    bool isDialOrder() const { return m_dialOrder; }
    // dial view: rows this close to the VFO are highlighted
    void setHighlightKHz(double kHz);
    // briefly tint the row of this entry (0 clears); survives list reloads
    void setFlash(qint64 entryId);
    void setCentre(double centreKHz);
    void refreshStatus(const QDateTime& utc = QDateTime::currentDateTimeUtc());
    int onAirCount() const;
    const StationEntry& entryAt(int row) const { return m_rows[row].entry; }
    Schedule::OnAir statusAt(int row) const { return m_rows[row].status; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

private:
    struct Row
    {
        StationEntry entry;
        Schedule::OnAir status = Schedule::OnAir::Unknown;
        double delta = 0.0;
        bool shaded = false;    // background shade alternates per frequency
        int side = 0;           // which half of the dial view shows the row
        bool blank = false;     // padding, not a station
    };
    void shadeGroups();
    bool m_dialOrder = false;
public:
    static int rank(Schedule::OnAir s);
private:
    static QString sourceLabel(const QString& id);
    void sortRows();
    QString tooltip(const Row& r) const;

    StationDb* m_db;
    QVector<Row> m_rows;
    double m_centre = 0.0;
    double m_highlightKHz = 0.0;
    int m_padding = 0;
    qint64 m_flashId = 0;
    int flashRow() const;
    QDateTime m_lastEval;
};
