// SPDX-License-Identifier: GPL-3.0-or-later
#include "KiwiDirectory.h"
#include "StationDb.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace
{
const QString kKeyJson = QStringLiteral("kiwi.directory");
const QString kKeyFetched = QStringLiteral("kiwi.directory.fetched");
const QString kKeyModified = QStringLiteral("kiwi.directory.lastModified");
}

KiwiDirectory::KiwiDirectory(StationDb* db, QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_nam(nam)
{
    load();
}

QUrl KiwiDirectory::defaultUrl()
{
    return QUrl(QStringLiteral("http://rx.linkfanel.net/kiwisdr_com.js"));
}

QDateTime KiwiDirectory::fetched() const
{
    return QDateTime::fromString(m_db->meta(kKeyFetched), Qt::ISODate);
}

bool KiwiDirectory::isStale(int maxAgeHours) const
{
    const QDateTime f = fetched();
    return !f.isValid() || f.secsTo(QDateTime::currentDateTimeUtc()) > maxAgeHours * 3600;
}

QList<KiwiDirectory::Receiver> KiwiDirectory::parse(const QByteArray& data, QString* error)
{
    QList<Receiver> out;
    // "var kiwisdr_com = [ {...}, ... ];" with trailing commas: cut to the
    // array and drop the commas JSON does not allow
    const int start = data.indexOf('[');
    const int end = data.lastIndexOf(']');
    if (start < 0 || end <= start)
    {
        if (error)
            *error = QStringLiteral("no receiver array found");
        return out;
    }
    QByteArray json = data.mid(start, end - start + 1);
    static const QRegularExpression trailing(QStringLiteral(",\\s*([\\]}])"));
    json = QString::fromUtf8(json).replace(trailing, QStringLiteral("\\1")).toUtf8();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isArray())
    {
        if (error)
            *error = perr.errorString();
        return out;
    }
    for (const QJsonValue& v : doc.array())
    {
        const QJsonObject o = v.toObject();
        Receiver r;
        r.url = o.value(QStringLiteral("url")).toString().trimmed();
        if (r.url.isEmpty())
            continue;
        r.name = o.value(QStringLiteral("name")).toString().simplified();
        r.location = o.value(QStringLiteral("loc")).toString().simplified();
        r.grid = o.value(QStringLiteral("grid")).toString();
        r.users = o.value(QStringLiteral("users")).toString().toInt();
        r.usersMax = o.value(QStringLiteral("users_max")).toString().toInt();
        r.offline = o.value(QStringLiteral("offline")).toString() == QLatin1String("yes")
                    || o.value(QStringLiteral("status")).toString() == QLatin1String("offline");
        const QStringList bands = o.value(QStringLiteral("bands")).toString().split(QLatin1Char('-'));
        if (bands.size() == 2)
        {
            r.lowKHz = bands[0].toDouble() / 1000.0;
            r.highKHz = bands[1].toDouble() / 1000.0;
        }
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const Receiver& a, const Receiver& b) {
        const int c = QString::localeAwareCompare(a.location, b.location);
        return c != 0 ? c < 0 : QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out;
}

void KiwiDirectory::load()
{
    const QString json = m_db->meta(kKeyJson);
    if (!json.isEmpty())
        m_receivers = parse(json.toUtf8());
}

void KiwiDirectory::store(const QByteArray& json, const QString& lastModified)
{
    m_db->setMeta(kKeyJson, QString::fromUtf8(json));
    m_db->setMeta(kKeyFetched, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    m_db->setMeta(kKeyModified, lastModified);
}

void KiwiDirectory::refresh(bool force)
{
    if (m_busy || (!force && !isStale()))
        return;
    // The file is large: while the server fails, each use of the player
    // would ask again, so after a failure the automatic refresh waits an
    // hour. A forced one (the list is empty, and the listener shows the
    // player or confirms Settings) still asks.
    if (!force && m_sinceFailure.isValid() && m_sinceFailure.elapsed() < 3600 * 1000)
        return;
    QNetworkRequest req(m_url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("otd/") + QLatin1String(OTD_VERSION)
                      + QStringLiteral(" (+https://github.com/oh2gba/otd)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(90000);
    const QString ims = m_db->meta(kKeyModified);
    if (!ims.isEmpty() && !m_receivers.isEmpty())
        req.setRawHeader("If-Modified-Since", ims.toLatin1());
    m_busy = true;
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_busy = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 304)
        {
            m_db->setMeta(kKeyFetched, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status != 200)
        {
            m_sinceFailure.start();
            emit failed(reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                                 : QString::number(status));
            return;
        }
        const QByteArray data = reply->readAll();
        QString err;
        const QList<Receiver> list = parse(data, &err);
        if (list.size() < 10)
        {
            m_sinceFailure.start();
            emit failed(err.isEmpty() ? QStringLiteral("receiver list is empty") : err);
            return;
        }
        m_sinceFailure.invalidate();   // the server is fine again
        // keep only what the app uses, as compact JSON
        QJsonArray arr;
        for (const Receiver& r : list)
        {
            QJsonObject o;
            o.insert(QStringLiteral("url"), r.url);
            o.insert(QStringLiteral("name"), r.name);
            o.insert(QStringLiteral("loc"), r.location);
            o.insert(QStringLiteral("grid"), r.grid);
            o.insert(QStringLiteral("users"), QString::number(r.users));
            o.insert(QStringLiteral("users_max"), QString::number(r.usersMax));
            o.insert(QStringLiteral("bands"), QStringLiteral("%1-%2").arg(qint64(r.lowKHz * 1000)).arg(qint64(r.highKHz * 1000)));
            if (r.offline)
                o.insert(QStringLiteral("offline"), QStringLiteral("yes"));
            arr.push_back(o);
        }
        m_receivers = list;
        store(QJsonDocument(arr).toJson(QJsonDocument::Compact),
              QString::fromLatin1(reply->rawHeader("Last-Modified")));
        emit updated();
    });
}
