// SPDX-License-Identifier: GPL-3.0-or-later
#include "AokiSource.h"
#include "AokiParser.h"
#include "EibiParser.h"
#include "StationDb.h"
#include "ZipReader.h"

#include <QNetworkReply>
#include <QRegularExpression>

AokiSource::AokiSource(StationDb* db, QNetworkAccessManager* nam, QObject* parent)
    : ScheduleSource(QStringLiteral("aoki"), QStringLiteral("Aoki"),
                     QUrl(QStringLiteral("http://www1.s2.starcat.ne.jp/ndxc/")), db, nam, parent)
{
}

QUrl AokiSource::findZipLink(const QByteArray& html, const QString& season, const QUrl& base)
{
    // The page is Shift-JIS but the hrefs are plain ASCII.
    const QString text = QString::fromLatin1(html);
    const QRegularExpression re(
        QStringLiteral("href=\"([^\"]*?[a-z]{2}%1\\.zip)\"").arg(season),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return QUrl();
    return base.resolved(QUrl(m.captured(1)));
}

void AokiSource::update()
{
    if (!begin())
        return;
    fetchIndex();
}

void AokiSource::fetchIndex()
{
    emit progress(tr("Reading Aoki index page ..."));
    QNetworkReply* reply = getFile(QStringLiteral("nnk.htm"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            finish(false, tr("Aoki index not available: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray html = reply->readAll();
        QString season = EibiParser::seasonCode(QDate::currentDate());
        QUrl zip = findZipLink(html, season, baseUrl());
        if (zip.isEmpty())
        {
            season = EibiParser::previousSeason(season);
            zip = findZipLink(html, season, baseUrl());
        }
        if (zip.isEmpty())
        {
            finish(false, tr("Aoki index has no link to a current season zip"));
            return;
        }
        fetchZip(zip, season);
    });
}

void AokiSource::fetchZip(const QUrl& url, const QString& season)
{
    emit progress(tr("Checking Aoki %1 ...").arg(url.fileName()));
    download(url, storedLastModified(season), [this, season](const Download& dl) {
        if (dl.status == 304)
        {
            touchUpdated();
            finish(true, tr("Aoki %1 is up to date (%2 entries)").arg(season.toUpper()).arg(count()));
            return;
        }
        if (dl.status != 200)
        {
            finish(false, tr("Aoki download failed: %1")
                              .arg(dl.status ? QString::number(dl.status) : dl.error));
            return;
        }

        QString zipError;
        const QHash<QString, QByteArray> files = ZipReader::extractAll(dl.data, &zipError);
        // "xta26.txt", never "userlist1.txt"
        const QString name = ZipReader::findName(
            files, QStringLiteral("^[a-z]{2}%1\\.txt$").arg(season));
        if (name.isEmpty())
        {
            finish(false, tr("Aoki archive has no schedule text file (%1)").arg(zipError));
            return;
        }
        const AokiParser::ParseResult parsed = AokiParser::parse(files.value(name));
        if (!parsed.error.isEmpty() || parsed.entries.size() < 100)
        {
            finish(false, tr("Aoki list unusable: %1").arg(parsed.error));
            return;
        }
        emit progress(tr("Storing %1 Aoki entries ...").arg(parsed.entries.size()));
        // how this list numbers the days, for reading its days column
        const QHash<QString, QHash<QString, QString>> days = {
            {QStringLiteral("days"),
             {{QStringLiteral("1"), parsed.sundayFirst ? QStringLiteral("Sunday") : QStringLiteral("Monday")}}}};
        if (!m_db->replaceCodes(id(), days) || !store(season, parsed.entries, dl.lastModified))
        {
            finish(false, tr("Database error: %1").arg(m_db->lastError()));
            return;
        }
        finish(true, tr("Aoki %1 loaded, %2 entries").arg(season.toUpper()).arg(parsed.entries.size()));
    });
}
