// SPDX-License-Identifier: GPL-3.0-or-later
#include "TraficomSource.h"
#include "StationDb.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

TraficomSource::TraficomSource(StationDb* db, QNetworkAccessManager* nam, QObject* parent)
    : ScheduleSource(QStringLiteral("traficom"), QStringLiteral("Traficom"),
                     QUrl(QStringLiteral("https://opendata.traficom.fi/api/v13/Taajuusjakotaulukko")),
                     db, nam, parent)
{
}

int TraficomSource::count() const
{
    return m_db->allocationCount(id());
}

bool TraficomSource::isStale(int maxAgeDays) const
{
    // no seasons here: age and emptiness decide
    const QDateTime last = lastUpdate();
    if (!last.isValid() || count() == 0)
        return true;
    return last.daysTo(QDateTime::currentDateTimeUtc()) >= maxAgeDays;
}

QString TraficomSource::hfQuery()
{
    return QStringLiteral("?$filter=Sub_band_upper_limit__Hz_%20le%2030000000"
                          "&$select=Sub_band_lower_limit__Hz_,Sub_band_upper_limit__Hz_,"
                          "Services_in_Finland,Sub_band_usage,Additional_information,"
                          "Mode_of_traffic,Class_of_emission,Comment");
}

QList<TraficomSource::Row> TraficomSource::parse(const QByteArray& json, QString* error)
{
    QList<Row> out;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject())
    {
        if (error)
            *error = perr.errorString();
        return out;
    }
    const QJsonArray value = doc.object().value(QStringLiteral("value")).toArray();
    for (const QJsonValue& v : value)
    {
        const QJsonObject o = v.toObject();
        Row r;
        r.lowKHz = o.value(QStringLiteral("Sub_band_lower_limit__Hz_")).toDouble() / 1000.0;
        r.highKHz = o.value(QStringLiteral("Sub_band_upper_limit__Hz_")).toDouble() / 1000.0;
        if (r.highKHz <= 0.0 || r.highKHz < r.lowKHz)
            continue;
        r.service = o.value(QStringLiteral("Services_in_Finland")).toString().simplified();
        r.usage = o.value(QStringLiteral("Sub_band_usage")).toString().simplified();
        r.info = o.value(QStringLiteral("Additional_information")).toString().simplified();
        r.mode = o.value(QStringLiteral("Mode_of_traffic")).toString().simplified();
        r.emission = o.value(QStringLiteral("Class_of_emission")).toString().simplified();
        r.comment = o.value(QStringLiteral("Comment")).toString().simplified();
        out.push_back(r);
    }
    if (out.isEmpty() && error && error->isEmpty())
        *error = QStringLiteral("no rows");
    return out;
}

void TraficomSource::update()
{
    if (!begin())
        return;
    emit progress(tr("Checking the Traficom allocation table ..."));
    QUrl url = baseUrl();
    url.setQuery(hfQuery().mid(1), QUrl::StrictMode);
    // If-Modified-Since only when something is stored already
    const QString ims = count() > 0 ? m_db->meta(id() + QStringLiteral(".lastModified")) : QString();
    download(url, ims, [this](const Download& dl) {
        if (dl.status == 304)
        {
            touchUpdated();
            finish(true, tr("Traficom table is up to date (%1 sub-bands)").arg(count()));
            return;
        }
        if (dl.status != 200)
        {
            finish(false, tr("Traficom download failed: %1")
                              .arg(dl.status ? QString::number(dl.status) : dl.error));
            return;
        }
        QString err;
        const QList<Row> rows = parse(dl.data, &err);
        if (rows.size() < 100)
        {
            finish(false, tr("Traficom table unusable: %1").arg(err));
            return;
        }
        emit progress(tr("Storing %1 Traficom sub-bands ...").arg(rows.size()));
        QList<StationDb::Allocation> list;
        list.reserve(rows.size());
        for (const Row& r : rows)
        {
            StationDb::Allocation a;
            a.lowKHz = r.lowKHz;
            a.highKHz = r.highKHz;
            a.service = r.service;
            a.usage = r.usage;
            a.info = r.info;
            a.mode = r.mode;
            a.emission = r.emission;
            a.comment = r.comment;
            list.push_back(a);
        }
        if (!m_db->replaceAllocations(id(), list))
        {
            finish(false, tr("Database error: %1").arg(m_db->lastError()));
            return;
        }
        m_db->setMeta(id() + QStringLiteral(".lastModified"), dl.lastModified);
        touchUpdated();
        finish(true, tr("Traficom table loaded, %1 sub-bands").arg(rows.size()));
    });
}
