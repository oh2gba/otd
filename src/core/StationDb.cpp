// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationDb.h"
#include "Ciraf.h"
#include "EibiParser.h"
#include "HfccParser.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

static const char* kSelectColumns =
    "source, khz, start_min, end_min, days, itu, station, lang, target, site,"
    " persistence, start_date, stop_date, last_heard, remarks, power, azimuth, flag, mode, id";
static const char* kInsertColumns =
    "source, khz, start_min, end_min, days, itu, station, lang, target, site,"
    " persistence, start_date, stop_date, last_heard, remarks, power, azimuth, flag, mode";
static const char* kInsertMarks = "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?";
static const char* kCreateStations =
    "CREATE TABLE IF NOT EXISTS stations ("
    " id INTEGER PRIMARY KEY,"
    " source TEXT NOT NULL,"
    " khz REAL NOT NULL,"
    " start_min INTEGER NOT NULL,"
    " end_min INTEGER NOT NULL,"
    " days TEXT, itu TEXT, station TEXT, lang TEXT, target TEXT, site TEXT,"
    " persistence INTEGER, start_date TEXT, stop_date TEXT, last_heard TEXT,"
    " remarks TEXT, power TEXT, azimuth TEXT, flag TEXT, mode TEXT)";
// lookup tables: a source's codes and their names, by kind
static const char* kCreateCodes =
    "CREATE TABLE IF NOT EXISTS codes ("
    " source TEXT NOT NULL, kind TEXT NOT NULL, code TEXT NOT NULL, name TEXT,"
    " PRIMARY KEY (source, kind, code))";

// the sources whose files otd downloads (and can download again)
static const QStringList& downloadedSources()
{
    static const QStringList s = {QStringLiteral("eibi"), QStringLiteral("hfcc"), QStringLiteral("aoki")};
    return s;
}

StationDb::StationDb(const QString& filePath, QObject* parent)
    : QObject(parent)
    , m_filePath(filePath)
    , m_connectionName(QStringLiteral("stationdb-") + QUuid::createUuid().toString(QUuid::Id128))
{
}

StationDb::~StationDb()
{
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool StationDb::open()
{
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_filePath);
    if (!db.open())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    QSqlQuery pragma(db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!createSchema() || !migrate())
        return false;
    loadCodes();
    rebuildNames();
    return true;
}

bool StationDb::migrate()
{
    // Version 3: the imported rows hold each source's data as published
    // (earlier versions stored names, remarks and modes the importer had
    // made up). What was imported is dropped and downloaded again; entries
    // otd cannot download again (the personal list, other imports) stay.
    static const QString kDataVersion = QStringLiteral("3");
    if (meta(QStringLiteral("schema.dataVersion")) == kDataVersion)
        return true;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    auto columnsOf = [&db](const QString& table) {
        QStringList columns;
        QSqlQuery info(db);
        if (info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
            while (info.next())
                columns << info.value(1).toString();
        return columns;
    };
    const bool oldStations = columnsOf(QStringLiteral("stations")).contains(QStringLiteral("lang_text"));
    const bool oldCodes = !columnsOf(QStringLiteral("codes")).contains(QStringLiteral("source"));
    if (!db.transaction())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    QStringList steps;
    const QString downloaded = QStringLiteral("'") + downloadedSources().join(QStringLiteral("','")) + QStringLiteral("'");
    if (oldStations)
    {
        // the old layout: keep what cannot be downloaded again, with the
        // names it was entered with as its language and site
        steps << QStringLiteral("ALTER TABLE stations RENAME TO stations_v2")
              << QStringLiteral("DROP INDEX IF EXISTS idx_stations_khz")
              << QStringLiteral("DROP INDEX IF EXISTS idx_stations_source")
              << QString::fromLatin1(kCreateStations)
              << QStringLiteral("INSERT INTO stations (source, khz, start_min, end_min, days, itu, station, lang,"
                                " target, site, persistence, start_date, stop_date, last_heard, remarks, mode)"
                                " SELECT source, khz, start_min, end_min, days, itu, station,"
                                " COALESCE(NULLIF(lang_text,''), lang), target, COALESCE(NULLIF(site_text,''), site),"
                                " persistence, start_date, stop_date, last_heard, remarks, mode"
                                " FROM stations_v2 WHERE source NOT IN (%1)").arg(downloaded)
              << QStringLiteral("DROP TABLE stations_v2")
              << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_khz ON stations(khz)")
              << QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_source ON stations(source)");
    }
    else
        steps << QStringLiteral("DELETE FROM stations WHERE source IN (%1)").arg(downloaded);
    if (oldCodes)
        steps << QStringLiteral("DROP TABLE codes") << QString::fromLatin1(kCreateCodes);
    else
        steps << QStringLiteral("DELETE FROM codes");
    // every source counts as never downloaded, so the next update fetches it
    steps << QStringLiteral("DELETE FROM meta WHERE key LIKE '%.lastModified' OR key LIKE '%.updated'");
    for (const QString& sql : steps)
    {
        QSqlQuery q(db);
        if (!q.exec(sql))
        {
            m_lastError = q.lastError().text();
            db.rollback();
            return false;
        }
    }
    if (!db.commit())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    setMeta(QStringLiteral("schema.dataVersion"), kDataVersion);
    return true;
}

bool StationDb::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    const QStringList statements = {
        QString::fromLatin1(kCreateStations),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_khz ON stations(khz)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_source ON stations(source)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"),
        QString::fromLatin1(kCreateCodes),
        // what the list shows for each code in the data, for the search
        QStringLiteral("CREATE TABLE IF NOT EXISTS names ("
                       " source TEXT NOT NULL, kind TEXT NOT NULL, value TEXT NOT NULL, name TEXT,"
                       " PRIMARY KEY (source, kind, value))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS allocations ("
                       " source TEXT NOT NULL, low_khz REAL NOT NULL, high_khz REAL NOT NULL,"
                       " service TEXT, usage TEXT, info TEXT, mode TEXT, emission TEXT, comment TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alloc_low ON allocations(low_khz)"),
    };
    for (const QString& sql : statements)
    {
        QSqlQuery q(db);
        if (!q.exec(sql))
        {
            m_lastError = q.lastError().text();
            return false;
        }
    }
    return true;
}

