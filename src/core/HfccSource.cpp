// SPDX-License-Identifier: GPL-3.0-or-later
#include "HfccSource.h"
#include "EibiParser.h"
#include "HfccParser.h"
#include "StationDb.h"
#include "ZipReader.h"

#include <QNetworkReply>

HfccSource::HfccSource(StationDb* db, QNetworkAccessManager* nam, QObject* parent)
    : ScheduleSource(QStringLiteral("hfcc"), QStringLiteral("HFCC"),
                     QUrl(QStringLiteral("http://www.hfcc.org/data/")), db, nam, parent)
{
}

void HfccSource::update()
{
    if (!begin())
        return;
    fetchZip(EibiParser::seasonCode(QDate::currentDate()), true);
}

void HfccSource::fetchZip(const QString& season, bool allowFallback)
{
    const QString file = QStringLiteral("%1/%1allx2.zip").arg(season);
    emit progress(tr("Checking HFCC %1 ...").arg(file));
    downloadFile(file, storedLastModified(season), [this, season, allowFallback](const Download& dl) {
        if (dl.status == 304)
        {
            touchUpdated();
            finish(true, tr("HFCC %1 is up to date (%2 entries)").arg(season.toUpper()).arg(count()));
            return;
        }
        if (dl.status != 200)
        {
            if (allowFallback && (dl.status == 404 || dl.status == 403))
            {
                fetchZip(EibiParser::previousSeason(season), false);
                return;
            }
            finish(false, tr("HFCC download failed: %1")
                              .arg(dl.status ? QString::number(dl.status) : dl.error));
            return;
        }

        QString zipError;
        const QHash<QString, QByteArray> files = ZipReader::extractAll(dl.data, &zipError);
        const QString skedName = ZipReader::findName(files, QStringLiteral("^[ab]\\d\\dall\\d\\d\\.txt$"));
        if (skedName.isEmpty())
        {
            finish(false, tr("HFCC archive has no schedule file (%1)").arg(zipError));
            return;
        }
        auto fileNamed = [&files](const QString& pattern) {
            const QString name = ZipReader::findName(files, pattern);
            return name.isEmpty() ? QByteArray() : files.value(name);
        };
        const HfccParser::Tables tables = HfccParser::parseTables(
            fileNamed(QStringLiteral("^site\\.txt$")), fileNamed(QStringLiteral("^broadcas\\.txt$")),
            fileNamed(QStringLiteral("^language\\.txt$")), fileNamed(QStringLiteral("^admin\\.txt$")));

        const HfccParser::ParseResult parsed = HfccParser::parseSchedule(files.value(skedName));
        if (!parsed.error.isEmpty() || parsed.entries.size() < 100)
        {
            finish(false, tr("HFCC schedule unusable: %1").arg(parsed.error));
            return;
        }
        emit progress(tr("Storing %1 HFCC entries ...").arg(parsed.entries.size()));
        // the reference files are the lookup tables for the codes in the lines
        if (!m_db->storeHfccTables(tables) || !store(season, parsed.entries, dl.lastModified))
        {
            finish(false, tr("Database error: %1").arg(m_db->lastError()));
            return;
        }
        finish(true, tr("HFCC %1 loaded, %2 entries").arg(season.toUpper()).arg(parsed.entries.size()));
    });
}
