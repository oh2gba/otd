// SPDX-License-Identifier: GPL-3.0-or-later
#include "StationDb.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

static const char* kSelectColumns =
    "source, khz, start_min, end_min, days, itu, station, lang, target, site,"
    " persistence, start_date, stop_date, last_heard, remarks, lang_text, site_text, mode, id";

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
    if (!createSchema())
        return false;
    loadCodes();

    // When the import format changes, forget the "unchanged on server"
    // bookkeeping so every source is re-imported at the next update.
    static const QString kDataVersion = QStringLiteral("2");
    if (meta(QStringLiteral("schema.dataVersion")) != kDataVersion)
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("DELETE FROM meta WHERE key LIKE '%.lastModified' OR key LIKE '%.updated'"));
        setMeta(QStringLiteral("schema.dataVersion"), kDataVersion);
    }
    return true;
}

bool StationDb::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS stations ("
                       " id INTEGER PRIMARY KEY,"
                       " source TEXT NOT NULL,"
                       " khz REAL NOT NULL,"
                       " start_min INTEGER NOT NULL,"
                       " end_min INTEGER NOT NULL,"
                       " days TEXT, itu TEXT, station TEXT, lang TEXT, target TEXT, site TEXT,"
                       " persistence INTEGER, start_date TEXT, stop_date TEXT, last_heard TEXT,"
                       " remarks TEXT, lang_text TEXT, site_text TEXT, mode TEXT)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_khz ON stations(khz)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_stations_source ON stations(source)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS codes ("
                       " kind TEXT NOT NULL, code TEXT NOT NULL, name TEXT,"
                       " PRIMARY KEY (kind, code))"),
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

    // Migration for databases created before lang_text/site_text existed.
    QStringList columns;
    QSqlQuery info(db);
    if (info.exec(QStringLiteral("PRAGMA table_info(stations)")))
        while (info.next())
            columns << info.value(1).toString();
    for (const QString& col : {QStringLiteral("lang_text"), QStringLiteral("site_text"), QStringLiteral("mode")})
    {
        if (columns.contains(col))
            continue;
        QSqlQuery alter(db);
        if (!alter.exec(QStringLiteral("ALTER TABLE stations ADD COLUMN %1 TEXT").arg(col)))
        {
            m_lastError = alter.lastError().text();
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
    ins.prepare(QStringLiteral(
        "INSERT INTO stations (source, khz, start_min, end_min, days, itu, station, lang,"
        " target, site, persistence, start_date, stop_date, last_heard, remarks,"
        " lang_text, site_text, mode)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const StationEntry& e : entries)
    {
        ins.addBindValue(source);
        ins.addBindValue(e.kHz);
        ins.addBindValue(e.startMin);
        ins.addBindValue(e.endMin);
        ins.addBindValue(e.days);
        ins.addBindValue(e.itu);
        ins.addBindValue(e.station);
        ins.addBindValue(e.lang);
        ins.addBindValue(e.target);
        ins.addBindValue(e.site);
        ins.addBindValue(e.persistence);
        ins.addBindValue(e.startDate);
        ins.addBindValue(e.stopDate);
        ins.addBindValue(e.lastHeard);
        ins.addBindValue(e.remarks);
        ins.addBindValue(e.langText);
        ins.addBindValue(e.siteText);
        ins.addBindValue(e.mode);
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
    emit changed();
    return true;
}

namespace
{
void bindEntry(QSqlQuery& q, const StationEntry& e)
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
    q.addBindValue(e.langText);
    q.addBindValue(e.siteText);
    q.addBindValue(e.mode);
}
} // namespace

bool StationDb::insertEntry(StationEntry& e)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO stations (source, khz, start_min, end_min, days, itu, station, lang,"
        " target, site, persistence, start_date, stop_date, last_heard, remarks,"
        " lang_text, site_text, mode) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    bindEntry(q, e);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
    e.id = q.lastInsertId().toLongLong();
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
        " remarks=?, lang_text=?, site_text=?, mode=? WHERE id=?"));
    bindEntry(q, e);
    q.addBindValue(e.id);
    if (!q.exec())
    {
        m_lastError = q.lastError().text();
        return false;
    }
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
    e.langText = q.value(15).toString();
    e.siteText = q.value(16).toString();
    e.mode = q.value(17).toString();
    e.id = q.value(18).toLongLong();
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

StationList StationDb::search(const QString& text, const QStringList& sources, int limit) const
{
    StationList out;
    // every word must be found somewhere in the entry ("bbc english" gives
    // the BBC's English programmes), the frequency written as a number
    // included ("4625"); a leading "!" turns a word into an exclusion
    // ("!china english": English, but not from China)
    QStringList words;
    for (const QString& w : text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts))
        if (w != QLatin1String("!"))
            words << w;
    if (words.isEmpty())
        return out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    QString sql = QStringLiteral("SELECT %1 FROM stations WHERE 1=1").arg(QLatin1String(kSelectColumns));
    for (const QString& w : words)
        // IFNULL: a NULL column would make a negated group NULL, i.e. false
        sql += QStringLiteral(" AND %1(IFNULL(station,'') LIKE ? ESCAPE '\\'"
                              " OR IFNULL(lang_text,'') LIKE ? ESCAPE '\\'"
                              " OR IFNULL(site_text,'') LIKE ? ESCAPE '\\'"
                              " OR IFNULL(remarks,'') LIKE ? ESCAPE '\\'"
                              // the frequency as written in the list: all three
                              // decimals, trailing zeros and point dropped
                              " OR rtrim(rtrim(printf('%.3f', khz), '0'), '.') LIKE ? ESCAPE '\\'"
                              " OR IFNULL(itu,'') = ? OR IFNULL(itu,'') IN (SELECT code FROM codes"
                              " WHERE kind='country' AND name LIKE ? ESCAPE '\\'))")
                   .arg(w.startsWith(QLatin1Char('!')) ? QStringLiteral("NOT ") : QString());
    if (!sources.isEmpty())
    {
        QStringList marks;
        for (int i = 0; i < sources.size(); ++i)
            marks << QStringLiteral("?");
        sql += QStringLiteral(" AND source NOT IN (%1)").arg(marks.join(QLatin1Char(',')));
    }
    sql += QStringLiteral(" ORDER BY khz, start_min");
    if (limit > 0)
        sql += QStringLiteral(" LIMIT ?");
    q.prepare(sql);
    for (QString word : words)
    {
        if (word.startsWith(QLatin1Char('!')))
            word.remove(0, 1);
        QString pattern = word;
        pattern.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
               .replace(QLatin1Char('%'), QLatin1String("\\%"))
               .replace(QLatin1Char('_'), QLatin1String("\\_"));
        const QString like = QLatin1Char('%') + pattern + QLatin1Char('%');
        for (int i = 0; i < 5; ++i)
            q.addBindValue(like);
        q.addBindValue(word.toUpper());
        q.addBindValue(like);
    }
    for (const QString& src : sources)
        q.addBindValue(src);
    if (limit > 0)
        q.addBindValue(limit);
    if (q.exec())
        while (q.next())
            out.push_back(fromRecord(q));
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

bool StationDb::storeCodes(const EibiParser::CodeTables& tables)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    if (!db.transaction())
        return false;
    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM codes"));
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO codes (kind, code, name) VALUES (?,?,?)"));
    auto insertAll = [&](const QString& kind, const QHash<QString, QString>& map) {
        for (auto it = map.cbegin(); it != map.cend(); ++it)
        {
            q.addBindValue(kind);
            q.addBindValue(it.key());
            q.addBindValue(it.value());
            if (!q.exec())
                return false;
        }
        return true;
    };
    const bool ok = insertAll(QStringLiteral("lang"), tables.languages)
                 && insertAll(QStringLiteral("country"), tables.countries)
                 && insertAll(QStringLiteral("target"), tables.targets)
                 && insertAll(QStringLiteral("site"), tables.sites);
    if (!ok)
    {
        m_lastError = q.lastError().text();
        db.rollback();
        return false;
    }
    db.commit();
    m_codes = tables;
    return true;
}

void StationDb::loadCodes()
{
    m_codes = EibiParser::CodeTables();
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT kind, code, name FROM codes")))
        return;
    while (q.next())
    {
        const QString kind = q.value(0).toString();
        const QString code = q.value(1).toString();
        const QString name = q.value(2).toString();
        if (kind == QLatin1String("lang"))         m_codes.languages.insert(code, name);
        else if (kind == QLatin1String("country")) m_codes.countries.insert(code, name);
        else if (kind == QLatin1String("target"))  m_codes.targets.insert(code, name);
        else if (kind == QLatin1String("site"))    m_codes.sites.insert(code, name);
    }
}

QString StationDb::languageName(const QString& code) const
{
    return m_codes.languages.value(code, code);
}

QString StationDb::countryName(const QString& code) const
{
    return m_codes.countries.value(code, code);
}

QString StationDb::targetName(const QString& code) const
{
    if (m_codes.targets.contains(code))
        return m_codes.targets.value(code);
    return m_codes.countries.value(code, code);
}

QString StationDb::siteName(const QString& homeItu, const QString& siteCode) const
{
    QString itu = homeItu;
    QString code = siteCode;
    bool relayed = false;
    if (code.startsWith(QLatin1Char('/')))
    {
        relayed = true;
        const int dash = code.indexOf(QLatin1Char('-'));
        itu = dash > 0 ? code.mid(1, dash - 1) : code.mid(1);
        code = dash > 0 ? code.mid(dash + 1) : QString();
    }
    const QString key = itu + QLatin1Char('/') + code;
    QString name = m_codes.sites.value(key);
    if (name.isEmpty())
        name = code;
    if (relayed)
    {
        const QString country = countryName(itu);
        return name.isEmpty() ? country : QStringLiteral("%1 (%2)").arg(name, country);
    }
    return name;
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