bool StationDb::replaceSource(const QString& source, const StationList& entries)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
    {
        m_lastError = db.lastError().text();
        return false;
    }

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM stations WHERE source = ?"));
    del.addBindValue(source);
    if (!del.exec())
    {
        m_lastError = del.lastError().text();
        db.rollback();
        return false;
    }

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT INTO stations (%1) VALUES (%2)")
                    .arg(QLatin1String(kInsertColumns), QLatin1String(kInsertMarks)));
    for (const StationEntry& e : entries)
    {
        StationEntry row = e;
        row.source = source;
        bindEntry(ins, row);
        if (!ins.exec())
        {
            m_lastError = ins.lastError().text();
            db.rollback();
            return false;
        }
    }

    if (!db.commit())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    rebuildNames();
    emit changed();
    return true;
}

void StationDb::bindEntry(QSqlQuery& q, const StationEntry& e)
{
    q.addBindValue(e.source);
    q.addBindValue(e.kHz);
    q.addBindValue(e.startMin);
    q.addBindValue(e.endMin);
    q.addBindValue(e.days);
    q.addBindValue(e.itu);
    q.addBindValue(e.station);
    q.addBindValue(e.lang);
    q.addBindValue(e.target);
    q.addBindValue(e.site);
    q.addBindValue(e.persistence);
    q.addBindValue(e.startDate);
    q.addBindValue(e.stopDate);
    q.addBindValue(e.lastHeard);
    q.addBindValue(e.remarks);
    q.addBindValue(e.power);
    q.addBindValue(e.azimuth);
    q.addBindValue(e.flag);
    q.addBindValue(e.mode);
}

bool StationDb::insertEntry(StationEntry& e)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT INTO stations (%1) VALUES (%2)")
                  .arg(QLatin1String(kInsertColumns), QLatin1String(kInsertMarks)));
    bindEntry(q, e);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
    e.id = q.lastInsertId().toLongLong();
    rebuildNames();
    emit changed();
    return true;
}

bool StationDb::updateEntry(const StationEntry& e)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE stations SET source=?, khz=?, start_min=?, end_min=?, days=?, itu=?, station=?,"
        " lang=?, target=?, site=?, persistence=?, start_date=?, stop_date=?, last_heard=?,"
        " remarks=?, power=?, azimuth=?, flag=?, mode=? WHERE id=?"));
    bindEntry(q, e);
    q.addBindValue(e.id);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
    rebuildNames();
    emit changed();
    return q.numRowsAffected() > 0;
}

bool StationDb::removeEntry(qint64 id)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM stations WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
    emit changed();
    return q.numRowsAffected() > 0;
}

