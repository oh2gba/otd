// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScheduleSource.h"
#include "EibiParser.h"
#include "StationDb.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <memory>

ScheduleSource::ScheduleSource(const QString& id, const QString& displayName,
                               const QUrl& defaultBase, StationDb* db,
                               QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_nam(nam)
    , m_id(id)
    , m_name(displayName)
    , m_defaultBase(defaultBase)
{
    setBaseUrl(defaultBase);
}

void ScheduleSource::setBaseUrl(const QUrl& base)
{
    m_base = base.isValid() && !base.isEmpty() ? base : m_defaultBase;
    if (!m_base.path().endsWith(QLatin1Char('/')))
        m_base.setPath(m_base.path() + QLatin1Char('/'));
}

QString ScheduleSource::season() const
{
    return m_db->meta(m_id + QStringLiteral(".season"));
}

QDateTime ScheduleSource::lastUpdate() const
{
    return QDateTime::fromString(m_db->meta(m_id + QStringLiteral(".updated")), Qt::ISODate);
}

int ScheduleSource::count() const
{
    return m_db->count(m_id);
}

bool ScheduleSource::isStale(int maxAgeDays) const
{
    const QDateTime last = lastUpdate();
    if (!last.isValid() || count() == 0)
        return true;
    if (season() != EibiParser::seasonCode(QDate::currentDate()))
        return true;
    return last.daysTo(QDateTime::currentDateTimeUtc()) >= maxAgeDays;
}

QNetworkReply* ScheduleSource::get(const QUrl& url, const QString& ifModifiedSince)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("otd/") + QLatin1String(OTD_VERSION)
                      + QStringLiteral(" (+https://github.com/oh2gba/otd)"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(90000);
    if (!ifModifiedSince.isEmpty())
        req.setRawHeader("If-Modified-Since", ifModifiedSince.toLatin1());
    return m_nam->get(req);
}

QNetworkReply* ScheduleSource::getFile(const QString& relativeName, const QString& ifModifiedSince)
{
    return get(m_base.resolved(QUrl(relativeName)), ifModifiedSince);
}

namespace
{
constexpr int kMaxAttempts = 6;

struct Transfer
{
    QUrl url;
    QString ifModifiedSince;
    QByteArray body;        // what has arrived so far, over all attempts
    qint64 total = -1;      // full length announced by the server, if any
    int attempt = 0;
    std::function<void(const ScheduleSource::Download&)> done;
    std::function<void()> start;   // one attempt; set once the lambda exists
};
}

void ScheduleSource::downloadFile(const QString& relativeName, const QString& ifModifiedSince,
                                  std::function<void(const Download&)> done)
{
    download(m_base.resolved(QUrl(relativeName)), ifModifiedSince, std::move(done));
}

void ScheduleSource::download(const QUrl& url, const QString& ifModifiedSince,
                              std::function<void(const Download&)> done)
{
    auto t = std::make_shared<Transfer>();
    t->url = url;
    t->ifModifiedSince = ifModifiedSince;
    t->done = std::move(done);

    // one attempt; runs again for the remainder when needed
    t->start = [this, t]() {
        ++t->attempt;
        const bool resuming = !t->body.isEmpty();
        QNetworkRequest req(t->url);
        req.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("otd/") + QLatin1String(OTD_VERSION)
                          + QStringLiteral(" (+https://github.com/oh2gba/otd)"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(90000);   // inactivity, not the whole transfer
        if (resuming)
            req.setRawHeader("Range", "bytes=" + QByteArray::number(t->body.size()) + "-");
        else if (!t->ifModifiedSince.isEmpty())
            req.setRawHeader("If-Modified-Since", t->ifModifiedSince.toLatin1());

        QNetworkReply* reply = m_nam->get(req);
        auto status = [reply]() {
            return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        };
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, t, status, resuming]() {
            const int code = status();
            if (code == 200 && resuming && t->total >= 0 && t->body.size() > 0)
                t->body.clear();   // the server ignored the Range and starts over
            if (code == 200 || code == 206)
            {
                if (t->total < 0 && code == 200)
                    t->total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
                t->body += reply->readAll();
                if (t->total > 0)
                    emit progress(tr("Downloading %1 ... %2 %")
                                      .arg(t->url.fileName())
                                      .arg(int(t->body.size() * 100 / t->total)));
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, t, status]() {
            reply->deleteLater();
            const int code = status();
            Download d;
            d.status = code;
            d.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
            if (code == 304)
            {
                t->done(d);
                return;
            }
            const bool ok = reply->error() == QNetworkReply::NoError && (code == 200 || code == 206);
            const bool complete = t->total < 0 ? ok : t->body.size() >= t->total;
            if (ok && complete)
            {
                d.status = 200;
                d.data = t->body;
                t->done(d);
                return;
            }
            // dropped halfway by a slow server: ask for the rest
            const bool started = (code == 200 || code == 206) && !t->body.isEmpty();
            if (started && t->attempt < kMaxAttempts)
            {
                emit progress(tr("Connection dropped, resuming %1 ...").arg(t->url.fileName()));
                t->start();
                return;
            }
            d.status = ok ? 0 : code;
            d.error = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                               : tr("incomplete download");
            t->done(d);
        });
    };
    t->start();
}

QString ScheduleSource::storedLastModified(const QString& season) const
{
    if (m_db->meta(m_id + QStringLiteral(".season")) != season || count() == 0)
        return QString();
    return m_db->meta(m_id + QStringLiteral(".lastModified"));
}

bool ScheduleSource::store(const QString& season, const StationList& entries,
                           const QString& lastModified)
{
    if (!m_db->replaceSource(m_id, entries))
        return false;
    m_db->setMeta(m_id + QStringLiteral(".season"), season);
    m_db->setMeta(m_id + QStringLiteral(".lastModified"), lastModified);
    touchUpdated();
    return true;
}

void ScheduleSource::touchUpdated()
{
    m_db->setMeta(m_id + QStringLiteral(".updated"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

bool ScheduleSource::begin()
{
    if (m_busy)
        return false;
    m_busy = true;
    return true;
}

void ScheduleSource::finish(bool ok, const QString& message)
{
    m_busy = false;
    emit finished(ok, message);
}
