// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ScheduleSource.h"

// The Finnish frequency allocation table (Traficom Radio Frequency
// Regulation 4) from Traficom's open data API, CC BY 4.0. Not a station
// list: it tells what each sub-band is used for in Finland, far finer
// than the built-in band plan. Only the part below 30 MHz is fetched.
class TraficomSource : public ScheduleSource
{
    Q_OBJECT
public:
    TraficomSource(StationDb* db, QNetworkAccessManager* nam, QObject* parent = nullptr);

    int count() const override;
    bool isStale(int maxAgeDays) const override;

    struct Row
    {
        double lowKHz = 0.0;
        double highKHz = 0.0;
        QString service;    // e.g. BROADCASTING (capitals = primary)
        QString usage;      // e.g. Broadcasting, Amateur, Data service
        QString info;       // additional information
        QString mode;       // simplex, duplex ...
        QString emission;
        QString comment;
    };
    // Parses the OData JSON answer.
    static QList<Row> parse(const QByteArray& json, QString* error = nullptr);
    // The query that fetches just the HF rows and the columns used.
    static QString hfQuery();

public slots:
    void update() override;
};