StationList StationDb::entriesOf(const QString& source) const
{
    StationList out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM stations WHERE source=? ORDER BY khz, start_min")
                  .arg(QLatin1String(kSelectColumns)));
    q.addBindValue(source);
    if (q.exec())
        while (q.next())
            out.push_back(fromRecord(q));
    return out;
}

int StationDb::count(const QString& source) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (source.isEmpty())
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM stations"));
    else
    {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM stations WHERE source = ?"));
        q.addBindValue(source);
    }
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

StationEntry StationDb::fromRecord(const QSqlQuery& q)
{
    StationEntry e;
    e.source = q.value(0).toString();
    e.kHz = q.value(1).toDouble();
    e.startMin = q.value(2).toInt();
    e.endMin = q.value(3).toInt();
    e.days = q.value(4).toString();
    e.itu = q.value(5).toString();
    e.station = q.value(6).toString();
    e.lang = q.value(7).toString();
    e.target = q.value(8).toString();
    e.site = q.value(9).toString();
    e.persistence = q.value(10).toInt();
    e.startDate = q.value(11).toString();
    e.stopDate = q.value(12).toString();
    e.lastHeard = q.value(13).toString();
    e.remarks = q.value(14).toString();
    e.power = q.value(15).toString();
    e.azimuth = q.value(16).toString();
    e.flag = q.value(17).toString();
    e.mode = q.value(18).toString();
    e.id = q.value(19).toLongLong();
    return e;
}


StationList StationDb::lookup(double centreKHz, double toleranceKHz,
                              const QStringList& sources) const
{
    StationList out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QString sql = QStringLiteral("SELECT %1 FROM stations WHERE khz BETWEEN ? AND ?")
                      .arg(QLatin1String(kSelectColumns));
    if (!sources.isEmpty())
    {
        QStringList marks;
        for (int i = 0; i < sources.size(); ++i)
            marks << QStringLiteral("?");
        sql += QStringLiteral(" AND source NOT IN (%1)").arg(marks.join(QLatin1Char(',')));
    }
    sql += QStringLiteral(" ORDER BY ABS(khz - ?), start_min");
    q.prepare(sql);
    q.addBindValue(centreKHz - toleranceKHz);
    q.addBindValue(centreKHz + toleranceKHz);
    for (const QString& src : sources)
        q.addBindValue(src);
    q.addBindValue(centreKHz);
    if (q.exec())
        while (q.next())
            out.push_back(fromRecord(q));
    return out;
}

StationList StationDb::around(double centreKHz, int count, const QStringList& sources) const
{
    StationList out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QString notIn;
    if (!sources.isEmpty())
    {
        QStringList marks;
        for (int i = 0; i < sources.size(); ++i)
            marks << QStringLiteral("?");
        notIn = QStringLiteral(" AND source NOT IN (%1)").arg(marks.join(QLatin1Char(',')));
    }
    // below: nearest first, then reversed into ascending order
    QSqlQuery below(db);
    below.prepare(QStringLiteral("SELECT %1 FROM stations WHERE khz < ?%2 ORDER BY khz DESC, start_min DESC LIMIT ?")
                      .arg(QLatin1String(kSelectColumns), notIn));
    below.addBindValue(centreKHz);
    for (const QString& src : sources)
        below.addBindValue(src);
    below.addBindValue(count);
    StationList lower;
    if (below.exec())
        while (below.next())
            lower.push_back(fromRecord(below));
    for (int i = lower.size() - 1; i >= 0; --i)
        out.push_back(lower[i]);

    QSqlQuery above(db);
    above.prepare(QStringLiteral("SELECT %1 FROM stations WHERE khz >= ?%2 ORDER BY khz, start_min LIMIT ?")
                      .arg(QLatin1String(kSelectColumns), notIn));
    above.addBindValue(centreKHz);
    for (const QString& src : sources)
        above.addBindValue(src);
    above.addBindValue(count);
    if (above.exec())
        while (above.next())
            out.push_back(fromRecord(above));
    return out;
}

const QStringList& StationDb::searchFields()
{
    static const QStringList fields = {QStringLiteral("station"), QStringLiteral("language"),
                                       QStringLiteral("country"), QStringLiteral("site"),
                                       QStringLiteral("target"), QStringLiteral("mode")};
    return fields;
}

