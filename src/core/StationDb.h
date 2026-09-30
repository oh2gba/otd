// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "EibiParser.h"
#include "HfccParser.h"
#include "Station.h"
#include "StationNames.h"

#include <QHash>
#include <QObject>
#include <QSqlDatabase>

// Persistent SQLite store for the imported schedules, the code tables and
// the settings; what the rows look like on screen is StationNames's.
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
    // Case-insensitive search in what the list shows (station, language,
    // country, site, target, remarks, frequency); every word must match, a
    // "!word" must not, "field:value" looks in one field only. Leaves out
    // the given sources. limit <= 0: every match.
    StationList search(const QString& text, const QStringList& excludeSources = QStringList(),
                       int limit = 0) const;
    // Every source id present, with its number of entries.
    QList<QPair<QString, int>> sourceCounts() const;

    QString meta(const QString& key, const QString& fallback = QString()) const;
    bool setMeta(const QString& key, const QString& value);

    // Lookup tables, per source and kind: a code and its name. EiBi's come
    // from its README (lang, country, target, site "ITU/code"), HFCC's from
    // its reference files (broadcaster, lang, admin, site, siteadm), Aoki's
    // say how it numbers the days (days "1" -> "Sunday" or "Monday").
    bool replaceCodes(const QString& source, const QHash<QString, QHash<QString, QString>>& kinds);
    bool storeCodes(const EibiParser::CodeTables& tables);
    bool storeHfccTables(const HfccParser::Tables& tables);
    // the name for a code, empty when the table has none
    QString code(const QString& source, const QString& kind, const QString& code) const;

    // What the list shows for an entry: its fields through these tables.
    const StationNames& names() const { return m_names; }

signals:
    void changed();

private:
    bool createSchema();
    bool migrate();
    void loadCodes();
    // the names table: what the list shows for each code in the data, so
    // the search finds names while the rows keep the codes
    void rebuildNames();
    static StationEntry fromRecord(const class QSqlQuery& q);
    static void bindEntry(class QSqlQuery& q, const StationEntry& e);

    QString m_filePath;
    QString m_connectionName;
    QString m_lastError;
    QHash<QString, QHash<QString, QString>> m_lookup;   // "source/kind" -> code -> name
    StationNames m_names;
};
