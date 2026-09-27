// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "EibiParser.h"
#include "Station.h"

#include <QHash>
#include <QObject>
#include <QSqlDatabase>

// Persistent SQLite store for the imported schedules and code tables.
class StationDb : public QObject
{
    Q_OBJECT
public:
    explicit StationDb(const QString& filePath, QObject* parent = nullptr);
    ~StationDb() override;

    bool open();
    QString lastError() const { return m_lastError; }
    QString filePath() const { return m_filePath; }

    // Replace every entry of a source in one transaction.
    bool replaceSource(const QString& source, const StationList& entries);
    // Single entries (used for the personal list). insert sets entry.id.
    bool insertEntry(StationEntry& entry);
    bool updateEntry(const StationEntry& entry);
    bool removeEntry(qint64 id);
    StationList entriesOf(const QString& source) const;
    int count(const QString& source = QString()) const;

    // National allocation tables (what a sub-band is used for), e.g. Traficom.
    struct Allocation
    {
        double lowKHz = 0.0;
        double highKHz = 0.0;
        QString service, usage, info, mode, emission, comment;
    };
    bool replaceAllocations(const QString& source, const QList<Allocation>& rows);
    int allocationCount(const QString& source) const;
    // every stored sub-band containing the frequency, narrowest first
    QList<Allocation> allocationsAt(double kHz) const;

    // All entries with |kHz - centre| <= tolerance, nearest first, leaving
    // out the given sources.
    StationList lookup(double centreKHz, double toleranceKHz,
                       const QStringList& excludeSources = QStringList()) const;
    // Up to `count` entries below and `count` entries at or above the centre
    // frequency, ordered by frequency, leaving out the given sources.
    StationList around(double centreKHz, int count, const QStringList& excludeSources = QStringList()) const;
    // Case-insensitive substring search on station name, language, site and
    // country code, optionally limited to some sources. limit <= 0: every match.
    StationList search(const QString& text, const QStringList& excludeSources = QStringList(),
                       int limit = 0) const;
    // Every source id present, with its number of entries.
    QList<QPair<QString, int>> sourceCounts() const;

    QString meta(const QString& key, const QString& fallback = QString()) const;
    bool setMeta(const QString& key, const QString& value);

    bool storeCodes(const EibiParser::CodeTables& tables);
    const EibiParser::CodeTables& codes() const { return m_codes; }
    QString languageName(const QString& code) const;
    QString countryName(const QString& code) const;
    QString targetName(const QString& code) const;
    // Human readable transmitter site: resolves "/RUS-s" to "Samara (Russia)".
    QString siteName(const QString& homeItu, const QString& siteCode) const;

signals:
    void changed();

private:
    bool createSchema();
    void loadCodes();
    static StationEntry fromRecord(const class QSqlQuery& q);

    QString m_filePath;
    QString m_connectionName;
    QString m_lastError;
    EibiParser::CodeTables m_codes;
};