QList<StationDb::SearchTerm> StationDb::parseSearch(const QString& text)
{
    // word | !word | field:value | field:"two words" (the closing quote may
    // still be missing while it is typed); an unknown field is a plain word
    static const QRegularExpression token(
        QStringLiteral("(!?)([A-Za-z]+):\"([^\"]*)\"?|(!?)([A-Za-z]+):(\\S*)|(\\S+)"));
    QList<SearchTerm> terms;
    auto it = token.globalMatch(text);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        SearchTerm t;
        const bool quoted = m.capturedStart(2) >= 0;
        if (quoted || m.capturedStart(5) >= 0)
        {
            QString field = m.captured(quoted ? 2 : 5).toLower();
            if (field == QLatin1String("lang"))
                field = QStringLiteral("language");
            if (searchFields().contains(field))
            {
                t.negate = !m.captured(quoted ? 1 : 4).isEmpty();
                t.field = field;
                t.value = m.captured(quoted ? 3 : 6).trimmed();
                if (!t.value.isEmpty())   // "target:" alone: a value is still being picked
                    terms << t;
                continue;
            }
        }
        QString word = m.captured(0);
        if (word.startsWith(QLatin1Char('!')))
        {
            t.negate = true;
            word.remove(0, 1);
        }
        if (word.isEmpty())
            continue;
        t.value = word;
        terms << t;
    }
    return terms;
}

StationList StationDb::search(const QString& text, const QStringList& sources, int limit) const
{
    StationList out;
    // Every word must be found somewhere in the entry ("bbc english" gives
    // the BBC's English programmes), the frequency written as a number
    // included ("4625"); a leading "!" turns a word into an exclusion
    // ("!china english": English, but not from China). "field:value"
    // looks in that field only (target:"North Europe", !country:china).
    const QList<SearchTerm> terms = parseSearch(text);
    if (terms.isEmpty())
        return out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QString sql = QStringLiteral("SELECT %1 FROM stations WHERE 1=1").arg(QLatin1String(kSelectColumns));
    QVariantList binds;
    // Words are looked for in what the list shows: the fields where they are
    // shown as the source gives them, otherwise the names the lookup tables
    // give the codes (the names table, see rebuildNames).
    // IFNULL: a NULL column would make a negated group NULL, i.e. false.
    const QString esc = QStringLiteral(" ESCAPE '\\'");
    auto raw = [&](const QString& column, const QString& like, const QString& where) {
        binds << like;
        return QStringLiteral("(%1IFNULL(%2,'') LIKE ?%3)").arg(where, column, esc);
    };
    auto named = [&](const QString& kind, const QString& value, const QString& like) {
        binds << like;
        return QStringLiteral("source||'|'||%2 IN (SELECT source||'|'||value FROM names"
                              " WHERE kind='%1' AND name LIKE ?%3)").arg(kind, value, esc);
    };
    const QString plain = QStringLiteral("source NOT IN ('eibi','hfcc') AND ");
    const QString siteKey = QStringLiteral("IFNULL(itu,'')||'|'||IFNULL(site,'')");
    QList<SearchTerm> modeTerms;
    for (const SearchTerm& t : terms)
    {
        QString pattern = t.value;
        pattern.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
               .replace(QLatin1Char('%'), QLatin1String("\\%"))
               .replace(QLatin1Char('_'), QLatin1String("\\_"));
        const QString like = QLatin1Char('%') + pattern + QLatin1Char('%');
        QStringList any;
        if (t.field == QLatin1String("mode"))
        {
            modeTerms << t;   // worked out per entry, filtered below
            continue;
        }
        if (t.field.isEmpty() || t.field == QLatin1String("station"))
            any << raw(QStringLiteral("station"), like, QStringLiteral("source <> 'hfcc' AND "))
                << named(QStringLiteral("station"), QStringLiteral("IFNULL(station,'')"), like);
        if (t.field.isEmpty() || t.field == QLatin1String("language"))
            any << raw(QStringLiteral("lang"), like, plain)
                << named(QStringLiteral("lang"), QStringLiteral("IFNULL(lang,'')"), like);
        if (t.field.isEmpty() || t.field == QLatin1String("site"))
            any << raw(QStringLiteral("site"), like, plain) << named(QStringLiteral("site"), siteKey, like);
        if (t.field.isEmpty() || t.field == QLatin1String("target"))
            any << raw(QStringLiteral("target"), like, plain)
                << named(QStringLiteral("target"), QStringLiteral("IFNULL(target,'')"), like);
        if (t.field.isEmpty() || t.field == QLatin1String("country"))
        {
            binds << t.value.toUpper();
            any << QStringLiteral("IFNULL(itu,'') = ?")
                << named(QStringLiteral("country"), QStringLiteral("IFNULL(itu,'')"), like);
        }
        if (t.field.isEmpty())
        {
            any << raw(QStringLiteral("remarks"), like, QString());
            // the frequency as written in the list: all three decimals,
            // trailing zeros and point dropped
            binds << like;
            any << QStringLiteral("rtrim(rtrim(printf('%.3f', khz), '0'), '.') LIKE ?") + esc;
        }
        sql += QStringLiteral(" AND %1(%2)").arg(t.negate ? QStringLiteral("NOT ") : QString(),
                                               any.join(QStringLiteral(" OR ")));
    }
    if (!sources.isEmpty())
    {
        QStringList marks;
        for (const QString& src : sources)
        {
            marks << QStringLiteral("?");
            binds << src;
        }
        sql += QStringLiteral(" AND source NOT IN (%1)").arg(marks.join(QLatin1Char(',')));
    }
    sql += QStringLiteral(" ORDER BY khz, start_min");
    if (limit > 0 && modeTerms.isEmpty())
    {
        sql += QStringLiteral(" LIMIT ?");
        binds << limit;
    }
    q.prepare(sql);
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec())
        return out;
    while (q.next())
    {
        const StationEntry e = fromRecord(q);
        bool keep = true;
        for (const SearchTerm& t : modeTerms)
            if (modeOf(e).contains(t.value, Qt::CaseInsensitive) == t.negate)
                keep = false;
        if (!keep)
            continue;
        out.push_back(e);
        if (limit > 0 && out.size() >= limit)
            break;
    }
    return out;
}

QList<QPair<QString, int>> StationDb::sourceCounts() const
{
    QList<QPair<QString, int>> out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT source, COUNT(*) FROM stations GROUP BY source ORDER BY source")))
        while (q.next())
            out.append({q.value(0).toString(), q.value(1).toInt()});
    return out;
}

QString StationDb::meta(const QString& key, const QString& fallback) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0).toString();
    return fallback;
}

bool StationDb::setMeta(const QString& key, const QString& value)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)"));
    q.addBindValue(key);
    q.addBindValue(value);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
    return true;
}

// ---- lookup tables ------------------------------------------------------

bool StationDb::replaceCodes(const QString& source, const QHash<QString, QHash<QString, QString>>& kinds)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM codes WHERE source = ?"));
    del.addBindValue(source);
    bool ok = del.exec();
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT OR REPLACE INTO codes (source, kind, code, name) VALUES (?,?,?,?)"));
    for (auto k = kinds.cbegin(); ok && k != kinds.cend(); ++k)
        for (auto it = k.value().cbegin(); ok && it != k.value().cend(); ++it)
        {
            ins.addBindValue(source);
            ins.addBindValue(k.key());
            ins.addBindValue(it.key());
            ins.addBindValue(it.value());
            ok = ins.exec();
        }
    if (!ok)
    {
        m_lastError = del.lastError().isValid() ? del.lastError().text() : ins.lastError().text();
        db.rollback();
        return false;
    }
    db.commit();
    for (auto it = m_lookup.begin(); it != m_lookup.end();)
        it = it.key().startsWith(source + QLatin1Char('/')) ? m_lookup.erase(it) : std::next(it);
    for (auto k = kinds.cbegin(); k != kinds.cend(); ++k)
        m_lookup.insert(source + QLatin1Char('/') + k.key(), k.value());
    rebuildNames();
    emit changed();
    return true;
}

bool StationDb::storeCodes(const EibiParser::CodeTables& tables)
{
    return replaceCodes(QStringLiteral("eibi"), {{QStringLiteral("lang"), tables.languages},
                                                 {QStringLiteral("country"), tables.countries},
                                                 {QStringLiteral("target"), tables.targets},
                                                 {QStringLiteral("site"), tables.sites}});
}

bool StationDb::storeHfccTables(const HfccParser::Tables& tables)
{
    QHash<QString, QString> sites, siteAdmins;
    for (auto it = tables.sites.cbegin(); it != tables.sites.cend(); ++it)
    {
        sites.insert(it.key(), it->name);
        siteAdmins.insert(it.key(), it->adm);
    }
    return replaceCodes(QStringLiteral("hfcc"), {{QStringLiteral("broadcaster"), tables.broadcasters},
                                                 {QStringLiteral("lang"), tables.languages},
                                                 {QStringLiteral("admin"), tables.admins},
                                                 {QStringLiteral("site"), sites},
                                                 {QStringLiteral("siteadm"), siteAdmins}});
}

void StationDb::loadCodes()
{
    m_lookup.clear();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT source, kind, code, name FROM codes")))
        return;
    while (q.next())
        m_lookup[q.value(0).toString() + QLatin1Char('/') + q.value(1).toString()]
            .insert(q.value(2).toString(), q.value(3).toString());
}

QString StationDb::code(const QString& source, const QString& kind, const QString& code) const
{
    const auto table = m_lookup.constFind(source + QLatin1Char('/') + kind);
    return table == m_lookup.cend() ? QString() : table->value(code);
}

void StationDb::rebuildNames()
{
    // For each code in the data, what the list shows for it: the search
    // looks there instead of in the rows, which keep the codes.
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        return;
    QSqlQuery clear(db);
    clear.exec(QStringLiteral("DELETE FROM names"));
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT OR REPLACE INTO names (source, kind, value, name) VALUES (?,?,?,?)"));
    auto add = [&ins](const QString& source, const QString& kind, const QString& value, const QString& name) {
        ins.addBindValue(source);
        ins.addBindValue(kind);
        ins.addBindValue(value);
        ins.addBindValue(name);
        ins.exec();
    };
    const QString coded = QStringLiteral("source IN ('eibi','hfcc')");
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT DISTINCT source, station FROM stations WHERE source = 'hfcc'")))
        while (q.next())
        {
            StationEntry e;
            e.source = q.value(0).toString();
            e.station = q.value(1).toString();
            add(e.source, QStringLiteral("station"), e.station, stationOf(e));
        }
    if (q.exec(QStringLiteral("SELECT DISTINCT source, IFNULL(lang,'') FROM stations WHERE %1").arg(coded)))
        while (q.next())
        {
            StationEntry e;
            e.source = q.value(0).toString();
            e.lang = q.value(1).toString();
            add(e.source, QStringLiteral("lang"), e.lang, languageOf(e));
        }
    if (q.exec(QStringLiteral("SELECT DISTINCT source, IFNULL(target,'') FROM stations WHERE %1").arg(coded)))
        while (q.next())
        {
            StationEntry e;
            e.source = q.value(0).toString();
            e.target = q.value(1).toString();
            add(e.source, QStringLiteral("target"), e.target, targetOf(e));
        }
    if (q.exec(QStringLiteral("SELECT DISTINCT source, IFNULL(itu,''), IFNULL(site,'') FROM stations WHERE %1").arg(coded)))
        while (q.next())
        {
            StationEntry e;
            e.source = q.value(0).toString();
            e.itu = q.value(1).toString();
            e.site = q.value(2).toString();
            add(e.source, QStringLiteral("site"), e.itu + QLatin1Char('|') + e.site, siteOf(e));
        }
    if (q.exec(QStringLiteral("SELECT DISTINCT source, IFNULL(itu,'') FROM stations")))
        while (q.next())
        {
            StationEntry e;
            e.source = q.value(0).toString();
            e.itu = q.value(1).toString();
            add(e.source, QStringLiteral("country"), e.itu, countryOf(e));
        }
    db.commit();
}

// ---- what the list shows -------------------------------------------------

QString StationDb::languageText(const QString& codes, bool details) const
{
    QStringList names;
    bool known = false;
    for (const QString& part : codes.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString c = part.trimmed();
        QString name = code(QStringLiteral("eibi"), QStringLiteral("lang"), c);
        if (name.isEmpty())
        {
            names << c;
            continue;
        }
        known = true;
        // "Finnish: Finland (5m)" -> "Finnish"; the countries after the
        // colon are where it is spoken, not the language
        const int colon = name.indexOf(QLatin1Char(':'));
        names << (colon > 0 && !details ? name.left(colon) : name).trimmed();
    }
    // the descriptions have commas of their own
    return known ? names.join(details ? QStringLiteral("; ") : QStringLiteral(", ")) : QString();
}

QString StationDb::targetText(const QString& codes, bool details) const
{
    const QString eibi = QStringLiteral("eibi"), targets = QStringLiteral("target");
    // "Europe (often including North Africa/Middle East)" -> "Europe" in the list
    auto shortName = [](const QString& name) {
        const int paren = name.indexOf(QStringLiteral(" ("));
        return paren > 0 ? name.left(paren) : name;
    };
    QStringList names;
    bool known = false;
    for (const QString& part : codes.split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        const QString c = part.trimmed();
        QString name = code(eibi, targets, c);
        if (!name.isEmpty())
            name = details ? name : shortName(name);
        else if (!code(eibi, QStringLiteral("country"), c).isEmpty())
            name = code(eibi, QStringLiteral("country"), c);
        else if (c.size() >= 3 && QStringLiteral("NSEWC").contains(c.at(0)))
        {
            // EiBi's README gives the parts: "N.." is "North ..", "Eu" is
            // Europe, so "NEu" is North Europe
            const QString prefix = code(eibi, targets, c.left(1) + QStringLiteral(".."));
            const QString base = code(eibi, targets, c.mid(1));
            if (prefix.contains(QStringLiteral("..")) && !base.isEmpty())
                name = QString(prefix).replace(QStringLiteral(".."), shortName(base)).simplified();
        }
        if (name.isEmpty())
        {
            names << c;
            continue;
        }
        known = true;
        names << name;
    }
    return known ? names.join(details ? QStringLiteral("; ") : QStringLiteral(", ")) : QString();
}

QString StationDb::countryName(const QString& itu) const
{
    const QString name = code(QStringLiteral("eibi"), QStringLiteral("country"), itu);
    return name.isEmpty() ? itu : name;
}

QString StationDb::siteName(const QString& homeItu, const QString& siteCode) const
{
    QString itu = homeItu;
    QString c = siteCode;
    bool relayed = false;
    if (c.startsWith(QLatin1Char('/')))
    {
        relayed = true;
        const int dash = c.indexOf(QLatin1Char('-'));
        itu = dash > 0 ? c.mid(1, dash - 1) : c.mid(1);
        c = dash > 0 ? c.mid(dash + 1) : QString();
    }
    QString name = code(QStringLiteral("eibi"), QStringLiteral("site"), itu + QLatin1Char('/') + c);
    if (name.isEmpty())
        name = c;
    if (relayed)
    {
        const QString country = countryName(itu);
        return name.isEmpty() ? country : QStringLiteral("%1 (%2)").arg(name, country);
    }
    return name;
}

QString StationDb::stationOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("broadcaster"), e.station);
        return name.isEmpty() ? e.station : name;
    }
    return e.station;
}

QString StationDb::languageOf(const StationEntry& e, bool details) const
{
    QString name;
    if (e.source == QLatin1String("eibi"))
        name = languageText(e.lang, details);
    else if (e.source == QLatin1String("hfcc"))
        name = code(e.source, QStringLiteral("lang"), e.lang);
    return name.isEmpty() ? e.lang : name;
}

QString StationDb::countryOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("admin"), e.itu);
        if (!name.isEmpty())
            return name;
    }
    return countryName(e.itu);
}

QString StationDb::siteOf(const StationEntry& e) const
{
    if (e.source == QLatin1String("eibi"))
        return siteName(e.itu, e.site);
    if (e.source == QLatin1String("hfcc"))
    {
        const QString name = code(e.source, QStringLiteral("site"), e.site);
        if (name.isEmpty())
            return e.site;
        // a site abroad says where it is
        const QString adm = code(e.source, QStringLiteral("siteadm"), e.site);
        if (adm.isEmpty() || adm == e.itu)
            return name;
        const QString admName = code(e.source, QStringLiteral("admin"), adm);
        return QStringLiteral("%1 (%2)").arg(name, admName.isEmpty() ? adm : admName);
    }
    return e.site;
}

QString StationDb::targetOf(const StationEntry& e, bool details) const
{
    QString name;
    if (e.source == QLatin1String("eibi"))
        name = targetText(e.target, details);
    else if (e.source == QLatin1String("hfcc"))
    {
        name = Ciraf::text(e.target, details);
        if (details && !name.isEmpty())
            name = QStringLiteral("CIRAF %1").arg(name);
    }
    return name.isEmpty() ? e.target : name;
}

QString StationDb::modeOf(const StationEntry& e)
{
    if (e.source == QLatin1String("eibi"))
        return EibiParser::guessMode(e);
    if (e.source == QLatin1String("hfcc"))
    {
        // HFCC's modulation letters
        if (e.mode == QLatin1String("D"))
            return QStringLiteral("AM");    // double sideband
        if (e.mode == QLatin1String("N"))
            return QStringLiteral("DRM");   // digital
        return QString();
    }
    if (e.source == QLatin1String("aoki"))
    {
        const QString remarks = e.remarks.toUpper();
        if (e.lang.contains(QLatin1String("(Digital)")) || e.station.contains(QLatin1String("DRM"))
            || remarks.contains(QLatin1String("DRM")))
            return QStringLiteral("DRM");
        if (remarks.contains(QLatin1String("USB")))
            return QStringLiteral("USB");
        if (remarks.contains(QLatin1String("LSB")))
            return QStringLiteral("LSB");
        if (remarks.contains(QLatin1String("CW")) || e.lang == QLatin1String("A1A"))
            return QStringLiteral("CW");
        return QStringLiteral("AM");
    }
    return e.mode;   // the personal list and other imports: as given
}

QString StationDb::remarksOf(const StationEntry& e)
{
    if (e.source != QLatin1String("hfcc") && e.source != QLatin1String("aoki"))
        return e.remarks;
    QStringList parts;
    if (!e.power.isEmpty())
        parts << e.power + QStringLiteral(" kW");
    if (!e.azimuth.isEmpty() && e.azimuth != QLatin1String("0") && e.azimuth != QLatin1String("ND"))
        parts << QStringLiteral("az %1°").arg(e.azimuth);
    if (e.source == QLatin1String("hfcc") && !e.mode.isEmpty() && modeOf(e).isEmpty())
        parts << QStringLiteral("mod %1").arg(e.mode);   // a letter without a mode of ours
    if (!e.remarks.isEmpty())
        parts << e.remarks;
    if (e.flag == QLatin1String("*"))
        parts << QStringLiteral("*");
    return parts.join(QStringLiteral(", "));
}

QString StationDb::weekdays(const StationEntry& e) const
{
    if (e.source != QLatin1String("aoki"))
        return e.days;
    // Aoki's days column: digits in its own numbering, day 1 Sunday unless
    // the list said Monday; as Monday-first digits, like EiBi's
    const bool sundayFirst = code(e.source, QStringLiteral("days"), QStringLiteral("1")) != QLatin1String("Monday");
    QList<int> days;
    for (const QChar c : e.days)
        if (c.isDigit() && c != QLatin1Char('0') && c <= QLatin1Char('7'))
        {
            int d = c.digitValue();
            if (sundayFirst)
                d = d == 1 ? 7 : d - 1;
            if (!days.contains(d))
                days << d;
        }
    std::sort(days.begin(), days.end());
    QString out;
    for (int d : days)
        out += QString::number(d);
    return out;
}

QString StationDb::daysOf(const StationEntry& e) const
{
    const QString days = weekdays(e);
    return days == QLatin1String("1234567") ? QString() : days;   // every day
}

bool StationDb::replaceAllocations(const QString& source, const QList<Allocation>& rows)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM allocations WHERE source = ?"));
    del.addBindValue(source);
    if (!del.exec())
    {
        m_lastError = del.lastError().text();
        db.rollback();
        return false;
    }
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT INTO allocations (source, low_khz, high_khz, service, usage, info,"
                               " mode, emission, comment) VALUES (?,?,?,?,?,?,?,?,?)"));
    for (const Allocation& a : rows)
    {
        ins.addBindValue(source);
        ins.addBindValue(a.lowKHz);
        ins.addBindValue(a.highKHz);
        ins.addBindValue(a.service);
        ins.addBindValue(a.usage);
        ins.addBindValue(a.info);
        ins.addBindValue(a.mode);
        ins.addBindValue(a.emission);
        ins.addBindValue(a.comment);
        if (!ins.exec())
        {
            m_lastError = ins.lastError().text();
            db.rollback();
            return false;
        }
    }
    if (!db.commit())
    {
        m_lastError = db.lastError().text();
        return false;
    }
    return true;
}

int StationDb::allocationCount(const QString& source) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM allocations WHERE source = ?"));
    q.addBindValue(source);
    if (q.exec() && q.next())
        return q.value(0).toInt();
    return 0;
}

QList<StationDb::Allocation> StationDb::allocationsAt(double kHz) const
{
    QList<Allocation> out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT low_khz, high_khz, service, usage, info, mode, emission, comment"
                             " FROM allocations WHERE low_khz <= ? AND high_khz >= ?"
                             " ORDER BY (high_khz - low_khz), low_khz"));
    q.addBindValue(kHz);
    q.addBindValue(kHz);
    if (!q.exec())
        return out;
    while (q.next())
    {
        Allocation a;
        a.lowKHz = q.value(0).toDouble();
        a.highKHz = q.value(1).toDouble();
        a.service = q.value(2).toString();
        a.usage = q.value(3).toString();
        a.info = q.value(4).toString();
        a.mode = q.value(5).toString();
        a.emission = q.value(6).toString();
        a.comment = q.value(7).toString();
        out.push_back(a);
    }
    return out;
}
